#include "db_query_store.h"
#include "../../../db/database.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_set>

#include <json/json.h>

#include "../v1/chat_store.h"
#include "../../../core/logger.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../device/device_wire_id.hpp"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    constexpr int kMaxQueries = 10;
    constexpr int kDefaultLimit = 100;
    constexpr int kMaxLimit = 1000;

    Json::Value make_query_error(
        const std::string& table,
        const std::string& code,
        const std::string& message,
        const std::string& field = "")
    {
        Json::Value result;
        result["table"] = table;
        result["count"] = 0;
        result["items"] = Json::Value(Json::arrayValue);
        Json::Value err;
        err["code"] = code;
        err["message"] = message;
        if (!field.empty())
            err["field"] = field;
        result["error"] = err;
        return result;
    }

    Json::Value make_query_success(const std::string& table, Json::Value items)
    {
        Json::Value result;
        result["table"] = table;
        result["count"] = static_cast<Json::UInt>(items.size());
        result["items"] = std::move(items);
        return result;
    }

    std::optional<int64_t> filter_int(const Json::Value& filter, const char* key)
    {
        if (!filter.isMember(key))
            return std::nullopt;
        const auto& value = filter[key];
        if (value.isInt64())
            return value.asInt64();
        if (value.isInt())
            return value.asInt();
        if (value.isUInt())
            return static_cast<int64_t>(value.asUInt());
        return std::nullopt;
    }

    std::optional<std::string> filter_string(const Json::Value& filter, const char* key)
    {
        if (!filter.isMember(key) || !filter[key].isString())
            return std::nullopt;
        return filter[key].asString();
    }

    bool has_any_filter(const Json::Value& filter, std::initializer_list<const char*> keys)
    {
        for (const char* key : keys)
        {
            if (filter.isMember(key))
                return true;
        }
        return false;
    }

    int clamp_limit(const Json::Value& query)
    {
        int limit = kDefaultLimit;
        if (query.isMember("limit") && query["limit"].isInt())
            limit = query["limit"].asInt();
        return std::max(1, std::min(limit, kMaxLimit));
    }

    bool order_desc(const Json::Value& query)
    {
        return query.isMember("order") && query["order"].isString() && query["order"].asString() == "desc";
    }

    std::string to_iso_or_null(const drogon::orm::Field& field)
    {
        if (field.isNull())
            return {};
        return v1::ChatStore::to_created_at_iso(field.as<std::string>());
    }
}

DbQueryStore::DbQueryStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

Json::Value DbQueryStore::execute(const Json::Value& request, std::string& error, std::string& field) const
{
    if (!request.isMember("queries") || !request["queries"].isArray())
    {
        error = "queries 배열이 필요합니다.";
        field = "queries";
        return Json::Value();
    }

    const auto& queries = request["queries"];
    if (queries.empty() || queries.size() > kMaxQueries)
    {
        error = "queries 는 1~10개여야 합니다.";
        field = "queries";
        return Json::Value();
    }

    Json::Value results(Json::arrayValue);
    for (const auto& query : queries)
        results.append(executeOne(query));

    Json::Value body;
    body["results"] = results;
    return body;
}

Json::Value DbQueryStore::executeOne(const Json::Value& query) const
{
    if (!query.isObject() || !query.isMember("table") || !query["table"].isString())
    {
        return make_query_error("", "INVALID_FILTER", "table 필드가 필요합니다.", "table");
    }

    const std::string table = query["table"].asString();

    try
    {
        return executeOneUnchecked(query, table);
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("db_query failed for table {}: {}", table, e.what());
        return make_query_error(table, "INTERNAL_ERROR", "조회 중 오류가 발생했습니다.", "table");
    }
}

Json::Value DbQueryStore::executeOneUnchecked(const Json::Value& query, const std::string& table) const
{
    const Json::Value filter = query.isMember("filter") && query["filter"].isObject()
        ? query["filter"]
        : Json::Value(Json::objectValue);
    const int limit = clamp_limit(query);
    const bool desc = order_desc(query);

    if (table == "user")
    {
        std::string sql = "SELECT id, name, created_at FROM user";
        std::vector<std::string> clauses;
        if (const auto id = filter_int(filter, "id"))
            clauses.push_back("id = " + std::to_string(*id));
        if (!clauses.empty())
            sql += " WHERE " + clauses[0];
        sql += desc ? " ORDER BY id DESC" : " ORDER BY id ASC";
        sql += " LIMIT " + std::to_string(limit);

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["name"] = row["name"].as<std::string>();
            if (!row["created_at"].isNull())
                item["createdAt"] = to_iso_or_null(row["created_at"]);
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "room")
    {
        std::ostringstream sql;
        sql << "SELECT DISTINCT r.id, r.name, r.description FROM room r";
        if (const auto user_id = filter_int(filter, "userId"))
            sql << " JOIN room_user_map m ON m.room_id = r.id WHERE m.user_id = " << *user_id;
        if (const auto id = filter_int(filter, "id"))
            sql << (filter_int(filter, "userId") ? " AND" : " WHERE") << " r.id = " << *id;
        sql << (desc ? " ORDER BY r.id DESC" : " ORDER BY r.id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["name"] = row["name"].as<std::string>();
            item["description"] = row["description"].as<std::string>();
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "room_user_map")
    {
        if (!has_any_filter(filter, {"roomId", "userId"}))
            return make_query_error(table, "INVALID_FILTER", "roomId|userId 중 최소 1개는 필수입니다.", "roomId|userId");

        std::ostringstream sql;
        sql << "SELECT room_id, user_id FROM room_user_map WHERE 1=1";
        if (const auto room_id = filter_int(filter, "roomId"))
            sql << " AND room_id = " << *room_id;
        if (const auto user_id = filter_int(filter, "userId"))
            sql << " AND user_id = " << *user_id;
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["roomId"] = static_cast<Json::Int64>(row["room_id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "device")
    {
        std::ostringstream sql;
        sql << "SELECT DISTINCT d.id, d.name, d.description, d.class, d.archived, d.enabled"
            << " FROM device d";
        bool joined_room = false;
        bool joined_user = false;
        if (filter_int(filter, "roomId"))
        {
            sql << " JOIN device_room_map drm ON drm.device_id = d.id";
            joined_room = true;
        }
        if (filter_int(filter, "userId"))
        {
            sql << " JOIN device_user_map dum ON dum.device_id = d.id";
            joined_user = true;
        }

        std::vector<std::string> clauses;
        if (const auto id = filter_int(filter, "id"))
            clauses.push_back("d.id = " + std::to_string(*id));
        if (const auto room_id = filter_int(filter, "roomId"))
            clauses.push_back("drm.room_id = " + std::to_string(*room_id));
        if (const auto user_id = filter_int(filter, "userId"))
            clauses.push_back("dum.user_id = " + std::to_string(*user_id));
        if (const auto class_name = filter_string(filter, "class"))
            clauses.push_back("d.class = '" + *class_name + "'");
        const int archived = filter.isMember("archived") ? filter["archived"].asInt() : 0;
        clauses.push_back("d.archived = " + std::to_string(archived));
        // Demo: cameras are hidden from agent db/query the same way as list_devices.
        if (demoVirtualDevicesEnabled())
            clauses.push_back("d.class NOT IN ('reolink_e1_pro', 'droid_cam')");

        if (!clauses.empty())
        {
            sql << " WHERE ";
            for (size_t i = 0; i < clauses.size(); ++i)
            {
                if (i > 0)
                    sql << " AND ";
                sql << clauses[i];
            }
        }
        (void)joined_room;
        (void)joined_user;
        sql << (desc ? " ORDER BY d.id DESC" : " ORDER BY d.id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            const auto name = row["name"].as<std::string>();
            item["wireId"] = dev::wireIdForDbRow(row["id"].as<int64_t>(), name);
            item["name"] = name;
            item["description"] = row["description"].as<std::string>();
            item["class"] = row["class"].as<std::string>();
            item["archived"] = row["archived"].as<int>();
            item["enabled"] = row["enabled"].as<int>() != 0;
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "device_user_map" || table == "device_room_map")
    {
        const bool is_user_map = table == "device_user_map";
        if (!has_any_filter(filter, is_user_map ? std::initializer_list<const char*>{"deviceId", "userId"}
                                              : std::initializer_list<const char*>{"deviceId", "roomId"}))
        {
            return make_query_error(
                table,
                "INVALID_FILTER",
                is_user_map ? "deviceId|userId 중 최소 1개는 필수입니다." : "deviceId|roomId 중 최소 1개는 필수입니다.",
                is_user_map ? "deviceId|userId" : "deviceId|roomId");
        }

        std::ostringstream sql;
        if (is_user_map)
            sql << "SELECT device_id, user_id FROM device_user_map WHERE 1=1";
        else
            sql << "SELECT device_id, room_id FROM device_room_map WHERE 1=1";

        if (const auto device_id = filter_int(filter, "deviceId"))
            sql << " AND device_id = " << *device_id;
        if (is_user_map)
        {
            if (const auto user_id = filter_int(filter, "userId"))
                sql << " AND user_id = " << *user_id;
        }
        else if (const auto room_id = filter_int(filter, "roomId"))
        {
            sql << " AND room_id = " << *room_id;
        }
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["deviceId"] = static_cast<Json::Int64>(row["device_id"].as<int64_t>());
            if (is_user_map)
                item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            else
                item["roomId"] = static_cast<Json::Int64>(row["room_id"].as<int64_t>());
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "notification")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, type, message, read, created_at FROM notification WHERE user_id = "
            << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (filter.isMember("read"))
            sql << " AND read = " << filter["read"].asInt();
        if (const auto from = filter_string(filter, "from"))
            sql << " AND created_at >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND created_at < '" << *to << "'";
        sql << (desc ? " ORDER BY created_at DESC, id DESC" : " ORDER BY created_at ASC, id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["type"] = row["type"].as<std::string>();
            item["message"] = row["message"].as<std::string>();
            item["read"] = row["read"].as<int>() != 0;
            item["createdAt"] = to_iso_or_null(row["created_at"]);
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "chat_history")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, title, created_at, updated_at FROM chat_history WHERE user_id = "
            << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (const auto from = filter_string(filter, "from"))
            sql << " AND updated_at >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND updated_at < '" << *to << "'";
        sql << (desc ? " ORDER BY updated_at DESC, id DESC" : " ORDER BY updated_at ASC, id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["title"] = row["title"].as<std::string>();
            item["createdAt"] = to_iso_or_null(row["created_at"]);
            item["updatedAt"] = to_iso_or_null(row["updated_at"]);
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "automation_rule")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, external_id, name, enabled, cooldown_ms, trigger_json, schedule_json, actions_json,"
            << " created_at, updated_at FROM automation_rule WHERE user_id = " << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (filter.isMember("enabled"))
            sql << " AND enabled = " << filter["enabled"].asInt();
        sql << (desc ? " ORDER BY updated_at DESC, id DESC" : " ORDER BY updated_at ASC, id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["externalId"] = row["external_id"].as<std::string>();
            item["name"] = row["name"].as<std::string>();
            item["enabled"] = row["enabled"].as<int>() != 0;
            item["cooldownMs"] = static_cast<Json::Int64>(row["cooldown_ms"].as<int64_t>());
            if (!row["trigger_json"].isNull())
                item["triggerJson"] = row["trigger_json"].as<std::string>();
            if (!row["schedule_json"].isNull())
                item["scheduleJson"] = row["schedule_json"].as<std::string>();
            item["actionsJson"] = row["actions_json"].as<std::string>();
            item["createdAt"] = to_iso_or_null(row["created_at"]);
            item["updatedAt"] = to_iso_or_null(row["updated_at"]);
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "power_energy")
    {
        std::ostringstream sql;
        sql << "SELECT pe.id, pe.device_id, pe.granularity, pe.time_start, pe.energy_wh, pe.coverage,"
            << " pe.sample_count, d.name AS device_name"
            << " FROM power_energy pe"
            << " LEFT JOIN device d ON d.id = pe.device_id"
            << " WHERE 1=1";
        if (filter.isMember("deviceId"))
        {
            if (filter["deviceId"].isNull())
                sql << " AND pe.device_id IS NULL";
            else if (const auto device_id = filter_int(filter, "deviceId"))
                sql << " AND pe.device_id = " << *device_id;
        }
        if (const auto id = filter_int(filter, "id"))
            sql << " AND pe.id = " << *id;
        if (const auto granularity = filter_string(filter, "granularity"))
            sql << " AND pe.granularity = '" << *granularity << "'";
        if (const auto from = filter_string(filter, "from"))
            sql << " AND pe.time_start >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND pe.time_start < '" << *to << "'";
        sql << (desc ? " ORDER BY pe.time_start DESC" : " ORDER BY pe.time_start ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            if (row["device_id"].isNull())
            {
                item["deviceId"] = Json::nullValue;
                item["deviceName"] = Json::nullValue;
            }
            else
            {
                item["deviceId"] = static_cast<Json::Int64>(row["device_id"].as<int64_t>());
                if (row["device_name"].isNull())
                    item["deviceName"] = Json::nullValue;
                else
                    item["deviceName"] = row["device_name"].as<std::string>();
            }
            item["granularity"] = row["granularity"].as<std::string>();
            item["timeStart"] = row["time_start"].as<std::string>();
            item["energyWh"] = row["energy_wh"].as<double>();
            item["coverage"] = row["coverage"].as<double>();
            item["sampleCount"] = static_cast<Json::Int64>(row["sample_count"].as<int64_t>());
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "power_report")
    {
        const bool report_desc =
            !query.isMember("order") || !query["order"].isString() || query["order"].asString() == "desc";

        std::ostringstream sql;
        sql << "SELECT pr.id, pr.energy_id, pr.device_id, pr.period, pr.period_start, pr.metrics,"
            << " pr.report_text, pr.created_at, d.name AS device_name"
            << " FROM power_report pr"
            << " LEFT JOIN device d ON d.id = pr.device_id"
            << " WHERE 1=1";
        if (filter.isMember("deviceId"))
        {
            if (filter["deviceId"].isNull())
                sql << " AND pr.device_id IS NULL";
            else if (const auto device_id = filter_int(filter, "deviceId"))
                sql << " AND pr.device_id = " << *device_id;
        }
        if (const auto id = filter_int(filter, "id"))
            sql << " AND pr.id = " << *id;
        if (const auto energy_id = filter_int(filter, "energyId"))
            sql << " AND pr.energy_id = " << *energy_id;
        if (const auto period = filter_string(filter, "period"))
            sql << " AND pr.period = '" << *period << "'";
        if (const auto period_start = filter_string(filter, "periodStart"))
            sql << " AND pr.period_start = '" << *period_start << "'";
        if (const auto from = filter_string(filter, "from"))
            sql << " AND pr.period_start >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND pr.period_start < '" << *to << "'";
        sql << (report_desc ? " ORDER BY pr.period_start DESC" : " ORDER BY pr.period_start ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["energyId"] = static_cast<Json::Int64>(row["energy_id"].as<int64_t>());
            if (row["device_id"].isNull())
            {
                item["deviceId"] = Json::nullValue;
                item["deviceName"] = Json::nullValue;
            }
            else
            {
                item["deviceId"] = static_cast<Json::Int64>(row["device_id"].as<int64_t>());
                if (row["device_name"].isNull())
                    item["deviceName"] = Json::nullValue;
                else
                    item["deviceName"] = row["device_name"].as<std::string>();
            }
            item["period"] = row["period"].as<std::string>();
            item["periodStart"] = row["period_start"].as<std::string>();
            if (!row["metrics"].isNull())
                item["metrics"] = row["metrics"].as<std::string>();
            if (!row["report_text"].isNull())
                item["reportText"] = row["report_text"].as<std::string>();
            if (!row["created_at"].isNull())
                item["createdAt"] = to_iso_or_null(row["created_at"]);
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "schedule_task")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, title, created_at, created_by, category, schedule_kind, day_of_week,"
            << " event_date, start_minute, end_minute, done, source_insight_id"
            << " FROM schedule_task WHERE user_id = " << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (const auto category = filter_string(filter, "category"))
            sql << " AND category = '" << *category << "'";
        if (const auto schedule_kind = filter_string(filter, "scheduleKind"))
            sql << " AND schedule_kind = '" << *schedule_kind << "'";
        if (const auto day = filter_string(filter, "dayOfWeek"))
            sql << " AND day_of_week = '" << *day << "'";
        if (const auto event_date = filter_string(filter, "eventDate"))
            sql << " AND event_date = '" << *event_date << "'";
        if (const auto from = filter_string(filter, "from"))
            sql << " AND event_date >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND event_date < '" << *to << "'";
        if (filter.isMember("done"))
            sql << " AND done = " << filter["done"].asInt();
        if (const auto created_by = filter_string(filter, "createdBy"))
            sql << " AND created_by = '" << *created_by << "'";
        if (const auto source_insight_id = filter_int(filter, "sourceInsightId"))
            sql << " AND source_insight_id = " << *source_insight_id;
        sql << " ORDER BY schedule_kind ASC, event_date ASC, day_of_week ASC, start_minute ASC";
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["title"] = row["title"].as<std::string>();
            if (!row["created_at"].isNull())
                item["createdAt"] = to_iso_or_null(row["created_at"]);
            else
                item["createdAt"] = Json::nullValue;
            item["createdBy"] = row["created_by"].as<std::string>();
            item["category"] = row["category"].as<std::string>();
            item["scheduleKind"] = row["schedule_kind"].as<std::string>();
            item["dayOfWeek"] = row["day_of_week"].as<std::string>();
            if (row["event_date"].isNull())
                item["eventDate"] = Json::nullValue;
            else
                item["eventDate"] = row["event_date"].as<std::string>();
            if (row["start_minute"].isNull())
                item["startMinute"] = Json::nullValue;
            else
                item["startMinute"] = row["start_minute"].as<int>();
            if (row["end_minute"].isNull())
                item["endMinute"] = Json::nullValue;
            else
                item["endMinute"] = row["end_minute"].as<int>();
            item["done"] = row["done"].as<int>() != 0;
            if (row["source_insight_id"].isNull())
                item["sourceInsightId"] = Json::nullValue;
            else
                item["sourceInsightId"] = static_cast<Json::Int64>(row["source_insight_id"].as<int64_t>());
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "insight")
    {
        if (!has_any_filter(filter, {"userId", "surface", "date", "from", "to"}))
            return make_query_error(
                table,
                "INVALID_FILTER",
                "userId|surface|date|from|to 중 최소 1개는 필수입니다.",
                "userId|surface|date|from|to");

        std::ostringstream sql;
        sql << "SELECT id, user_id, surface, kind, date, label, title, text, actionable, action_type,"
            << " approved, rule_json, schedule_task_json, created_at FROM insight WHERE 1=1";
        if (const auto user_id = filter_int(filter, "userId"))
            sql << " AND user_id = " << *user_id;
        if (const auto surface = filter_string(filter, "surface"))
            sql << " AND surface = '" << *surface << "'";
        if (const auto date = filter_string(filter, "date"))
            sql << " AND date = '" << *date << "'";
        if (const auto from = filter_string(filter, "from"))
            sql << " AND date >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND date < '" << *to << "'";
        sql << (desc ? " ORDER BY date DESC, id DESC" : " ORDER BY date ASC, id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["surface"] = row["surface"].as<std::string>();
            item["kind"] = row["kind"].as<std::string>();
            item["date"] = row["date"].as<std::string>();
            if (row["label"].isNull())
                item["label"] = Json::nullValue;
            else
                item["label"] = row["label"].as<std::string>();
            item["title"] = row["title"].as<std::string>();
            item["text"] = row["text"].as<std::string>();
            item["actionable"] = row["actionable"].as<int>() != 0;
            if (row["action_type"].isNull())
                item["actionType"] = Json::nullValue;
            else
                item["actionType"] = row["action_type"].as<std::string>();
            item["approved"] = row["approved"].as<int>() != 0;
            if (row["rule_json"].isNull())
                item["ruleJson"] = Json::nullValue;
            else
                item["ruleJson"] = row["rule_json"].as<std::string>();
            if (row["schedule_task_json"].isNull())
                item["scheduleTaskJson"] = Json::nullValue;
            else
                item["scheduleTaskJson"] = row["schedule_task_json"].as<std::string>();
            item["createdAt"] = to_iso_or_null(row["created_at"]);
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "user_action_log")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, action_type, ref_type, ref_id, occurred_at, category"
            << " FROM user_action_log WHERE user_id = " << *filter_int(filter, "userId");
        if (const auto action_type = filter_string(filter, "actionType"))
            sql << " AND action_type = '" << *action_type << "'";
        if (const auto ref_type = filter_string(filter, "refType"))
            sql << " AND ref_type = '" << *ref_type << "'";
        if (const auto ref_id = filter_int(filter, "refId"))
            sql << " AND ref_id = " << *ref_id;
        if (const auto category = filter_string(filter, "category"))
            sql << " AND category = '" << *category << "'";
        if (const auto from = filter_string(filter, "from"))
            sql << " AND occurred_at >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND occurred_at < '" << *to << "'";
        sql << (desc ? " ORDER BY occurred_at DESC, id DESC" : " ORDER BY occurred_at ASC, id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["actionType"] = row["action_type"].as<std::string>();
            item["refType"] = row["ref_type"].as<std::string>();
            item["refId"] = static_cast<Json::Int64>(row["ref_id"].as<int64_t>());
            item["occurredAt"] = row["occurred_at"].as<std::string>();
            if (row["category"].isNull())
                item["category"] = Json::nullValue;
            else
                item["category"] = row["category"].as<std::string>();
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "goal")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, title, category, status, created_at, updated_at"
            << " FROM goal WHERE user_id = " << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (const auto status = filter_string(filter, "status"))
            sql << " AND status = '" << *status << "'";
        sql << (desc ? " ORDER BY created_at DESC, id DESC" : " ORDER BY created_at ASC, id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["title"] = row["title"].as<std::string>();
            item["category"] = row["category"].as<std::string>();
            item["status"] = row["status"].as<std::string>();
            item["createdAt"] = to_iso_or_null(row["created_at"]);
            item["updatedAt"] = to_iso_or_null(row["updated_at"]);
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "sleep_session")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, room_id, radar_id, station_id, night_date, onset, final_wake,"
            << " time_in_bed_s, asleep_total_s, efficiency, stage_totals, toss_events, hr_mean, br_mean, snore_ratio"
            << " FROM sleep_session WHERE user_id = " << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (const auto night_date = filter_string(filter, "nightDate"))
            sql << " AND night_date = '" << *night_date << "'";
        if (const auto from = filter_string(filter, "from"))
            sql << " AND night_date >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND night_date < '" << *to << "'";
        sql << (desc ? " ORDER BY night_date DESC, id DESC" : " ORDER BY night_date ASC, id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["roomId"] = static_cast<Json::Int64>(row["room_id"].as<int64_t>());
            item["radarId"] = static_cast<Json::Int64>(row["radar_id"].as<int64_t>());
            if (row["station_id"].isNull())
                item["stationId"] = Json::nullValue;
            else
                item["stationId"] = static_cast<Json::Int64>(row["station_id"].as<int64_t>());
            item["nightDate"] = row["night_date"].as<std::string>();
            if (!row["onset"].isNull())
                item["onset"] = row["onset"].as<std::string>();
            if (!row["final_wake"].isNull())
                item["finalWake"] = row["final_wake"].as<std::string>();
            if (!row["time_in_bed_s"].isNull())
                item["timeInBedS"] = static_cast<Json::Int64>(row["time_in_bed_s"].as<int64_t>());
            if (!row["asleep_total_s"].isNull())
                item["asleepTotalS"] = static_cast<Json::Int64>(row["asleep_total_s"].as<int64_t>());
            if (!row["efficiency"].isNull())
                item["efficiency"] = row["efficiency"].as<double>();
            if (!row["stage_totals"].isNull())
            {
                const auto raw = row["stage_totals"].as<std::string>();
                Json::Value parsed;
                Json::CharReaderBuilder reader;
                std::string errors;
                std::istringstream stream(raw);
                if (Json::parseFromStream(reader, stream, &parsed, &errors) && parsed.isObject())
                    item["stageTotals"] = parsed;
                else
                    item["stageTotals"] = raw;
            }
            if (!row["toss_events"].isNull())
                item["tossEvents"] = static_cast<Json::Int64>(row["toss_events"].as<int64_t>());
            if (!row["hr_mean"].isNull())
                item["hrMean"] = row["hr_mean"].as<double>();
            if (!row["br_mean"].isNull())
                item["brMean"] = row["br_mean"].as<double>();
            if (!row["snore_ratio"].isNull())
                item["snoreRatio"] = row["snore_ratio"].as<double>();
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "sleep_stat")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, room_id, session_id, granularity, time_start, time_end, coverage,"
            << " stage_label, stage_ratio, stage_confidence, status_ratio, toss_mean, toss_max, toss_p90,"
            << " toss_events, toss_ratio, hr_mean, hr_min, hr_max, hr_std, hr_confidence,"
            << " br_mean, br_min, br_max, br_std, snore_ratio, env_temp, env_lux, env_noise, summary_text"
            << " FROM sleep_stat WHERE user_id = " << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (const auto granularity = filter_string(filter, "granularity"))
            sql << " AND granularity = '" << *granularity << "'";
        if (const auto session_id = filter_int(filter, "sessionId"))
            sql << " AND session_id = " << *session_id;
        if (const auto from = filter_string(filter, "from"))
            sql << " AND time_start >= '" << *from << "'";
        if (const auto to = filter_string(filter, "to"))
            sql << " AND time_start < '" << *to << "'";
        sql << (desc ? " ORDER BY time_start DESC" : " ORDER BY time_start ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["roomId"] = static_cast<Json::Int64>(row["room_id"].as<int64_t>());
            if (row["session_id"].isNull())
                item["sessionId"] = Json::nullValue;
            else
                item["sessionId"] = static_cast<Json::Int64>(row["session_id"].as<int64_t>());
            item["granularity"] = row["granularity"].as<std::string>();
            item["timeStart"] = row["time_start"].as<std::string>();
            if (!row["time_end"].isNull())
                item["timeEnd"] = row["time_end"].as<std::string>();
            item["coverage"] = row["coverage"].as<double>();
            if (!row["status_ratio"].isNull())
                item["statusRatio"] = row["status_ratio"].as<std::string>();
            if (!row["toss_mean"].isNull())
                item["tossMean"] = row["toss_mean"].as<double>();
            if (!row["summary_text"].isNull())
                item["summaryText"] = row["summary_text"].as<std::string>();
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "sleep_report")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, period, period_start, session_id, metrics, report_text"
            << " FROM sleep_report WHERE user_id = " << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (const auto period = filter_string(filter, "period"))
            sql << " AND period = '" << *period << "'";
        if (const auto period_start = filter_string(filter, "periodStart"))
            sql << " AND period_start = '" << *period_start << "'";
        sql << (desc ? " ORDER BY period_start DESC" : " ORDER BY period_start ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["period"] = row["period"].as<std::string>();
            item["periodStart"] = row["period_start"].as<std::string>();
            if (row["session_id"].isNull())
                item["sessionId"] = Json::nullValue;
            else
                item["sessionId"] = static_cast<Json::Int64>(row["session_id"].as<int64_t>());
            if (!row["metrics"].isNull())
                item["metrics"] = row["metrics"].as<std::string>();
            if (!row["report_text"].isNull())
                item["reportText"] = row["report_text"].as<std::string>();
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    if (table == "alarm")
    {
        if (!filter_int(filter, "userId"))
            return make_query_error(table, "INVALID_FILTER", "userId 는 필수입니다.", "userId");

        std::ostringstream sql;
        sql << "SELECT id, user_id, name, time_minute, days_of_week, smart_wake, radar_device_id,"
            << " device_id, method, enabled, created_at, updated_at"
            << " FROM alarm WHERE user_id = " << *filter_int(filter, "userId");
        if (const auto id = filter_int(filter, "id"))
            sql << " AND id = " << *id;
        if (filter.isMember("enabled"))
            sql << " AND enabled = " << (filter["enabled"].asBool() ? 1 : 0);
        if (filter.isMember("smartWake"))
            sql << " AND smart_wake = " << (filter["smartWake"].asBool() ? 1 : 0);
        if (const auto device_id = filter_int(filter, "deviceId"))
            sql << " AND device_id = " << *device_id;
        if (const auto radar_id = filter_int(filter, "radarDeviceId"))
            sql << " AND radar_device_id = " << *radar_id;
        sql << (desc ? " ORDER BY id DESC" : " ORDER BY id ASC");
        sql << " LIMIT " << limit;

        Json::Value items(Json::arrayValue);
        for (const auto& row : m_client->execSqlSync(sql.str()))
        {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
            item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
            item["name"] = row["name"].as<std::string>();
            item["timeMinute"] = row["time_minute"].as<int>();
            item["daysOfWeek"] = row["days_of_week"].as<std::string>();
            item["smartWake"] = row["smart_wake"].as<int>() != 0;
            if (row["radar_device_id"].isNull())
                item["radarDeviceId"] = Json::nullValue;
            else
                item["radarDeviceId"] = static_cast<Json::Int64>(row["radar_device_id"].as<int64_t>());
            if (row["device_id"].isNull())
                item["deviceId"] = Json::nullValue;
            else
                item["deviceId"] = static_cast<Json::Int64>(row["device_id"].as<int64_t>());
            if (!row["method"].isNull())
                item["method"] = row["method"].as<std::string>();
            item["enabled"] = row["enabled"].as<int>() != 0;
            item["createdAt"] = row["created_at"].as<std::string>();
            item["updatedAt"] = row["updated_at"].as<std::string>();
            items.append(item);
        }
        return make_query_success(table, std::move(items));
    }

    static const std::unordered_set<std::string> kUnavailableTables = {
        "posture_stat",
        "posture_report",
        "weekly_plan_report",
        "gesture_set",
        "gesture_log",
    };
    if (kUnavailableTables.count(table) > 0)
    {
        return make_query_error(table, "TABLE_NOT_AVAILABLE", "테이블이 아직 준비되지 않았습니다.", "table");
    }

    return make_query_error(table, "INVALID_FILTER", "알 수 없는 테이블입니다: " + table, "table");
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
