#include "user_model_manager.h"

#include <algorithm>
#include <optional>

#include "../app/app_state.h"
#include "../core/json.h"
#include "../core/logger.h"
#include "agent/banner_generator.h"
#include "agent/habit_event_catalog.h"
#include "agent/habit_generator.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

UserModelManager& UserModelManager::get()
{
    static UserModelManager instance;
    return instance;
}

UserModelManager::~UserModelManager()
{
    stop();
}

void UserModelManager::start()
{
    if (m_running.exchange(true))
        return;

    m_worker = std::thread([this]()
    {
        while (m_running.load(std::memory_order_acquire))
        {
            if (!AppState::get().no_devices
                && AppState::get().running.load(std::memory_order_acquire))
            {
                auto client = AppState::get().db();
                if (client)
                {
                    try
                    {
                        const auto rows = client->execSqlSync("SELECT date('now', 'localtime') AS today");
                        if (!rows.empty())
                        {
                            const std::string today = rows[0]["today"].as<std::string>();
                            std::string previous;
                            {
                                std::lock_guard lock(m_mutex);
                                previous = m_lastModelDate;
                            }

                            if (today != previous)
                            {
                                // First tick of a new calendar day — roll up the day that just ended.
                                if (!previous.empty())
                                {
                                    const auto y_rows = client->execSqlSync("SELECT date(?, '-1 day') AS d", today);
                                    const std::string yesterday =
                                        y_rows.empty() ? today : y_rows[0]["d"].as<std::string>();
                                    runRolloverFor(yesterday);
                                }
                                std::lock_guard lock(m_mutex);
                                m_lastModelDate = today;
                            }
                        }
                    }
                    catch (const std::exception& e)
                    {
                        WLOG_WARN("UserModelManager: rollover check failed: {}", e.what());
                    }
                }
            }

            // Day-rollover detection doesn't need 1s precision (unlike PowerManager's
            // plug sampling) — a 1-minute tick is plenty.
            std::unique_lock lock(m_stopMutex);
            m_stopCv.wait_for(lock, std::chrono::minutes(1), [this]()
            {
                return !m_running.load(std::memory_order_acquire);
            });
        }
    });
}

void UserModelManager::stop()
{
    if (!m_running.exchange(false))
        return;

    m_stopCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void UserModelManager::computeNow(const std::string& for_date)
{
    runRolloverFor(for_date);
}

void UserModelManager::runRolloverFor(const std::string& for_date)
{
    // Order matters: daily_user_model (Understand) first, then habit
    // confidence refresh/expiry (cheap, pure SQL), then LLM-backed new-habit
    // discovery (Predict) last — discovery excludes event keys already
    // covered by an active habit, so it should see the freshly-refreshed set.
    // Banner synthesis (Act) runs last of all so it reflects today's freshly
    // discovered/reinforced/expired habits, not yesterday's set.
    computeAndStoreUserModelForAllUsers(for_date);
    refreshHabitsForAllUsers(for_date);
    discoverHabitsForAllUsers(for_date);
    synthesizeBannersForAllUsers(for_date);
}

void UserModelManager::computeAndStoreUserModelForAllUsers(const std::string& for_date)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        const auto users = client->execSqlSync("SELECT id FROM user");
        for (const auto& row : users)
            computeAndStoreUserModelForUser(row["id"].as<int64_t>(), for_date);
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("UserModelManager: user list query failed: {}", e.what());
    }
}

void UserModelManager::computeAndStoreUserModelForUser(int64_t user_id, const std::string& for_date)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        const std::string window_start_modifier =
            "-" + std::to_string(std::max(0, kWindowDays - 1)) + " day";

        // Shift any clock time before 12:00 by +1440 before averaging so a
        // 23:40 bedtime and a 00:15 bedtime land on the same continuous scale
        // instead of averaging toward noon, then mod back to 0-1439. Real
        // bedtimes/wake times never fall in the 12:00-18:00 band, so this
        // simple shift is safe (a true circular mean isn't needed here).
        const auto sleep_rows = client->execSqlSync(
            R"SQL(
WITH shifted AS (
    SELECT
        asleep_total_s,
        CASE WHEN (CAST(strftime('%H', onset) AS INTEGER) * 60 + CAST(strftime('%M', onset) AS INTEGER)) < 720
             THEN (CAST(strftime('%H', onset) AS INTEGER) * 60 + CAST(strftime('%M', onset) AS INTEGER)) + 1440
             ELSE (CAST(strftime('%H', onset) AS INTEGER) * 60 + CAST(strftime('%M', onset) AS INTEGER))
        END AS onset_shifted,
        CASE WHEN (CAST(strftime('%H', final_wake) AS INTEGER) * 60 + CAST(strftime('%M', final_wake) AS INTEGER)) < 720
             THEN (CAST(strftime('%H', final_wake) AS INTEGER) * 60 + CAST(strftime('%M', final_wake) AS INTEGER)) + 1440
             ELSE (CAST(strftime('%H', final_wake) AS INTEGER) * 60 + CAST(strftime('%M', final_wake) AS INTEGER))
        END AS wake_shifted
    FROM sleep_session
    WHERE user_id = ?
      AND onset IS NOT NULL AND final_wake IS NOT NULL
      AND night_date >= date(?, ?)
      AND night_date <= ?
)
SELECT
    COUNT(*) AS sample_days,
    CAST(ROUND(AVG(onset_shifted)) AS INTEGER) % 1440 AS avg_bedtime_minute,
    CAST(ROUND(AVG(wake_shifted)) AS INTEGER) % 1440 AS avg_wake_minute,
    AVG(asleep_total_s) / 60.0 AS sleep_duration_avg_minutes
FROM shifted
)SQL",
            user_id, for_date, window_start_modifier, for_date);

        int sample_days = 0;
        std::optional<int64_t> avg_bedtime;
        std::optional<int64_t> avg_wake;
        std::optional<double> avg_duration;
        if (!sleep_rows.empty())
        {
            sample_days = sleep_rows[0]["sample_days"].as<int>();
            if (sample_days > 0 && !sleep_rows[0]["avg_bedtime_minute"].isNull())
            {
                avg_bedtime = sleep_rows[0]["avg_bedtime_minute"].as<int64_t>();
                avg_wake = sleep_rows[0]["avg_wake_minute"].as<int64_t>();
                avg_duration = sleep_rows[0]["sleep_duration_avg_minutes"].as<double>();
            }
        }

        const auto light_rows = client->execSqlSync(
            R"SQL(
SELECT AVG(CAST(json_extract(detail_json, '$.params.value') AS REAL)) AS avg_brightness
FROM home_event he
JOIN device d ON d.id = he.device_id
WHERE he.user_id = ?
  AND he.type = 'execution'
  AND d.class LIKE 'philips_wiz_e29%'
  AND json_extract(he.detail_json, '$.action') = 'brightness'
  AND substr(he.occurred_at, 1, 10) >= date(?, ?)
  AND substr(he.occurred_at, 1, 10) <= ?
)SQL",
            user_id, for_date, window_start_modifier, for_date);

        std::optional<double> avg_brightness;
        if (!light_rows.empty() && !light_rows[0]["avg_brightness"].isNull())
            avg_brightness = light_rows[0]["avg_brightness"].as<double>();

        const auto now_rows = client->execSqlSync("SELECT datetime('now', 'localtime') AS now");
        const std::string computed_at = now_rows.empty() ? for_date : now_rows[0]["now"].as<std::string>();

        // Nullable numeric binds — this codebase's execSqlSync doesn't bind
        // std::optional directly (see PowerManager::flushBucket), so branch
        // on which values are present rather than trying to pass optionals
        // straight through. Sleep fields are always null/non-null together
        // (same query, gated by sample_days); brightness is independent.
        if (avg_bedtime && avg_brightness)
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO daily_user_model
    (user_id, model_date, window_days, avg_bedtime_minute, avg_wake_minute,
     sleep_duration_avg_minutes, preferred_light_brightness, sample_days, computed_at)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(user_id, model_date) DO UPDATE SET
    window_days = excluded.window_days, avg_bedtime_minute = excluded.avg_bedtime_minute,
    avg_wake_minute = excluded.avg_wake_minute, sleep_duration_avg_minutes = excluded.sleep_duration_avg_minutes,
    preferred_light_brightness = excluded.preferred_light_brightness, sample_days = excluded.sample_days,
    computed_at = excluded.computed_at
)SQL",
                user_id, for_date, kWindowDays, *avg_bedtime, *avg_wake, *avg_duration,
                *avg_brightness, sample_days, computed_at);
        }
        else if (avg_bedtime)
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO daily_user_model
    (user_id, model_date, window_days, avg_bedtime_minute, avg_wake_minute,
     sleep_duration_avg_minutes, preferred_light_brightness, sample_days, computed_at)
VALUES (?, ?, ?, ?, ?, ?, NULL, ?, ?)
ON CONFLICT(user_id, model_date) DO UPDATE SET
    window_days = excluded.window_days, avg_bedtime_minute = excluded.avg_bedtime_minute,
    avg_wake_minute = excluded.avg_wake_minute, sleep_duration_avg_minutes = excluded.sleep_duration_avg_minutes,
    preferred_light_brightness = excluded.preferred_light_brightness, sample_days = excluded.sample_days,
    computed_at = excluded.computed_at
)SQL",
                user_id, for_date, kWindowDays, *avg_bedtime, *avg_wake, *avg_duration,
                sample_days, computed_at);
        }
        else if (avg_brightness)
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO daily_user_model
    (user_id, model_date, window_days, avg_bedtime_minute, avg_wake_minute,
     sleep_duration_avg_minutes, preferred_light_brightness, sample_days, computed_at)
VALUES (?, ?, ?, NULL, NULL, NULL, ?, ?, ?)
ON CONFLICT(user_id, model_date) DO UPDATE SET
    window_days = excluded.window_days, avg_bedtime_minute = excluded.avg_bedtime_minute,
    avg_wake_minute = excluded.avg_wake_minute, sleep_duration_avg_minutes = excluded.sleep_duration_avg_minutes,
    preferred_light_brightness = excluded.preferred_light_brightness, sample_days = excluded.sample_days,
    computed_at = excluded.computed_at
)SQL",
                user_id, for_date, kWindowDays, *avg_brightness, sample_days, computed_at);
        }
        else
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO daily_user_model
    (user_id, model_date, window_days, avg_bedtime_minute, avg_wake_minute,
     sleep_duration_avg_minutes, preferred_light_brightness, sample_days, computed_at)
VALUES (?, ?, ?, NULL, NULL, NULL, NULL, ?, ?)
ON CONFLICT(user_id, model_date) DO UPDATE SET
    window_days = excluded.window_days, avg_bedtime_minute = excluded.avg_bedtime_minute,
    avg_wake_minute = excluded.avg_wake_minute, sleep_duration_avg_minutes = excluded.sleep_duration_avg_minutes,
    preferred_light_brightness = excluded.preferred_light_brightness, sample_days = excluded.sample_days,
    computed_at = excluded.computed_at
)SQL",
                user_id, for_date, kWindowDays, sample_days, computed_at);
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("UserModelManager: daily_user_model compute failed for user {}: {}", user_id, e.what());
    }
}

void UserModelManager::refreshHabitsForAllUsers(const std::string& for_date)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        const auto users = client->execSqlSync("SELECT id FROM user");
        for (const auto& row : users)
            refreshHabitsForUser(row["id"].as<int64_t>(), for_date);
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("UserModelManager: habit refresh user list query failed: {}", e.what());
    }
}

void UserModelManager::refreshHabitsForUser(int64_t user_id, const std::string& for_date)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        const auto rows = client->execSqlSync(
            "SELECT id, evidence_json, window_days FROM user_habit WHERE user_id = ? AND status = 'active'",
            user_id);

        for (const auto& row : rows)
        {
            const int64_t habit_id = row["id"].as<int64_t>();
            const int window_days = row["window_days"].as<int>();

            std::string event;
            try
            {
                const auto evidence = json::parse(row["evidence_json"].as<std::string>());
                if (evidence.contains("event") && evidence["event"].is_string())
                    event = evidence["event"].get<std::string>();
            }
            catch (const std::exception&)
            {
                // Malformed evidence — treat as unverifiable (0 matches) below, which
                // naturally drives it toward expiry rather than leaving it stuck.
            }

            const auto recount = countEventOccurrences(client, user_id, event, window_days, for_date);
            const double confidence = window_days > 0
                ? std::clamp(static_cast<double>(recount.matchedDays) / window_days, 0.0, 1.0)
                : 0.0;
            const std::string status = confidence < kHabitExpireThreshold ? "expired" : "active";

            const auto now_rows = client->execSqlSync("SELECT datetime('now', 'localtime') AS now");
            const std::string now = now_rows.empty() ? for_date : now_rows[0]["now"].as<std::string>();

            if (recount.matchedDays > 0)
            {
                client->execSqlSync(
                    "UPDATE user_habit SET confidence = ?, status = ?, last_verified_at = ?, updated_at = ? WHERE id = ?",
                    confidence, status, now, now, habit_id);
            }
            else
            {
                client->execSqlSync(
                    "UPDATE user_habit SET confidence = ?, status = ?, updated_at = ? WHERE id = ?",
                    confidence, status, now, habit_id);
            }
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("UserModelManager: habit refresh failed for user {}: {}", user_id, e.what());
    }
}

void UserModelManager::discoverHabitsForAllUsers(const std::string& for_date)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    const auto agent_base_url = AppState::get().config.agent.base_url;

    try
    {
        const auto users = client->execSqlSync("SELECT id FROM user");
        for (const auto& row : users)
        {
            const int64_t user_id = row["id"].as<int64_t>();
            std::string error;
            if (!generateAndPersistHabitsForUser(client, agent_base_url, user_id, for_date, error))
                WLOG_WARN("UserModelManager: habit discovery failed for user {}: {}", user_id, error);
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("UserModelManager: habit discovery user list query failed: {}", e.what());
    }
}

void UserModelManager::synthesizeBannersForAllUsers(const std::string& for_date)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    const auto agent_base_url = AppState::get().config.agent.base_url;

    try
    {
        const auto users = client->execSqlSync("SELECT id FROM user");
        for (const auto& row : users)
        {
            const int64_t user_id = row["id"].as<int64_t>();
            std::string error;
            if (!generateAndPersistDashboardBanner(client, agent_base_url, user_id, for_date, error))
                WLOG_WARN("UserModelManager: dashboard banner synthesis failed for user {}: {}", user_id, error);

            // weekly_plan_report.period_start just needs to be a monotonically
            // increasing reference date that ORDER BY ... DESC LIMIT 1 naturally
            // picks up — reusing the same daily `for_date` as the sleep/habit
            // rollover keeps this on one simple cadence rather than needing a
            // separate Monday-of-week calculation.
            error.clear();
            if (!generateAndPersistWeeklyPlanBanner(client, agent_base_url, user_id, for_date, error))
                WLOG_WARN("UserModelManager: weekly plan banner synthesis failed for user {}: {}", user_id, error);
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("UserModelManager: banner synthesis user list query failed: {}", e.what());
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
