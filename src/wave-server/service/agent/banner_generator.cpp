#include "banner_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../../core/json.h"
#include "../../core/logger.h"
#include "util/time_util.h"
#include "agent_client.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    // Top-1 active habit per requested type, in the given type order — e.g. for
    // {"sleep","power","lifestyle"} this returns at most 3 rows, one per category,
    // each the highest-confidence active habit of that type. A type with no active
    // habit is simply absent from the result (never padded/faked).
    json fetch_active_habits(const db::DbClientPtr& client, int64_t user_id, const std::vector<std::string>& types)
    {
        json habits = json::array();
        for (const auto& habit_type : types)
        {
            const auto rows = client->execSqlSync(
                "SELECT habit_type, title, description, confidence FROM user_habit"
                " WHERE user_id = ? AND status = 'active' AND habit_type = ?"
                " ORDER BY confidence DESC, updated_at DESC LIMIT 1",
                user_id,
                habit_type);
            if (rows.empty())
                continue;
            habits.push_back(json{
                {"habitType", rows[0]["habit_type"].as<std::string>()},
                {"title", rows[0]["title"].as<std::string>()},
                {"description", rows[0]["description"].as<std::string>()},
                {"confidence", rows[0]["confidence"].as<double>()},
            });
        }
        return habits;
    }

    // Deterministic weekly sleep + power + appliance-control stats, gathered
    // over the 7 real days ending `for_date` (inclusive) — explicitly a 7-day
    // window, not daily_user_model's 14-day rolling window, so this matches
    // "1주일 데이터" literally. Empty object (not partial) only when there's
    // truly nothing in any of the three domains for this user this week.
    json fetch_weekly_dashboard_metrics(const db::DbClientPtr& client, int64_t user_id, const std::string& for_date)
    {
        json metrics = json::object();

        // Sleep: avg bedtime/wake/duration over the trailing 7 days, same
        // midnight-wrap-safe shift UserModelManager's rollup query uses.
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
      AND night_date >= date(?, '-6 day')
      AND night_date <= ?
)
SELECT
    COUNT(*) AS sample_days,
    CAST(ROUND(AVG(onset_shifted)) AS INTEGER) % 1440 AS avg_bedtime_minute,
    CAST(ROUND(AVG(wake_shifted)) AS INTEGER) % 1440 AS avg_wake_minute,
    AVG(asleep_total_s) / 60.0 AS sleep_duration_avg_minutes
FROM shifted
)SQL",
            user_id, for_date, for_date);

        if (!sleep_rows.empty() && sleep_rows[0]["sample_days"].as<int>() > 0
            && !sleep_rows[0]["avg_bedtime_minute"].isNull())
        {
            const auto bedtime_minute = sleep_rows[0]["avg_bedtime_minute"].as<int64_t>();
            const auto wake_minute = sleep_rows[0]["avg_wake_minute"].as<int64_t>();
            const auto format_hhmm = [](int64_t minute_of_day) {
                char buf[6];
                std::snprintf(buf, sizeof(buf), "%02lld:%02lld",
                    static_cast<long long>(minute_of_day / 60), static_cast<long long>(minute_of_day % 60));
                return std::string(buf);
            };

            json sleep;
            sleep["sampleDays"] = sleep_rows[0]["sample_days"].as<int>();
            sleep["avgBedtime"] = format_hhmm(bedtime_minute);
            sleep["avgWake"] = format_hhmm(wake_minute);
            sleep["avgDurationMinutes"] =
                std::round(sleep_rows[0]["sleep_duration_avg_minutes"].as<double>() * 10.0) / 10.0;
            metrics["sleep"] = std::move(sleep);
        }

        // Power: total kWh over the trailing 7 days (same 5m-bucket aggregation
        // shape power_store.cpp's 1w report branch uses).
        const auto power_rows = client->execSqlSync(
            R"SQL(
SELECT COALESCE(SUM(energy_wh), 0) AS energy_wh, COUNT(*) AS buckets
FROM power_energy
WHERE device_id IS NULL AND granularity = '5m'
  AND time_start >= datetime(?, '-6 day') AND time_start < datetime(?, '+1 day')
)SQL",
            for_date, for_date);

        if (!power_rows.empty() && power_rows[0]["buckets"].as<int64_t>() > 0)
        {
            json power;
            power["totalKwh"] = std::round(power_rows[0]["energy_wh"].as<double>() / 100.0) / 10.0;
            metrics["power"] = std::move(power);
        }

        // Appliance control: how many device-execution events happened this
        // week, grounding the "가전 제어" clause of the summary.
        const auto appliance_rows = client->execSqlSync(
            R"SQL(
SELECT COUNT(*) AS execution_count
FROM home_event
WHERE user_id = ? AND type = 'execution'
  AND substr(occurred_at, 1, 10) >= date(?, '-6 day')
  AND substr(occurred_at, 1, 10) <= ?
)SQL",
            user_id, for_date, for_date);

        if (!appliance_rows.empty() && appliance_rows[0]["execution_count"].as<int64_t>() > 0)
        {
            json appliance;
            appliance["executionCount"] = appliance_rows[0]["execution_count"].as<int64_t>();
            metrics["appliance"] = std::move(appliance);
        }

        return metrics;
    }
}

bool generateAndPersistDashboardBanner(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& for_date,
    std::string& out_error)
{
    if (!client)
    {
        out_error = "no db client";
        return false;
    }

    const auto metrics = fetch_weekly_dashboard_metrics(client, user_id, for_date);
    if (metrics.empty())
        return true; // nothing to summarize yet — dashboardDailyMessage's existing fallback covers this

    json body;
    body["userId"] = user_id;
    body["date"] = for_date;
    body["metrics"] = metrics;

    AgentBannerJobResult result;
    if (runDashboardSummaryJobSync(agent_base_url, body, result, out_error) != AgentClientResult::success)
    {
        WLOG_WARN("dashboard summary job failed (user {}, date {}): {}", user_id, for_date, out_error);
        return false;
    }
    if (result.headline.empty() || result.body.empty())
        return true; // agent had nothing usable — leave prior banner/fallback in place

    try
    {
        const auto now = formatTimestamp();

        client->execSqlSync(
            "DELETE FROM insight WHERE user_id = ? AND surface = 'dashboard_banner' AND kind = 'banner' AND date = ?",
            user_id,
            for_date);

        const auto id_rows = client->execSqlSync("SELECT COALESCE(MAX(id), 0) AS max_id FROM insight");
        const int64_t next_id = (id_rows.empty() ? 0 : id_rows[0]["max_id"].as<int64_t>()) + 1;

        client->execSqlSync(
            R"SQL(
INSERT INTO insight (
    id, user_id, surface, kind, date, label, title, text,
    actionable, approved, created_at
) VALUES (?, ?, 'dashboard_banner', 'banner', ?, '안내', ?, ?, 0, 0, ?)
)SQL",
            next_id, user_id, for_date, result.headline, result.body, now);
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        WLOG_WARN("dashboard banner persist failed (user {}, date {}): {}", user_id, for_date, out_error);
        return false;
    }

    return true;
}

bool generateAndPersistWeeklyPlanBanner(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& period_start,
    std::string& out_error)
{
    if (!client)
    {
        out_error = "no db client";
        return false;
    }

    // All habit types combined (sleep+power+lifestyle) — this is the same
    // all-types combination the dashboard banner used to show before it moved
    // to a deterministic weekly-stats summary; the routine planner is now
    // where that habit-based narrative lives.
    const auto habits = fetch_active_habits(client, user_id, {"sleep", "power", "lifestyle"});
    if (habits.empty())
        return true; // weeklyReport's existing fallbacks cover this

    json body;
    body["userId"] = user_id;
    body["date"] = period_start;
    body["surface"] = "weekly_plan";
    body["habits"] = habits;

    AgentBannerJobResult result;
    if (runBannerJobSync(agent_base_url, body, result, out_error) != AgentClientResult::success)
    {
        WLOG_WARN("weekly plan banner job failed (user {}, period {}): {}", user_id, period_start, out_error);
        return false;
    }
    if (result.headline.empty() || result.body.empty())
        return true;

    try
    {
        const auto now = formatTimestamp();
        client->execSqlSync(
            R"SQL(
INSERT INTO weekly_plan_report (user_id, period_start, headline, report_text, created_at)
VALUES (?, ?, ?, ?, ?)
ON CONFLICT(user_id, period_start) DO UPDATE SET
    headline = excluded.headline, report_text = excluded.report_text, created_at = excluded.created_at
)SQL",
            user_id, period_start, result.headline, result.body, now);
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        WLOG_WARN("weekly plan banner persist failed (user {}, period {}): {}", user_id, period_start, out_error);
        return false;
    }

    return true;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
