#include "devices_internal_store.h"
#include "../../../db/database.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../device/device.h"
#include "../../../device/device_wire_id.hpp"
#include "device_class_registry.h"
#include "../v1/iot_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    std::optional<int64_t> parse_optional_int(const std::string& value)
    {
        if (value.empty())
            return std::nullopt;
        try
        {
            return std::stoll(value);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    bool room_matches(const Json::Value& room, int64_t room_id)
    {
        if (!room.isObject() || !room.isMember("id"))
            return false;
        const auto& id_value = room["id"];
        if (id_value.isInt64())
            return id_value.asInt64() == room_id;
        if (id_value.isInt())
            return id_value.asInt() == room_id;
        if (id_value.isString())
        {
            const auto parsed = parse_optional_int(id_value.asString());
            return parsed && *parsed == room_id;
        }
        return false;
    }

    bool manifest_has_device(const std::string& manifest_id)
    {
        return dev::manifestHasWireId(manifest_id);
    }

    std::optional<std::string> manifest_id_for_device_name(const std::string& name)
    {
        (void)name;
        return std::nullopt;
    }

    std::optional<std::string> manifest_id_for_db_row(
        const db::DbClientPtr& client,
        int64_t db_id)
    {
        if (!client || db_id <= 0)
            return std::nullopt;

        auto rows = client->execSqlSync(
            "SELECT name FROM device WHERE id = ? LIMIT 1",
            db_id);
        if (rows.empty())
            return std::nullopt;

        return dev::wireIdForDbRow(db_id, rows[0]["name"].as<std::string>());
    }

    std::optional<int64_t> optional_user_id(const Json::Value& body)
    {
        if (body.isMember("userId") && body["userId"].isInt64())
            return body["userId"].asInt64();
        if (body.isMember("userId") && body["userId"].isInt())
            return body["userId"].asInt();
        return std::nullopt;
    }

    bool contains_type(const std::vector<std::string>& types, const std::string& type)
    {
        return types.empty()
            || std::any_of(types.begin(), types.end(), [&](const std::string& candidate) {
                   return candidate == type;
               });
    }

    v1::IotStore make_iot_store()
    {
        return v1::IotStore(AppState::get().deviceManager);
    }

    bool demo_db_devices_enabled()
    {
        return demoVirtualDevicesEnabled();
    }

    std::string runtime_from_body(const Json::Value& body)
    {
        if (body.isMember("demoRuntimeId") && body["demoRuntimeId"].isString())
        {
            const auto id = body["demoRuntimeId"].asString();
            if (!id.empty())
            {
                rememberPreferredDemoRuntimeId(id);
                return id;
            }
        }
        return fallbackDemoRuntimeId();
    }

    std::string wire_id_from_db_row(int64_t db_id, const std::string& db_name)
    {
        return dev::wireIdForDbRow(db_id, db_name);
    }

    struct DbDeviceRow
    {
        int64_t id = 0;
        std::string name;
        std::string description;
        std::string device_class;
        bool enabled = true;
        int64_t room_id = 0;
        std::string room_name;
    };

    std::optional<DbDeviceRow> row_from_result(const drogon::orm::Row& row)
    {
        DbDeviceRow out;
        out.id = row["id"].as<int64_t>();
        out.name = row["name"].as<std::string>();
        out.description = row["description"].as<std::string>();
        out.device_class = row["class"].as<std::string>();
        out.enabled = row["enabled"].as<int>() != 0;
        if (!row["room_id"].isNull())
            out.room_id = row["room_id"].as<int64_t>();
        if (!row["room_name"].isNull())
            out.room_name = row["room_name"].as<std::string>();
        return out;
    }

    std::optional<DbDeviceRow> lookup_db_device_row(
        const db::DbClientPtr& client,
        const std::string& device_id_param)
    {
        if (!client || device_id_param.empty())
            return std::nullopt;

        if (const auto db_id = dev::dbIdForWireId(client, device_id_param))
        {
            auto rows = client->execSqlSync(
                R"SQL(
    SELECT d.id, d.name, d.description, d.class, d.enabled,
       r.id AS room_id, r.name AS room_name
    FROM device d
    LEFT JOIN device_room_map drm ON drm.device_id = d.id
    LEFT JOIN room r ON r.id = drm.room_id
    WHERE d.archived = 0 AND d.id = ?
    LIMIT 1
    )SQL",
                *db_id);
            if (!rows.empty())
                return row_from_result(rows[0]);
        }

        const auto parsed = dev::parseDeviceID(device_id_param);
        if (parsed != 0 && parsed < 1'000'000)
        {
            auto rows = client->execSqlSync(
                R"SQL(
    SELECT d.id, d.name, d.description, d.class, d.enabled,
       r.id AS room_id, r.name AS room_name
    FROM device d
    LEFT JOIN device_room_map drm ON drm.device_id = d.id
    LEFT JOIN room r ON r.id = drm.room_id
    WHERE d.archived = 0 AND d.id = ?
    LIMIT 1
    )SQL",
                static_cast<int64_t>(parsed));
            if (!rows.empty())
                return row_from_result(rows[0]);
        }

        if (const auto as_int = parse_optional_int(device_id_param))
        {
            if (*as_int > 0)
            {
                auto rows = client->execSqlSync(
                    R"SQL(
    SELECT d.id, d.name, d.description, d.class, d.enabled,
       r.id AS room_id, r.name AS room_name
    FROM device d
    LEFT JOIN device_room_map drm ON drm.device_id = d.id
    LEFT JOIN room r ON r.id = drm.room_id
    WHERE d.archived = 0 AND d.id = ?
    LIMIT 1
    )SQL",
                    *as_int);
                if (!rows.empty())
                    return row_from_result(rows[0]);
            }
        }

        return std::nullopt;
    }

    Json::Value device_json_from_db_row(const DbDeviceRow& row)
    {
        Json::Value item;
        item["id"] = wire_id_from_db_row(row.id, row.name);
        item["name"] = row.name;
        item["description"] = row.description;
        item["class"] = row.device_class;
        item["classLabel"] = DeviceClassRegistry::label_for_class(row.device_class);
        item["vendor"] = "";
        item["model"] = "";
        item["enabled"] = row.enabled;
        item["connected"] = row.device_class != "droid_cam";
        item["stateSummary"] = "데모";
        if (row.room_id > 0)
        {
            Json::Value room;
            room["id"] = static_cast<Json::Int64>(row.room_id);
            room["name"] = row.room_name;
            item["room"] = room;
        }
        else
        {
            item["room"] = Json::nullValue;
        }
        return item;
    }

    bool devices_ready(const db::DbClientPtr& client, std::string& code)
    {
        if (demo_db_devices_enabled())
        {
            if (!client)
            {
                code = "DEVICES_UNAVAILABLE";
                return false;
            }
            return true;
        }

        auto& state = AppState::get();
        if (state.no_devices)
        {
            code = "DEVICES_UNAVAILABLE";
            return false;
        }

        v1::IotStore store(state.deviceManager);
        if (!store.devicesAvailable())
        {
            code = "DEVICES_UNAVAILABLE";
            return false;
        }
        return true;
    }

    std::optional<std::string> resolve_manifest_device_id(
        const db::DbClientPtr& client,
        const std::string& device_id_param)
    {
        if (device_id_param.empty())
            return std::nullopt;

        if (manifest_has_device(device_id_param))
            return device_id_param;

        const auto parsed = dev::parseDeviceID(device_id_param);
        if (parsed != 0)
        {
            if (parsed < 1'000'000)
            {
                if (const auto from_db = manifest_id_for_db_row(client, static_cast<int64_t>(parsed)))
                    return from_db;
            }

            const auto from_parsed = dev::deviceIDToString(parsed);
            if (manifest_has_device(from_parsed))
                return from_parsed;
        }

        if (const auto as_int = parse_optional_int(device_id_param))
        {
            if (*as_int > 0 && *as_int < 1'000'000)
            {
                if (const auto from_db = manifest_id_for_db_row(client, *as_int))
                    return from_db;
            }
        }

        if (demo_db_devices_enabled() && client)
        {
            if (const auto row = lookup_db_device_row(client, device_id_param))
                return wire_id_from_db_row(row->id, row->name);
        }

        return std::nullopt;
    }

    std::optional<Json::Value> lookup_db_room_ref(
        const db::DbClientPtr& client,
        const std::string& wire_id,
        const std::string& device_name,
        const Json::Value& manifest_room)
    {
        if (!client)
            return std::nullopt;

        auto query_by_wire = [&]()
        {
            if (wire_id.empty())
                return drogon::orm::Result(nullptr);
            const auto db_id = dev::dbIdForWireId(client, wire_id);
            if (!db_id)
                return drogon::orm::Result(nullptr);
            return client->execSqlSync(
                R"SQL(
    SELECT r.id, r.name
    FROM device d
    JOIN device_room_map drm ON drm.device_id = d.id
    JOIN room r ON r.id = drm.room_id
    WHERE d.id = ?
    LIMIT 1
    )SQL",
                *db_id);
        };

        auto query_by_name = [&]()
        {
            if (device_name.empty())
                return drogon::orm::Result(nullptr);
            return client->execSqlSync(
                R"SQL(
    SELECT r.id, r.name
    FROM device d
    JOIN device_room_map drm ON drm.device_id = d.id
    JOIN room r ON r.id = drm.room_id
    WHERE d.name = ?
    LIMIT 1
    )SQL",
                device_name);
        };

        auto query_by_room_name = [&]()
        {
            if (!manifest_room.isObject() || !manifest_room.isMember("name"))
                return drogon::orm::Result(nullptr);
            const auto room_name = manifest_room["name"].asString();
            if (room_name.empty())
                return drogon::orm::Result(nullptr);
            return client->execSqlSync(
                "SELECT id, name FROM room WHERE name = ? LIMIT 1",
                room_name);
        };

        auto rows = query_by_wire();
        if (rows.empty())
            rows = query_by_name();
        if (rows.empty())
            rows = query_by_room_name();
        if (rows.empty())
            return std::nullopt;

        Json::Value out;
        out["id"] = static_cast<Json::Int64>(rows[0]["id"].as<int64_t>());
        out["name"] = rows[0]["name"].as<std::string>();
        return out;
    }

    void apply_db_room_ref(
        Json::Value& device,
        const db::DbClientPtr& client)
    {
        if (!device.isObject())
            return;

        const auto wire_id = device.get("id", "").asString();
        const auto device_name = device.get("name", "").asString();
        const auto manifest_room = device.get("room", Json::nullValue);
        const auto db_room = lookup_db_room_ref(client, wire_id, device_name, manifest_room);
        if (db_room)
            device["room"] = *db_room;
    }
    }

DevicesInternalStore::DevicesInternalStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::optional<std::string> DevicesInternalStore::resolve_wire_device_id(
    const db::DbClientPtr& client,
    const std::string& device_id_param)
{
    return resolve_manifest_device_id(client, device_id_param);
}

bool DevicesInternalStore::deviceAllowedForUser(const std::string& external_id, int64_t user_id) const
{
    if (!m_client)
        return true;

    auto count_rows = m_client->execSqlSync(
        "SELECT COUNT(*) AS cnt FROM device_user_map WHERE user_id = ?",
        user_id);
    if (count_rows.empty() || count_rows[0]["cnt"].as<int64_t>() == 0)
        return true;

    std::optional<int64_t> db_row_id;
    if (demo_db_devices_enabled())
    {
        if (const auto row = lookup_db_device_row(m_client, external_id))
            db_row_id = row->id;
    }
    else
    {
        const auto manifest_id = resolve_manifest_device_id(m_client, external_id);
        if (!manifest_id)
            return false;

        if (const auto internal_id = internalIdFromExternal(*manifest_id))
            db_row_id = internal_id;
        else
            return false;
    }

    if (!db_row_id)
        return false;

    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT 1
FROM device_user_map
WHERE user_id = ? AND device_id = ?
LIMIT 1
)SQL",
        user_id,
        *db_row_id);
    return !rows.empty();
}

std::optional<Json::Value> DevicesInternalStore::findListedDevice(
    const std::string& device_id,
    const std::optional<std::string>& demo_runtime_id) const
{
    std::string code;
    if (!devices_ready(m_client, code))
        return std::nullopt;

    if (demo_db_devices_enabled())
    {
        if (const auto row = lookup_db_device_row(m_client, device_id))
        {
            const auto wire_id = wire_id_from_db_row(row->id, row->name);
            const auto runtime_id = demo_runtime_id && !demo_runtime_id->empty()
                ? *demo_runtime_id
                : fallbackDemoRuntimeId();
            if (demo_runtime_id && !demo_runtime_id->empty())
                rememberPreferredDemoRuntimeId(*demo_runtime_id);
            DemoDeviceBackend backend(m_client);
            std::string list_code;
            const auto listed = backend.listDevices(runtime_id, list_code);
            if (list_code.empty())
            {
                for (const auto& device : listed["items"])
                {
                    if (device.isObject() && device.get("id", "").asString() == wire_id)
                        return device;
                }
            }
            return device_json_from_db_row(*row);
        }
        return std::nullopt;
    }

    const auto manifest_id = resolve_manifest_device_id(m_client, device_id);
    if (!manifest_id)
        return std::nullopt;

    const Json::Value listed = make_iot_store().listDevices();
    for (const auto& device : listed)
    {
        if (device.isObject() && device.get("id", "").asString() == *manifest_id)
        {
            Json::Value enriched = device;
            apply_db_room_ref(enriched, m_client);
            return enriched;
        }
    }
    return std::nullopt;
}

std::string DevicesInternalStore::externalIdFromDb(int64_t device_row_id) const
{
    if (!m_client)
        return {};

    auto rows = m_client->execSqlSync(
        "SELECT name FROM device WHERE id = ?",
        device_row_id);
    if (rows.empty())
        return {};

    return dev::wireIdForDbRow(device_row_id, rows[0]["name"].as<std::string>());
}

std::optional<int64_t> DevicesInternalStore::internalIdFromExternal(const std::string& wire_id) const
{
    return dev::dbIdForWireId(m_client, wire_id);
}

bool DevicesInternalStore::name_matches(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return false;

    auto lower = [](std::string value) {
        for (auto& ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value;
    };

    return lower(haystack).find(lower(needle)) != std::string::npos;
}

bool DevicesInternalStore::device_query_matches(
    const std::string& name,
    const std::string& description,
    const std::string& needle)
{
    if (needle.empty())
        return false;

    auto lower = [](std::string value) {
        for (auto& ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value;
    };

    const auto needle_l = lower(needle);
    const auto name_l = lower(name);
    const auto desc_l = lower(description);
    if (name_matches(name, needle) || name_matches(description, needle))
        return true;

    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char ch : needle_l)
    {
        if (std::isspace(ch) || ch == '-' || ch == '_' || ch == '/' || ch == ',')
        {
            if (!current.empty())
            {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(static_cast<char>(ch));
    }
    if (!current.empty())
        tokens.push_back(current);

    if (tokens.size() < 2)
        return false;

    const auto haystack = name_l + " " + desc_l;
    for (const auto& token : tokens)
    {
        if (haystack.find(token) == std::string::npos)
            return false;
    }
    return true;
}

std::string DevicesInternalStore::make_event_id()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::ostringstream stream;
    stream << "evt_" << ms;
    return stream.str();
}

Json::Value DevicesInternalStore::listDevices(
    const DeviceListFilter& filter,
    std::string& code,
    const std::optional<std::string>& demo_runtime_id) const
{
    if (!devices_ready(m_client, code))
        return Json::Value();

    if (demo_db_devices_enabled())
    {
        DemoDeviceBackend backend(m_client);
        const auto runtime_id = demo_runtime_id && !demo_runtime_id->empty()
            ? *demo_runtime_id
            : fallbackDemoRuntimeId();
        if (demo_runtime_id && !demo_runtime_id->empty())
            rememberPreferredDemoRuntimeId(*demo_runtime_id);
        const auto listed = backend.listDevices(runtime_id, code);
        if (!code.empty())
            return Json::Value();

        Json::Value items(Json::arrayValue);
        for (const auto& device : listed["items"])
        {
            if (!device.isObject())
                continue;

            if (filter.enabled && *filter.enabled != device.get("enabled", true).asBool())
                continue;
            if (filter.connected && *filter.connected != device.get("connected", false).asBool())
                continue;
            if (filter.device_class && device.get("class", "").asString() != *filter.device_class)
                continue;
            if (filter.room_id && !room_matches(device.get("room", Json::nullValue), *filter.room_id))
                continue;

            const std::string external_id = device.get("id", "").asString();
            if (filter.user_id && !deviceAllowedForUser(external_id, *filter.user_id))
                continue;

            Json::Value item;
            item["id"] = external_id;
            item["name"] = device.get("name", "");
            item["description"] = device.get("description", "");
            item["class"] = device.get("class", "");
            item["classLabel"] = device.get("classLabel", "");
            item["vendor"] = device.get("vendor", "");
            item["model"] = device.get("model", "");
            item["enabled"] = device.get("enabled", true);
            item["connected"] = device.get("connected", false);
            item["stateSummary"] = device.get("stateSummary", "");
            if (device.isMember("room"))
                item["room"] = device["room"];
            items.append(item);
        }

        Json::Value body;
        body["items"] = items;
        body["count"] = static_cast<Json::UInt>(items.size());
        return body;
    }

    const Json::Value listed = make_iot_store().listDevices();
    Json::Value items(Json::arrayValue);
    for (const auto& device : listed)
    {
        if (!device.isObject())
            continue;

        Json::Value normalized = device;
        apply_db_room_ref(normalized, m_client);

        if (filter.enabled && !*filter.enabled && normalized.get("enabled", true).asBool())
            continue;
        if (filter.enabled && *filter.enabled && !normalized.get("enabled", true).asBool())
            continue;
        if (filter.connected && *filter.connected != normalized.get("connected", false).asBool())
            continue;
        if (filter.device_class && normalized.get("class", "").asString() != *filter.device_class)
            continue;
        if (filter.room_id && !room_matches(normalized.get("room", Json::nullValue), *filter.room_id))
            continue;

        const std::string external_id = normalized.get("id", "").asString();
        if (filter.user_id && !deviceAllowedForUser(external_id, *filter.user_id))
            continue;

        Json::Value item;
        item["id"] = external_id;
        item["name"] = normalized.get("name", "");
        item["description"] = normalized.get("description", "");
        item["class"] = normalized.get("class", "");
        item["classLabel"] = normalized.get("classLabel", "");
        item["vendor"] = normalized.get("vendor", "");
        item["model"] = normalized.get("model", "");
        item["enabled"] = normalized.get("enabled", true);
        item["connected"] = normalized.get("connected", false);
        item["stateSummary"] = normalized.get("stateSummary", "");
        if (normalized.isMember("room"))
            item["room"] = normalized["room"];
        items.append(item);
    }

    Json::Value body;
    body["items"] = items;
    body["count"] = static_cast<Json::UInt>(items.size());
    return body;
}

Json::Value DevicesInternalStore::getDevice(
    const std::string& device_id,
    const std::optional<int64_t> user_id,
    std::string& code,
    const std::optional<std::string>& demo_runtime_id) const
{
    if (!devices_ready(m_client, code))
        return Json::Value();

    if (user_id && !deviceAllowedForUser(device_id, *user_id))
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const auto listed = findListedDevice(device_id, demo_runtime_id);
    if (!listed)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    Json::Value body;
    body["id"] = (*listed).get("id", "");
    body["name"] = (*listed).get("name", "");
    body["description"] = (*listed).get("description", "");
    body["class"] = (*listed).get("class", "");
    body["classLabel"] = (*listed).get("classLabel", "");
    body["vendor"] = (*listed).get("vendor", "");
    body["model"] = (*listed).get("model", "");
    body["enabled"] = (*listed).get("enabled", true);
    body["connected"] = (*listed).get("connected", false);
    body["stateSummary"] = (*listed).get("stateSummary", "");
    if ((*listed).isMember("room"))
        body["room"] = (*listed)["room"];
    body["capabilities"] = DeviceClassRegistry::capabilities_for_class(body.get("class", "").asString());
    return body;
}

Json::Value DevicesInternalStore::getState(
    const std::string& device_id,
    const std::optional<int64_t> user_id,
    std::string& code,
    const std::optional<std::string>& demo_runtime_id) const
{
    if (!devices_ready(m_client, code))
        return Json::Value();

    if (user_id && !deviceAllowedForUser(device_id, *user_id))
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const auto listed = findListedDevice(device_id, demo_runtime_id);
    if (!listed)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    if (demo_db_devices_enabled())
    {
        DemoDeviceBackend backend(m_client);
        const auto runtime_id = demo_runtime_id && !demo_runtime_id->empty()
            ? *demo_runtime_id
            : fallbackDemoRuntimeId();
        if (demo_runtime_id && !demo_runtime_id->empty())
            rememberPreferredDemoRuntimeId(*demo_runtime_id);
        return backend.getState(runtime_id, (*listed).get("id", "").asString(), code);
    }

    v1::IotStore store = make_iot_store();
    const auto class_name = (*listed).get("class", "").asString();
    const auto manifest_id = (*listed).get("id", "").asString();
    const auto query_name = class_name == "samsung_g7" || class_name == "tizen_tv" ? "state" : "status";
    const auto state = store.queryDevice(manifest_id, query_name, code);
    if (!code.empty())
        return Json::Value();

    Json::Value body;
    body["deviceId"] = manifest_id;
    body["connected"] = (*listed).get("connected", false);
    body["state"] = state;
    return body;
}

Json::Value DevicesInternalStore::queryDevice(
    const std::string& device_id,
    const std::string& query_name,
    const Json::Value& body,
    std::string& code) const
{
    if (!devices_ready(m_client, code))
        return Json::Value();

    if (const auto user_id = optional_user_id(body))
    {
        if (*user_id > 0 && !deviceAllowedForUser(device_id, *user_id))
        {
            code = "NOT_FOUND";
            return Json::Value();
        }
    }

    const auto listed = findListedDevice(device_id, runtime_from_body(body));
    if (!listed)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const std::string manifest_id = (*listed).get("id", "").asString();

    if (demo_db_devices_enabled())
    {
        DemoDeviceBackend backend(m_client);
        const auto runtime_id = runtime_from_body(body);
        const auto response = backend.queryDevice(runtime_id, manifest_id, query_name, code);
        if (!code.empty())
            return Json::Value();
        return response;
    }

    v1::IotStore store = make_iot_store();
    const auto result = store.queryDevice(manifest_id, query_name, code);
    if (!code.empty())
        return Json::Value();

    Json::Value response;
    response["deviceId"] = manifest_id;
    response["query"] = query_name;
    response["result"] = result;
    return response;
}

Json::Value DevicesInternalStore::invokeAction(
    const std::string& device_id,
    const std::string& action_name,
    const Json::Value& body,
    std::string& code) const
{
    if (!devices_ready(m_client, code))
        return Json::Value();

    if (const auto user_id = optional_user_id(body))
    {
        if (*user_id > 0 && !deviceAllowedForUser(device_id, *user_id))
        {
            code = "NOT_FOUND";
            return Json::Value();
        }
    }

    Json::Value params(Json::objectValue);
    if (body.isMember("params") && body["params"].isObject())
        params = body["params"];
    else if (body.isObject())
    {
        static const std::unordered_set<std::string> k_meta = {
            "userId", "reason", "execMode", "triggeredBy", "params", "repeatIntervalMs", "demoRuntimeId"};
        for (const auto& key : body.getMemberNames())
        {
            if (k_meta.count(key) == 0)
                params[key] = body[key];
        }
    }

    const auto listed = findListedDevice(device_id, runtime_from_body(body));
    if (!listed)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const std::string manifest_id = (*listed).get("id", "").asString();

    if (demo_db_devices_enabled())
    {
        DemoDeviceBackend backend(m_client);
        const auto runtime_id = runtime_from_body(body);
        Json::Value invoke_body = body;
        invoke_body["params"] = params;
        Json::Value response = backend.invokeAction(runtime_id, manifest_id, action_name, invoke_body, code);
        if (!code.empty())
            return Json::Value();
        response["eventId"] = make_event_id();
        return response;
    }

    const auto result = make_iot_store().invokeDevice(manifest_id, action_name, params, code);
    if (!code.empty())
        return Json::Value();

    Json::Value response;
    response["ok"] = true;
    response["deviceId"] = manifest_id;
    response["action"] = action_name;
    if (result.isObject())
        response["state"] = result;
    response["eventId"] = make_event_id();
    return response;
}

Json::Value DevicesInternalStore::listEvents(const EventsListFilter& filter, std::string& code) const
{
    if (!devices_ready(m_client, code))
        return Json::Value();

    if (demo_db_devices_enabled())
    {
        Json::Value body;
        body["items"] = Json::Value(Json::arrayValue);
        body["count"] = 0;
        return body;
    }

    const int limit = std::max(1, std::min(filter.limit, 200));
    std::string event_device_id;
    if (filter.device_id)
    {
        if (const auto manifest_id = resolve_manifest_device_id(m_client, *filter.device_id))
            event_device_id = *manifest_id;
        else
            event_device_id = *filter.device_id;
    }
    const Json::Value all = make_iot_store().listEvents(event_device_id);
    Json::Value items(Json::arrayValue);

    for (const auto& event : all)
    {
        if (!event.isObject())
            continue;
        const auto type = event.get("type", "").asString();
        if (!contains_type(filter.types, type))
            continue;
        if (filter.from && event.get("occurredAt", "").asString() < *filter.from)
            continue;
        if (filter.to && event.get("occurredAt", "").asString() > *filter.to)
            continue;
        items.append(event);
        if (static_cast<int>(items.size()) >= limit)
            break;
    }

    Json::Value body;
    body["items"] = items;
    body["count"] = static_cast<Json::UInt>(items.size());
    return body;
}

std::optional<ResolvedDevice> DevicesInternalStore::resolveDeviceByName(
    int64_t room_id,
    const std::string& device_name,
    const std::optional<int64_t> user_id,
    std::string& code,
    const std::optional<std::string>& demo_runtime_id) const
{
    DeviceListFilter filter;
    filter.room_id = room_id;
    filter.enabled = std::nullopt;
    if (user_id)
        filter.user_id = *user_id;

    const Json::Value listed = listDevices(filter, code, demo_runtime_id);
    if (!code.empty())
        return std::nullopt;

    std::vector<ResolvedDevice> matches;
    for (const auto& item : listed["items"])
    {
        if (!item.isObject())
            continue;
        const auto name = item.get("name", "").asString();
        const auto description = item.get("description", "").asString();
        if (!device_query_matches(name, description, device_name))
            continue;

        ResolvedDevice resolved;
        resolved.device_id = item.get("id", "").asString();
        resolved.device_name = name;
        resolved.device_class = item.get("class", "").asString();
        matches.push_back(std::move(resolved));
    }

    if (matches.empty())
    {
        code = "NOT_FOUND";
        return std::nullopt;
    }
    if (matches.size() > 1)
    {
        code = "AMBIGUOUS_DEVICE";
        return std::nullopt;
    }

    code.clear();
    return matches.front();
}

Json::Value DevicesInternalStore::toolListDevices(const Json::Value& body, std::string& code) const
{
    DeviceListFilter filter;
    if (const auto user_id = optional_user_id(body))
        filter.user_id = *user_id;
    if (body.isMember("roomId"))
    {
        if (body["roomId"].isInt64())
            filter.room_id = body["roomId"].asInt64();
        else if (body["roomId"].isInt())
            filter.room_id = body["roomId"].asInt();
    }
    filter.enabled = std::nullopt;

    const auto runtime_id = demo_db_devices_enabled()
        ? std::optional<std::string>(runtime_from_body(body))
        : std::nullopt;
    const Json::Value listed = listDevices(filter, code, runtime_id);
    if (!code.empty())
        return Json::Value();

    Json::Value items(Json::arrayValue);
    for (const auto& item : listed["items"])
    {
        if (!item.isObject())
            continue;

        const auto class_name = item.get("class", "").asString();
        const auto caps = DeviceClassRegistry::capabilities_for_class(class_name);

        Json::Value summary;
        summary["id"] = item.get("id", "");
        summary["name"] = item.get("name", "");
        summary["class"] = class_name;
        summary["connected"] = item.get("connected", false);
        summary["stateSummary"] = item.get("stateSummary", "");

        Json::Value actions(Json::arrayValue);
        if (caps.isMember("actions") && caps["actions"].isArray())
        {
            for (const auto& action : caps["actions"])
            {
                if (action.isObject() && action.isMember("name"))
                    actions.append(action["name"]);
            }
        }
        summary["actions"] = actions;
        items.append(summary);
    }

    Json::Value response;
    response["items"] = items;
    return response;
}

Json::Value DevicesInternalStore::toolControlDevice(const Json::Value& body, std::string& code) const
{
    if (!body.isMember("roomId") || !body.isMember("device") || !body.isMember("action"))
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }

    const int64_t room_id = body["roomId"].isInt64()
        ? body["roomId"].asInt64()
        : static_cast<int64_t>(body["roomId"].asInt());
    const auto runtime_id = demo_db_devices_enabled()
        ? std::optional<std::string>(runtime_from_body(body))
        : std::nullopt;
    const auto resolved = resolveDeviceByName(
        room_id, body["device"].asString(), optional_user_id(body), code, runtime_id);
    if (!resolved)
        return Json::Value();

    Json::Value invoke_body = body;
    invoke_body.removeMember("roomId");
    invoke_body.removeMember("device");

    Json::Value response = invokeAction(resolved->device_id, body["action"].asString(), invoke_body, code);
    if (!code.empty())
        return Json::Value();

    response["deviceName"] = resolved->device_name;
    return response;
}

Json::Value DevicesInternalStore::toolQueryDevice(const Json::Value& body, std::string& code) const
{
    if (!body.isMember("roomId") || !body.isMember("device") || !body.isMember("query"))
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }

    const int64_t room_id = body["roomId"].isInt64()
        ? body["roomId"].asInt64()
        : static_cast<int64_t>(body["roomId"].asInt());
    const auto runtime_id = demo_db_devices_enabled()
        ? std::optional<std::string>(runtime_from_body(body))
        : std::nullopt;
    const auto resolved = resolveDeviceByName(
        room_id, body["device"].asString(), optional_user_id(body), code, runtime_id);
    if (!resolved)
        return Json::Value();

    return queryDevice(resolved->device_id, body["query"].asString(), body, code);
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
