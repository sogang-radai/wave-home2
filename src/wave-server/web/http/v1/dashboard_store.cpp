#include "dashboard_store.h"
#include "../../../db/database.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <optional>
#include <sstream>
#include <vector>

#include "../../../device/device_wire_id.hpp"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {
namespace
{
    const char* kDays[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

    int day_index_for_name(const std::string& day)
    {
        for (int i = 0; i < 7; ++i)
        {
            if (day == kDays[i])
                return i;
        }
        return -1;
    }

    std::tm local_now_tm()
    {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &time);
#else
        localtime_r(&time, &local_tm);
#endif
        return local_tm;
    }

    std::time_t local_tm_to_time_t(std::tm tm)
    {
        tm.tm_isdst = -1;
        return std::mktime(&tm);
    }

    std::tm start_of_day(std::tm tm)
    {
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        return tm;
    }

    bool is_ringing_soon(const std::tm& next_fire_local, const std::tm& now_local)
    {
        const auto next_day = start_of_day(next_fire_local);
        const auto now_day = start_of_day(now_local);
        const auto next_ts = local_tm_to_time_t(next_day);
        const auto now_ts = local_tm_to_time_t(now_day);
        const int diff_days = static_cast<int>(std::lround(
            std::difftime(next_ts, now_ts) / (24.0 * 60.0 * 60.0)));
        if (diff_days == 0)
            return true;
        if (diff_days == 1)
            return next_fire_local.tm_hour < 12;
        return false;
    }

    std::optional<std::tm> compute_next_fire_local(
        int time_minute,
        const Json::Value& days_of_week,
        const std::tm& now_local)
    {
        const int hour = time_minute / 60;
        const int minute = time_minute % 60;

        std::tm base = start_of_day(now_local);
        base.tm_hour = hour;
        base.tm_min = minute;
        base.tm_isdst = -1;

        if (!days_of_week.isArray() || days_of_week.empty())
        {
            std::tm candidate = base;
            const auto candidate_ts = local_tm_to_time_t(candidate);
            const auto now_ts = local_tm_to_time_t(now_local);
            if (candidate_ts <= now_ts)
            {
                candidate.tm_mday += 1;
                candidate.tm_isdst = -1;
                mktime(&candidate);
            }
            return candidate;
        }

        std::vector<int> target_indices;
        target_indices.reserve(days_of_week.size());
        for (const auto& day : days_of_week)
        {
            if (!day.isString())
                continue;
            const int index = day_index_for_name(day.asString());
            if (index >= 0)
                target_indices.push_back(index);
        }
        if (target_indices.empty())
            return std::nullopt;

        for (int add = 0; add <= 7; ++add)
        {
            std::tm candidate = base;
            candidate.tm_mday += add;
            candidate.tm_isdst = -1;
            mktime(&candidate);
            if (std::find(target_indices.begin(), target_indices.end(), candidate.tm_wday)
                == target_indices.end())
            {
                continue;
            }
            if (add == 0)
            {
                const auto candidate_ts = local_tm_to_time_t(candidate);
                const auto now_ts = local_tm_to_time_t(now_local);
                if (candidate_ts <= now_ts)
                    continue;
            }
            return candidate;
        }

        return base;
    }

    std::string device_name_for_wire_id(db::DbClientPtr client, const std::string& wire_id)
    {
        const auto db_id = dev::dbIdForWireId(client, wire_id);
        if (!db_id)
            return wire_id;
        const auto rows = client->execSqlSync(
            "SELECT name FROM device WHERE id = ? LIMIT 1",
            *db_id);
        if (rows.empty())
            return wire_id;
        return rows[0]["name"].as<std::string>();
    }

    std::string to_utc_iso_from_local_tm(std::tm local_tm)
    {
        local_tm.tm_isdst = -1;
        const std::time_t epoch = std::mktime(&local_tm);
        std::tm utc_tm {};
#if defined(_WIN32)
        gmtime_s(&utc_tm, &epoch);
#else
        gmtime_r(&epoch, &utc_tm);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", &utc_tm);
        return buffer;
    }
}

DashboardStore::DashboardStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

Json::Value DashboardStore::parse_json_column(const drogon::orm::Field& field)
{
    if (field.isNull())
        return Json::nullValue;

    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    const auto text = field.as<std::string>();
    std::istringstream stream(text);
    if (Json::parseFromStream(builder, stream, &parsed, &errors))
        return parsed;
    return Json::nullValue;
}

Json::Value DashboardStore::parse_days_json(const std::string& raw)
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

Json::Value DashboardStore::currentState() const
{
    Json::Value out;
    Json::Value indoor;
    indoor["label"] = "쾌적";
    indoor["detail"] = "온도 24℃ · 습도 45%";
    out["indoorEnvironment"] = indoor;

    Json::Value mode;
    mode["label"] = "집중 모드";
    mode["activatedAt"] = "2026-07-02T13:00:00+09:00";
    out["controlMode"] = mode;

    Json::Value radar;
    radar["connected"] = true;
    radar["name"] = "방 1";
    out["radar"] = radar;
    return out;
}

Json::Value DashboardStore::upcomingAlarms(int64_t user_id) const
{
    const auto rows = m_client->execSqlSync(
        "SELECT id, name, time_minute, days_of_week, enabled"
        " FROM alarm WHERE user_id = ? AND enabled = 1",
        user_id);

    Json::Value alarms(Json::arrayValue);
    for (const auto& row : rows)
    {
        Json::Value alarm;
        alarm["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        alarm["name"] = row["name"].as<std::string>();
        alarm["timeMinute"] = row["time_minute"].as<int>();
        alarm["daysOfWeek"] = parse_days_json(row["days_of_week"].as<std::string>());
        alarm["enabled"] = true;
        alarms.append(alarm);
    }
    return upcomingAlarmsFromItems(alarms);
}

Json::Value DashboardStore::upcomingAlarmsFromItems(const Json::Value& alarms) const
{
    struct Candidate
    {
        int64_t id;
        std::string name;
        int time_minute;
        Json::Value days_of_week;
        std::tm next_fire;
    };

    const auto now_local = local_now_tm();
    std::vector<Candidate> candidates;
    if (alarms.isArray())
        candidates.reserve(alarms.size());

    for (const auto& item : alarms)
    {
        if (!item.isObject() || !item.get("enabled", true).asBool())
            continue;
        const auto days = item.isMember("daysOfWeek") ? item["daysOfWeek"] : Json::Value(Json::arrayValue);
        const auto next_fire = compute_next_fire_local(
            item.get("timeMinute", 0).asInt(),
            days,
            now_local);
        if (!next_fire || !is_ringing_soon(*next_fire, now_local))
            continue;

        candidates.push_back(Candidate{
            item.get("id", Json::Int64(0)).asInt64(),
            item.get("name", "").asString(),
            item.get("timeMinute", 0).asInt(),
            days,
            *next_fire,
        });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return local_tm_to_time_t(a.next_fire) < local_tm_to_time_t(b.next_fire);
    });

    Json::Value out(Json::arrayValue);
    for (const auto& item : candidates)
    {
        Json::Value alarm;
        alarm["id"] = static_cast<Json::Int64>(item.id);
        alarm["name"] = item.name;
        alarm["timeMinute"] = item.time_minute;
        alarm["daysOfWeek"] = item.days_of_week;
        alarm["nextFireAt"] = to_utc_iso_from_local_tm(item.next_fire);
        out.append(alarm);
    }
    return out;
}

Json::Value DashboardStore::activeGestureRules(int64_t user_id) const
{
    const auto rows = m_client->execSqlSync(
        "SELECT external_id, trigger_json, actions_json"
        " FROM automation_rule WHERE user_id = ? AND enabled = 1",
        user_id);

    Json::Value out(Json::arrayValue);
    for (const auto& row : rows)
    {
        const auto trigger = parse_json_column(row["trigger_json"]);
        if (!trigger.isObject() || trigger.get("kind", "").asString() != "gesture")
            continue;

        const auto actions = parse_json_column(row["actions_json"]);
        Json::Value primary_action;
        if (actions.isArray() && !actions.empty())
            primary_action = actions[0];
        else if (actions.isObject())
            primary_action = actions;
        else
            continue;

        const auto device_id = primary_action.get("deviceId", "").asString();
        if (device_id.empty())
            continue;

        Json::Value item;
        item["id"] = row["external_id"].as<std::string>();

        std::string gesture_set_path;
        if (trigger.isMember("gestureSetPath") && trigger["gestureSetPath"].isString())
            gesture_set_path = trigger["gestureSetPath"].asString();
        else if (trigger.isMember("gesture_set_path") && trigger["gesture_set_path"].isString())
            gesture_set_path = trigger["gesture_set_path"].asString();

        std::string gesture_set_id;
        const auto slash = gesture_set_path.find('/');
        if (slash != std::string::npos)
        {
            const auto next = gesture_set_path.find('/', slash + 1);
            if (next != std::string::npos)
                gesture_set_id = gesture_set_path.substr(slash + 1, next - slash - 1);
        }
        item["gestureSetId"] = gesture_set_id;

        if (trigger.isMember("classId"))
            item["classId"] = trigger["classId"];
        else if (trigger.isMember("class_id"))
            item["classId"] = trigger["class_id"];
        else
            item["classId"] = 0;

        item["actionDeviceId"] = device_id;
        item["actionDeviceName"] = device_name_for_wire_id(m_client, device_id);
        item["actionName"] = primary_action.get("name", "").asString();
        out.append(item);
    }
    return out;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
