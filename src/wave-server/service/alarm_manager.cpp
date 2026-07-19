#include "alarm_manager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

#include <drogon/drogon.h>

#include "../app/app_state.h"
#include "../core/json.h"
#include "../core/logger.h"
#include "util/time_util.h"
#include "../device/device_wire_id.hpp"
#include "../web/http/v1/iot_store.h"
#include "action_queue.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr int k_smart_wake_window_min = 30;

    std::string weekday_token(int wday)
    {
        switch (wday)
        {
        case 1:
            return "mon";
        case 2:
            return "tue";
        case 3:
            return "wed";
        case 4:
            return "thu";
        case 5:
            return "fri";
        case 6:
            return "sat";
        case 0:
            return "sun";
        default:
            return "";
        }
    }

    Json::Value parseDaysJson(const std::string& raw)
    {
        Json::CharReaderBuilder reader;
        Json::Value value(Json::arrayValue);
        std::string errors;
        std::istringstream stream(raw.empty() ? "[]" : raw);
        Json::parseFromStream(reader, stream, &value, &errors);
        if (!value.isArray())
            return Json::Value(Json::arrayValue);
        return value;
    }

    void enqueue_action(const std::string& device_id, const std::string& action, const json& params, const std::string& source)
    {
        auto& app = AppState::get();
        if (!app.automationReady())
            return;

        ActionJob job;
        job.targetDeviceId = device_id;
        job.actionName = action;
        job.params = params.is_object() ? params : json::object();
        job.execMode = ExecMode::Once;
        job.sourceRef = source;
        job.logMessage = "알람 실행: " + action;
        app.actionQueue().enqueue(std::move(job));
    }

    bool is_wake_friendly_stage(const std::string& stage)
    {
        return stage == "light" || stage == "rem" || stage == "awake";
    }

    int minutes_before_target(int now_minute, int target_minute)
    {
        return (target_minute - now_minute + 1440) % 1440;
    }
}

AlarmManager& AlarmManager::get()
{
    static AlarmManager instance;
    return instance;
}

AlarmManager::~AlarmManager()
{
    stop();
}

void AlarmManager::start()
{
    if (m_running.exchange(true))
        return;

    reconcile();
    m_worker = std::thread([this]() { runLoop(); });
    WLOG_INFO("AlarmManager started");
}

void AlarmManager::stop()
{
    if (!m_running.exchange(false))
        return;

    m_stop_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();

    std::lock_guard lock(m_mutex);
    m_alarms.clear();
    m_runtime.clear();
}

void AlarmManager::reconcile()
{
    const auto loaded = loadEnabledAlarms();
    std::lock_guard lock(m_mutex);

    std::unordered_map<int64_t, RuntimeState> preserved;
    for (const auto& alarm : loaded)
    {
        const auto it = m_runtime.find(alarm.id);
        if (it != m_runtime.end())
            preserved.emplace(alarm.id, it->second);
    }

    m_alarms.clear();
    for (const auto& alarm : loaded)
        m_alarms.emplace(alarm.id, alarm);

    m_runtime = std::move(preserved);
    for (const auto& alarm : loaded)
        m_runtime.try_emplace(alarm.id);
}

std::vector<AlarmRecord> AlarmManager::loadEnabledAlarms()
{
    std::vector<AlarmRecord> out;
    const auto client = AppState::get().db();
    if (!client)
        return out;

    try
    {
        const auto rows = client->execSqlSync(
            R"SQL(
SELECT a.id, a.user_id, a.name, a.time_minute, a.days_of_week, a.smart_wake,
       a.device_id, a.radar_device_id, a.method, a.enabled,
       d.id AS device_row_id, d.name AS device_name,
       r.id AS radar_row_id, r.name AS radar_name
FROM alarm a
LEFT JOIN device d ON d.id = a.device_id
LEFT JOIN device r ON r.id = a.radar_device_id
WHERE a.enabled = 1
ORDER BY a.time_minute ASC
)SQL");

        for (const auto& row : rows)
        {
            AlarmRecord alarm;
            alarm.id = row["id"].as<int64_t>();
            alarm.user_id = row["user_id"].as<int64_t>();
            alarm.name = row["name"].as<std::string>();
            alarm.time_minute = row["time_minute"].as<int>();
            alarm.smart_wake = row["smart_wake"].as<int>() != 0;
            alarm.enabled = row["enabled"].as<int>() != 0;

            const Json::Value days = parseDaysJson(row["days_of_week"].as<std::string>());
            for (const auto& day : days)
            {
                if (day.isString())
                    alarm.days_of_week.push_back(day.asString());
            }

            if (!row["device_id"].isNull())
            {
                const int64_t device_id = row["device_row_id"].as<int64_t>();
                const std::string name = row["device_name"].as<std::string>();
                alarm.device_external_id = dev::wireIdForDbRow(device_id, name);
            }

            if (!row["radar_device_id"].isNull())
            {
                const int64_t radar_id = row["radar_row_id"].as<int64_t>();
                const std::string name = row["radar_name"].as<std::string>();
                alarm.radar_device_id = radar_id;
                alarm.radar_external_id = dev::wireIdForDbRow(radar_id, name);
            }

            Json::CharReaderBuilder reader;
            Json::Value method;
            std::string errors;
            std::istringstream stream(row["method"].as<std::string>());
            if (Json::parseFromStream(reader, stream, &method, &errors) && method.isObject())
                alarm.method = method;

            out.push_back(std::move(alarm));
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("AlarmManager load failed: {}", e.what());
    }

    return out;
}

std::string AlarmManager::todayDate() const
{
    const std::time_t now_t = std::time(nullptr);
    std::tm local_tm {};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_t);
#else
    localtime_r(&now_t, &local_tm);
#endif
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local_tm);
    return buffer;
}

std::string AlarmManager::nowStamp() const
{
    return formatTimestamp();
}

int AlarmManager::localNowMinute() const
{
    const std::time_t now_t = std::time(nullptr);
    std::tm local_tm {};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_t);
#else
    localtime_r(&now_t, &local_tm);
#endif
    return local_tm.tm_hour * 60 + local_tm.tm_min;
}

bool AlarmManager::isScheduledToday(const AlarmRecord& alarm) const
{
    if (alarm.days_of_week.empty())
        return true;

    const std::time_t now_t = std::time(nullptr);
    std::tm local_tm {};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_t);
#else
    localtime_r(&now_t, &local_tm);
#endif

    const std::string token = weekday_token(local_tm.tm_wday);
    return std::find(alarm.days_of_week.begin(), alarm.days_of_week.end(), token)
        != alarm.days_of_week.end();
}

bool AlarmManager::isInFireWindow(const AlarmRecord& alarm) const
{
    const int now_minute = localNowMinute();
    if (!alarm.smart_wake)
        return now_minute == alarm.time_minute;

    const int before = minutes_before_target(now_minute, alarm.time_minute);
    return before <= k_smart_wake_window_min;
}

bool AlarmManager::isLightWakeStage(const AlarmRecord& alarm) const
{
    if (alarm.radar_device_id <= 0)
        return false;

    const auto client = AppState::get().db();
    if (!client)
        return false;

    try
    {
        const auto cutoff = formatTimestamp(
            std::chrono::system_clock::now() - std::chrono::minutes(3));
        const auto rows = client->execSqlSync(
            R"SQL(
SELECT s.stage_label, s.toss_mean
FROM sleep_stat s
JOIN device_room_map drm ON drm.room_id = s.room_id
WHERE s.user_id = ?
  AND s.granularity = '1m'
  AND drm.device_id = ?
  AND s.time_start >= ?
ORDER BY s.time_start DESC
LIMIT 1
)SQL",
            alarm.user_id,
            alarm.radar_device_id,
            cutoff);

        if (rows.empty())
            return false;

        if (!rows[0]["stage_label"].isNull())
        {
            const auto stage = rows[0]["stage_label"].as<std::string>();
            if (is_wake_friendly_stage(stage))
                return true;
        }

        if (!rows[0]["toss_mean"].isNull())
        {
            const double toss = rows[0]["toss_mean"].as<double>();
            if (toss >= 0.5)
                return true;
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("AlarmManager smart-wake stage query failed: {}", e.what());
    }

    return false;
}

bool AlarmManager::shouldFireNow(const AlarmRecord& alarm) const
{
    if (!isScheduledToday(alarm) || !isInFireWindow(alarm))
        return false;

    if (!alarm.smart_wake)
        return true;

    const int now_minute = localNowMinute();
    if (now_minute == alarm.time_minute)
        return true;

    return isLightWakeStage(alarm);
}

void AlarmManager::markFired(AlarmRecord& alarm, const std::string& today)
{
    auto& state = m_runtime[alarm.id];
    state.last_fired_date = today;
    if (alarm.days_of_week.empty())
        state.once_fired = true;
}

void AlarmManager::disableOnceAlarm(int64_t alarm_id)
{
    const auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        client->execSqlSync(
            "UPDATE alarm SET enabled = 0, updated_at = ? WHERE id = ?",
            nowStamp(),
            alarm_id);
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("AlarmManager disable once alarm failed: {}", e.what());
    }
}

void AlarmManager::insertNotification(int64_t user_id, const std::string& message)
{
    const auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        auto rows = client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM notification");
        const int64_t next_id = rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();
        client->execSqlSync(
            "INSERT INTO notification (id, user_id, type, message, read, created_at) VALUES (?, ?, 'alarm', ?, 0, ?)",
            next_id,
            user_id,
            message,
            nowStamp());
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("AlarmManager notification insert failed: {}", e.what());
    }
}

void AlarmManager::executeMethod(const AlarmRecord& alarm)
{
    if (alarm.device_external_id.empty() || !alarm.method.isObject())
    {
        WLOG_WARN("Alarm {} has no device or method", alarm.id);
        return;
    }

    const std::string type = alarm.method.get("type", "").asString();
    const std::string source = "alarm:" + std::to_string(alarm.id);

    if (type == "light_on")
    {
        const int brightness = alarm.method.get("brightness", 70).asInt();
        enqueue_action(alarm.device_external_id, "on", json::object(), source);
        enqueue_action(
            alarm.device_external_id,
            "brightness",
            json{{"value", std::clamp(brightness, 10, 100)}},
            source);
        return;
    }

    if (type == "light_blink")
    {
        const int brightness = alarm.method.get("brightness", 70).asInt();
        const int interval_sec = std::clamp(alarm.method.get("intervalSec", 2).asInt(), 1, 10);
        const std::string device_id = alarm.device_external_id;

        enqueue_action(device_id, "on", json::object(), source);
        enqueue_action(
            device_id,
            "brightness",
            json{{"value", std::clamp(brightness, 10, 100)}},
            source);

        std::thread([device_id, interval_sec, source]()
        {
            std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
            enqueue_action(device_id, "off", json::object(), source);
        }).detach();
        return;
    }

    if (type == "plug_toggle")
    {
        enqueue_action(alarm.device_external_id, "toggle", json::object(), source);
        return;
    }
    if (type == "plug_on")
    {
        enqueue_action(alarm.device_external_id, "on", json::object(), source);
        return;
    }
    if (type == "plug_off")
    {
        enqueue_action(alarm.device_external_id, "off", json::object(), source);
        return;
    }

    if (type == "tts" || type == "sound")
    {
        std::string text = alarm.method.get("text", "").asString();
        if (text.empty() && type == "sound")
            text = alarm.method.get("soundId", "알람").asString();
        const int speaker_id = alarm.method.get("speakerId", 0).asInt();
        const int repeat_count = std::clamp(alarm.method.get("repeatCount", 1).asInt(), 1, 20);
        const int interval_sec = std::clamp(alarm.method.get("intervalSec", 5).asInt(), 1, 60);
        const std::string device_id = alarm.device_external_id;

        std::thread([device_id, text, speaker_id, repeat_count, interval_sec]()
        {
            auto& app = AppState::get();
            web::v1::IotStore store(app.deviceManager);
            for (int i = 0; i < repeat_count; ++i)
            {
                std::string code;
                store.sendDeviceTts(device_id, text, speaker_id, 1.0f, code);
                if (i + 1 < repeat_count)
                    std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
            }
        }).detach();
        return;
    }

    WLOG_WARN("Alarm {} unsupported method type: {}", alarm.id, type);
}

void AlarmManager::fireAlarm(const AlarmRecord& alarm, bool smart_early)
{
    if (alarm.smart_wake)
    {
        WLOG_INFO(
            "Alarm {} smart wake {} (radar={})",
            alarm.id,
            smart_early ? "early" : "deadline",
            alarm.radar_external_id.empty() ? "-" : alarm.radar_external_id);
    }

    executeMethod(alarm);
    insertNotification(alarm.user_id, "\"" + alarm.name + "\" 알람이 울렸습니다.");

    AppState::get().iot.logEvent(
        "execution",
        alarm.device_external_id,
        alarm.name,
        "알람 실행",
        "alarm:" + std::to_string(alarm.id));

    WLOG_INFO("Alarm fired: id={} name={}", alarm.id, alarm.name);
}

void AlarmManager::tick()
{
    if (!AppState::get().automationReady())
        return;

    const std::string today = todayDate();
    std::vector<std::pair<int64_t, bool>> due_ids;

    {
        std::lock_guard lock(m_mutex);
        for (auto& [id, alarm] : m_alarms)
        {
            if (!alarm.enabled)
                continue;

            auto& state = m_runtime[id];
            if (!shouldFireNow(alarm))
                continue;

            if (!alarm.days_of_week.empty() && state.last_fired_date == today)
                continue;
            if (alarm.days_of_week.empty() && state.once_fired)
                continue;

            const bool smart_early =
                alarm.smart_wake && localNowMinute() != alarm.time_minute;
            due_ids.emplace_back(id, smart_early);
            markFired(alarm, today);
        }
    }

    for (const auto& [id, smart_early] : due_ids)
    {
        AlarmRecord alarm;
        bool is_once = false;
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_alarms.find(id);
            if (it == m_alarms.end())
                continue;
            alarm = it->second;
            is_once = alarm.days_of_week.empty();
            if (is_once)
                m_alarms.erase(it);
        }

        fireAlarm(alarm, smart_early);
        if (is_once)
            disableOnceAlarm(id);
    }
}

void AlarmManager::runLoop()
{
    while (m_running.load(std::memory_order_acquire))
    {
        if (AppState::get().running.load(std::memory_order_acquire))
            tick();

        std::unique_lock lock(m_stop_mutex);
        m_stop_cv.wait_for(lock, std::chrono::seconds(1), [this]()
        {
            return !m_running.load(std::memory_order_acquire);
        });
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
