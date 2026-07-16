#include "alarms_internal_store.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "../../../app/app_state.h"
#include "../../../device/device_wire_id.hpp"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    static const char* kDays[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

    bool isValidDay(const std::string& day)
    {
        return std::any_of(std::begin(kDays), std::end(kDays), [&](const char* candidate) {
            return day == candidate;
        });
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

    std::string daysToString(const Json::Value& days)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, days.isArray() ? days : Json::Value(Json::arrayValue));
    }

    Json::Value normalizeAlarmMethod(const Json::Value& method)
    {
        if (!method.isObject() || !method.isMember("type") || !method["type"].isString())
            return method;

        if (method["type"].asString() != "sound")
            return method;

        // mock.db 레거시 sound 형식 → alarms-api.md 의 tts 로 변환 (에이전트 파싱 호환)
        Json::Value normalized;
        normalized["type"] = "tts";
        normalized["speakerId"] = 0;
        const std::string sound_id = method.get("soundId", "").asString();
        normalized["text"] = sound_id.empty() ? "알람" : sound_id;
        normalized["repeatCount"] = 1;
        normalized["intervalSec"] = 5;
        return normalized;
    }
}

AlarmsInternalStore::AlarmsInternalStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::string AlarmsInternalStore::nowStamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm {};
#if defined(_WIN32)
    localtime_s(&local_tm, &time);
#else
    localtime_r(&time, &local_tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    return buffer;
}

std::optional<int64_t> AlarmsInternalStore::resolveInternalDeviceId(const std::string& wire_id) const
{
    return dev::dbIdForWireId(m_client, wire_id);
}

std::string AlarmsInternalStore::externalDeviceId(int64_t internal_id) const
{
    auto rows = m_client->execSqlSync(
        "SELECT name FROM device WHERE id = ?",
        internal_id);
    if (rows.empty())
        return {};

    return dev::wireIdForDbRow(internal_id, rows[0]["name"].as<std::string>());
}

Json::Value AlarmsInternalStore::rowToJson(const drogon::orm::Row& row) const
{
    Json::Value item;
    item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
    item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
    item["name"] = row["name"].as<std::string>();
    item["timeMinute"] = row["time_minute"].as<int>();
    item["daysOfWeek"] = parseDaysJson(row["days_of_week"].as<std::string>());
    item["repeatWeekly"] = item["daysOfWeek"].isArray() && !item["daysOfWeek"].empty();
    item["smartWake"] = row["smart_wake"].as<int>() != 0;
    if (row["radar_device_id"].isNull())
        item["radarDeviceId"] = Json::nullValue;
    else
    {
        const auto wire_id = externalDeviceId(row["radar_device_id"].as<int64_t>());
        item["radarDeviceId"] = wire_id.empty() ? Json::nullValue : Json::Value(wire_id);
    }
    if (row["device_id"].isNull())
        item["deviceId"] = Json::nullValue;
    else
    {
        const auto wire_id = externalDeviceId(row["device_id"].as<int64_t>());
        item["deviceId"] = wire_id.empty() ? Json::nullValue : Json::Value(wire_id);
    }

    Json::CharReaderBuilder reader;
    Json::Value method;
    std::string errors;
    std::istringstream stream(row["method"].as<std::string>());
    if (Json::parseFromStream(reader, stream, &method, &errors) && !method.isNull())
        item["method"] = normalizeAlarmMethod(method);
    else
        item["method"] = Json::nullValue;

    item["enabled"] = row["enabled"].as<int>() != 0;
    item["createdAt"] = row["created_at"].as<std::string>();
    item["updatedAt"] = row["updated_at"].as<std::string>();
    return item;
}

bool AlarmsInternalStore::validatePayload(
    const Json::Value& body,
    bool partial,
    std::string& error,
    std::string& field)
{
    if ((!partial || body.isMember("timeMinute")) && body.isMember("timeMinute"))
    {
        if (!body["timeMinute"].isInt())
        {
            error = "timeMinute은 0~1439 사이의 정수여야 합니다.";
            field = "timeMinute";
            return false;
        }
        const int minute = body["timeMinute"].asInt();
        if (minute < 0 || minute > 1439)
        {
            error = "timeMinute은 0~1439 사이의 정수여야 합니다.";
            field = "timeMinute";
            return false;
        }
    }
    else if (!partial)
    {
        error = "timeMinute이 필요합니다.";
        field = "timeMinute";
        return false;
    }

    if (body.isMember("daysOfWeek") && body["daysOfWeek"].isArray())
    {
        for (const auto& day : body["daysOfWeek"])
        {
            if (!day.isString() || !isValidDay(day.asString()))
            {
                error = "daysOfWeek는 mon~sun 중 하나여야 합니다.";
                field = "daysOfWeek";
                return false;
            }
        }
    }

    const bool smart_wake = body.isMember("smartWake") ? body["smartWake"].asBool() : false;
    if (smart_wake)
    {
        const bool has_radar = body.isMember("radarDeviceId")
            && !body["radarDeviceId"].isNull()
            && body["radarDeviceId"].isString()
            && !body["radarDeviceId"].asString().empty();
        if (!has_radar)
        {
            error = "기상 맞춤 알람은 레이더를 선택해야 합니다.";
            field = "radarDeviceId";
            return false;
        }
    }

    return true;
}

Json::Value AlarmsInternalStore::listAlarms(const AlarmListFilter& filter) const
{
    std::ostringstream sql;
    sql << "SELECT id, user_id, name, time_minute, days_of_week, smart_wake, radar_device_id,"
        << " device_id, method, enabled, created_at, updated_at"
        << " FROM alarm WHERE user_id = " << filter.user_id;
    if (filter.enabled)
        sql << " AND enabled = " << (*filter.enabled ? 1 : 0);
    sql << " ORDER BY time_minute ASC";

    Json::Value items(Json::arrayValue);
    for (const auto& row : m_client->execSqlSync(sql.str()))
        items.append(rowToJson(row));
    return items;
}

Json::Value AlarmsInternalStore::createAlarm(
    const Json::Value& body,
    std::string& error,
    std::string& field) const
{
    if (!body.isMember("userId"))
    {
        error = "userId가 필요합니다.";
        field = "userId";
        return Json::Value();
    }
    if (!validatePayload(body, false, error, field))
        return Json::Value();

    const int64_t user_id = body["userId"].isInt64()
        ? body["userId"].asInt64()
        : static_cast<int64_t>(body["userId"].asInt());
    const auto stamp = nowStamp();
    const Json::Value days = body.isMember("daysOfWeek") ? body["daysOfWeek"] : Json::Value(Json::arrayValue);
    const bool smart_wake = body.get("smartWake", false).asBool();

    std::optional<int64_t> radar_id;
    if (smart_wake && body.isMember("radarDeviceId") && body["radarDeviceId"].isString())
        radar_id = resolveInternalDeviceId(body["radarDeviceId"].asString());

    std::optional<int64_t> device_id;
    if (body.isMember("deviceId") && body["deviceId"].isString())
        device_id = resolveInternalDeviceId(body["deviceId"].asString());

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const std::string method_json = body.isMember("method") && !body["method"].isNull()
        ? Json::writeString(builder, body["method"])
        : "{}";

    auto rows = m_client->execSqlSync(
        "SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM alarm");
    const int64_t next_id = rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();

    m_client->execSqlSync(
        R"SQL(
INSERT INTO alarm (
    id, user_id, name, time_minute, days_of_week, smart_wake,
    radar_device_id, device_id, method, enabled, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
        next_id,
        user_id,
        body.isMember("name") ? body["name"].asString() : "알람",
        body["timeMinute"].asInt(),
        daysToString(days),
        smart_wake ? 1 : 0,
        radar_id ? *radar_id : std::optional<int64_t>{},
        device_id ? *device_id : std::optional<int64_t>{},
        method_json,
        body.get("enabled", true).asBool() ? 1 : 0,
        stamp,
        stamp);

    rows = m_client->execSqlSync("SELECT * FROM alarm WHERE id = ?", next_id);
    if (rows.empty())
    {
        error = "알람 생성에 실패했습니다.";
        return Json::Value();
    }
    return rowToJson(rows[0]);
}

Json::Value AlarmsInternalStore::updateAlarm(
    int64_t user_id,
    int64_t alarm_id,
    const Json::Value& body,
    std::string& error,
    std::string& field) const
{
    if (!validatePayload(body, true, error, field))
        return Json::Value();

    auto rows = m_client->execSqlSync(
        "SELECT * FROM alarm WHERE id = ? AND user_id = ?",
        alarm_id,
        user_id);
    if (rows.empty())
    {
        error = "알람을 찾을 수 없습니다.";
        return Json::Value();
    }

    const auto& existing = rows[0];
    const std::string stamp = nowStamp();
    const std::string name = body.isMember("name") ? body["name"].asString() : existing["name"].as<std::string>();
    const int time_minute = body.isMember("timeMinute") ? body["timeMinute"].asInt() : existing["time_minute"].as<int>();
    const Json::Value days = body.isMember("daysOfWeek")
        ? body["daysOfWeek"]
        : parseDaysJson(existing["days_of_week"].as<std::string>());
    const bool smart_wake = body.isMember("smartWake")
        ? body["smartWake"].asBool()
        : existing["smart_wake"].as<int>() != 0;
    const bool enabled = body.isMember("enabled")
        ? body["enabled"].asBool()
        : existing["enabled"].as<int>() != 0;

    std::optional<int64_t> radar_id;
    if (smart_wake)
    {
        if (body.isMember("radarDeviceId") && body["radarDeviceId"].isString())
            radar_id = resolveInternalDeviceId(body["radarDeviceId"].asString());
        else if (!existing["radar_device_id"].isNull())
            radar_id = existing["radar_device_id"].as<int64_t>();
    }

    std::optional<int64_t> device_id;
    if (body.isMember("deviceId"))
    {
        if (body["deviceId"].isString())
            device_id = resolveInternalDeviceId(body["deviceId"].asString());
    }
    else if (!existing["device_id"].isNull())
    {
        device_id = existing["device_id"].as<int64_t>();
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const std::string method_json = body.isMember("method")
        ? Json::writeString(builder, body["method"])
        : existing["method"].as<std::string>();

    m_client->execSqlSync(
        R"SQL(
UPDATE alarm SET
    name = ?, time_minute = ?, days_of_week = ?, smart_wake = ?,
    radar_device_id = ?, device_id = ?, method = ?, enabled = ?, updated_at = ?
WHERE id = ? AND user_id = ?
)SQL",
        name,
        time_minute,
        daysToString(days),
        smart_wake ? 1 : 0,
        radar_id ? *radar_id : std::optional<int64_t>{},
        device_id ? *device_id : std::optional<int64_t>{},
        method_json,
        enabled ? 1 : 0,
        stamp,
        alarm_id,
        user_id);

    rows = m_client->execSqlSync(
        "SELECT * FROM alarm WHERE id = ? AND user_id = ?",
        alarm_id,
        user_id);
    return rowToJson(rows[0]);
}

Json::Value AlarmsInternalStore::deleteAlarm(int64_t user_id, int64_t alarm_id, std::string& error) const
{
    auto rows = m_client->execSqlSync(
        "SELECT id FROM alarm WHERE id = ? AND user_id = ?",
        alarm_id,
        user_id);
    if (rows.empty())
    {
        error = "알람을 찾을 수 없습니다.";
        return Json::Value();
    }

    m_client->execSqlSync("DELETE FROM alarm WHERE id = ? AND user_id = ?", alarm_id, user_id);
    Json::Value body;
    body["id"] = static_cast<Json::Int64>(alarm_id);
    return body;
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
