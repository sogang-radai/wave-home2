#include "demo_automation_runtime.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "../app/app_state.h"
#include "demo_device_backend.h"
#include "demo_session_registry.h"
#include "demo_session_writes.h"
#include "../core/logger.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    std::string weekdayToken(int wday)
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

    std::string todayDate()
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

    int localNowMinute()
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

    int localWeekday()
    {
        const std::time_t now_t = std::time(nullptr);
        std::tm local_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &now_t);
#else
        localtime_r(&now_t, &local_tm);
#endif
        return local_tm.tm_wday;
    }

    bool parseClock(const std::string& hhmm, int& out_hour, int& out_minute)
    {
        const auto colon = hhmm.find(':');
        if (colon == std::string::npos)
            return false;
        try
        {
            out_hour = std::stoi(hhmm.substr(0, colon));
            out_minute = std::stoi(hhmm.substr(colon + 1));
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    }

    bool localClockMatches(const std::string& hhmm)
    {
        int hour = 0;
        int minute = 0;
        if (!parseClock(hhmm, hour, minute))
            return false;

        const std::time_t now_t = std::time(nullptr);
        std::tm local_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &now_t);
#else
        localtime_r(&now_t, &local_tm);
#endif
        return local_tm.tm_hour == hour && local_tm.tm_min == minute;
    }

    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    bool daysContainToday(const Json::Value& days, bool empty_means_all)
    {
        if (!days.isArray() || days.empty())
            return empty_means_all;
        const std::string token = weekdayToken(localWeekday());
        for (const auto& day : days)
        {
            if (day.isString() && day.asString() == token)
                return true;
        }
        return false;
    }

    bool alarmIsDue(const Json::Value& alarm)
    {
        if (!alarm.get("enabled", true).asBool())
            return false;
        if (localNowMinute() != alarm.get("timeMinute", -1).asInt())
            return false;
        return daysContainToday(alarm.get("daysOfWeek", Json::Value(Json::arrayValue)), true);
    }

    bool alarmIsOnce(const Json::Value& alarm)
    {
        if (alarm.isMember("repeatWeekly") && !alarm["repeatWeekly"].isNull())
            return !alarm["repeatWeekly"].asBool();
        const auto& days = alarm.get("daysOfWeek", Json::Value(Json::arrayValue));
        return !days.isArray() || days.empty();
    }

    void fireScheduleRule(
        const std::string& runtime_id,
        const Json::Value& rule,
        const drogon::orm::DbClientPtr& client)
    {
        if (!rule.isObject() || !client)
            return;

        const std::string rule_name = rule.get("name", "예약").asString();
        const int64_t user_id = rule.get("userId", Json::Int64(0)).asInt64();
        const auto& action = rule.get("action", Json::Value(Json::objectValue));
        if (!action.isObject())
        {
            demoAppendNotification(runtime_id, user_id, "schedule", "예약 실행: " + rule_name);
            return;
        }

        const std::string device_id = action.get("deviceId", "").asString();
        const std::string action_name = action.get("name", "").asString();
        Json::Value invoke_body(Json::objectValue);
        if (action.isMember("params") && action["params"].isObject())
            invoke_body["params"] = action["params"];

        std::string device_name;
        std::string code;
        if (!device_id.empty() && !action_name.empty())
        {
            DemoDeviceBackend backend(client);
            const auto result = backend.invokeAction(runtime_id, device_id, action_name, invoke_body, code);
            if (result.isObject() && result.isMember("deviceName"))
                device_name = result["deviceName"].asString();
        }

        if (!device_name.empty() && !action_name.empty())
        {
            demoAppendNotification(
                runtime_id,
                user_id,
                "schedule",
                device_name + "이(가) " + action_name + " 되었습니다.");
        }
        else
        {
            demoAppendNotification(runtime_id, user_id, "schedule", "예약 실행: " + rule_name);
        }
    }

    void tickAlarms(const std::string& runtime_id, const drogon::orm::DbClientPtr& client)
    {
        const auto session_copy = DemoSessionRegistry::instance().get(runtime_id);
        if (!session_copy)
            return;

        const std::string today = todayDate();
        std::vector<Json::Value> due;

        {
            auto locked_session = DemoSessionRegistry::instance().lockSession(runtime_id);
            auto& session = *locked_session;
            for (const auto& alarm : session.alarms)
            {
                if (!alarm.isObject() || !alarmIsDue(alarm))
                    continue;

                const int64_t alarm_id = alarm.get("id", Json::Int64(0)).asInt64();
                const bool once = alarmIsOnce(alarm);

                if (once)
                {
                    if (session.alarm_once_fired.count(alarm_id) != 0)
                        continue;
                    session.alarm_once_fired.insert(alarm_id);
                }
                else
                {
                    const auto it = session.alarm_last_fired_date.find(alarm_id);
                    if (it != session.alarm_last_fired_date.end() && it->second == today)
                        continue;
                    session.alarm_last_fired_date[alarm_id] = today;
                }

                due.push_back(alarm);
            }
        }

        for (const auto& alarm : due)
        {
            demoFireAlarm(runtime_id, alarm, client);
            if (alarmIsOnce(alarm))
                demoDisableAlarm(runtime_id, alarm.get("id", Json::Int64(0)).asInt64());
        }
    }

    void tickSchedules(const std::string& runtime_id, const drogon::orm::DbClientPtr& client)
    {
        const int64_t now = nowMs();
        std::vector<Json::Value> due;
        std::vector<std::string> once_to_disable;

        {
            auto locked_session = DemoSessionRegistry::instance().lockSession(runtime_id);
            auto& session = *locked_session;

            for (Json::ArrayIndex i = 0; i < session.rules.size(); ++i)
            {
                auto& rule = session.rules[i];
                if (!rule.isObject() || !rule.get("enabled", true).asBool())
                    continue;
                if (!rule.isMember("schedule") || !rule["schedule"].isObject())
                    continue;

                const auto& schedule = rule["schedule"];
                const std::string repeat = schedule.get("repeat", "").asString();
                const std::string rule_id = rule.get("id", "").asString();
                if (rule_id.empty() || repeat.empty())
                    continue;

                if (repeat == "once")
                {
                    if (session.schedule_once_fired.count(rule_id) != 0)
                        continue;

                    if (session.schedule_next_fire_ms.find(rule_id) == session.schedule_next_fire_ms.end())
                    {
                        int64_t delay_ms = 0;
                        if (schedule.isMember("delayMinutes") && !schedule["delayMinutes"].isNull())
                            delay_ms = static_cast<int64_t>(schedule["delayMinutes"].asInt()) * 60 * 1000;
                        session.schedule_next_fire_ms[rule_id] = now + delay_ms;
                    }

                    if (now < session.schedule_next_fire_ms[rule_id])
                        continue;

                    session.schedule_once_fired.insert(rule_id);
                    rule["enabled"] = false;
                    due.push_back(rule);
                    once_to_disable.push_back(rule_id);
                    continue;
                }

                if (repeat == "daily")
                {
                    const std::string time = schedule.get("time", "").asString();
                    if (time.empty())
                        continue;

                    const bool matches = localClockMatches(time);
                    if (!matches)
                    {
                        session.schedule_slot_fired[rule_id] = false;
                        continue;
                    }
                    if (session.schedule_slot_fired[rule_id])
                        continue;

                    session.schedule_slot_fired[rule_id] = true;
                    due.push_back(rule);
                    continue;
                }

                if (repeat == "weekly")
                {
                    const std::string time = schedule.get("time", "").asString();
                    if (time.empty())
                        continue;

                    const bool matches = localClockMatches(time)
                        && daysContainToday(schedule.get("daysOfWeek", Json::Value(Json::arrayValue)), false);
                    if (!matches)
                    {
                        session.schedule_slot_fired[rule_id] = false;
                        continue;
                    }
                    if (session.schedule_slot_fired[rule_id])
                        continue;

                    session.schedule_slot_fired[rule_id] = true;
                    due.push_back(rule);
                }
            }
        }

        for (const auto& rule : due)
            fireScheduleRule(runtime_id, rule, client);

        for (const auto& rule_id : once_to_disable)
        {
            std::string code;
            Json::Value patch(Json::objectValue);
            patch["enabled"] = false;
            demoUpdateRule(runtime_id, rule_id, patch, code);
        }
    }
}

DemoAutomationRuntime& DemoAutomationRuntime::get()
{
    static DemoAutomationRuntime instance;
    return instance;
}

DemoAutomationRuntime::~DemoAutomationRuntime()
{
    stop();
}

void DemoAutomationRuntime::start()
{
    if (m_running.exchange(true))
        return;

    m_worker = std::thread([this]() { runLoop(); });
    LOG_INFO("DemoAutomationRuntime started");
}

void DemoAutomationRuntime::stop()
{
    if (!m_running.exchange(false))
        return;

    m_stopCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    LOG_INFO("DemoAutomationRuntime stopped");
}

void DemoAutomationRuntime::tickSession(const std::string& runtime_id)
{
    const auto client = AppState::get().db();
    if (!client)
        return;

    tickAlarms(runtime_id, client);
    tickSchedules(runtime_id, client);
    demoRefreshSpeechOverlays(runtime_id, nowMs());
}

void DemoAutomationRuntime::tick()
{
    if (!AppState::get().running.load(std::memory_order_acquire))
        return;
    if (!demoVirtualDevicesEnabled())
        return;

    const auto ids = DemoSessionRegistry::instance().listRuntimeIds();
    for (const auto& runtime_id : ids)
        tickSession(runtime_id);
}

void DemoAutomationRuntime::runLoop()
{
    while (m_running.load(std::memory_order_acquire))
    {
        tick();

        std::unique_lock lock(m_stopMutex);
        m_stopCv.wait_for(lock, std::chrono::seconds(1), [this]()
        {
            return !m_running.load(std::memory_order_acquire);
        });
    }
}

WAVE_NAMESPACE_END
