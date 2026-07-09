#include "devices_internal_store.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include "../../../app/app_state.h"
#include "../../../device/device.h"
#include "../../../device/device_manager.h"
#include "device_class_registry.h"
#include "../v1/iot_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    std::optional<int64_t> parseOptionalInt(const std::string& value)
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

    bool roomMatches(const Json::Value& room, int64_t room_id)
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
            const auto parsed = parseOptionalInt(id_value.asString());
            return parsed && *parsed == room_id;
        }
        return false;
    }

    bool manifestHasDevice(const std::string& manifest_id)
    {
        if (manifest_id.empty())
            return false;

        for (const auto& entry : AppState::get().deviceManager.manifestEntries())
        {
            if (entry.config.value("id", "") == manifest_id)
                return true;
        }
        return false;
    }

    std::optional<std::string> manifestIdForDeviceName(const std::string& name)
    {
        for (const auto& entry : AppState::get().deviceManager.manifestEntries())
        {
            if (entry.config.value("name", "") == name)
            {
                const auto id = entry.config.value("id", "");
                if (!id.empty())
                    return id;
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> manifestIdForDbRow(
        const drogon::orm::DbClientPtr& client,
        int64_t db_id)
    {
        if (!client || db_id <= 0)
            return std::nullopt;

        auto rows = client->execSqlSync(
            "SELECT external_id, name FROM device WHERE id = ? LIMIT 1",
            db_id);
        if (rows.empty())
            return std::nullopt;

        if (!rows[0]["external_id"].isNull())
        {
            const auto ext = rows[0]["external_id"].as<std::string>();
            if (!ext.empty() && manifestHasDevice(ext))
                return ext;
        }

        return manifestIdForDeviceName(rows[0]["name"].as<std::string>());
    }

    std::optional<int64_t> optionalUserId(const Json::Value& body)
    {
        if (body.isMember("userId") && body["userId"].isInt64())
            return body["userId"].asInt64();
        if (body.isMember("userId") && body["userId"].isInt())
            return body["userId"].asInt();
        return std::nullopt;
    }

    bool containsType(const std::vector<std::string>& types, const std::string& type)
    {
        return types.empty()
            || std::any_of(types.begin(), types.end(), [&](const std::string& candidate) {
                   return candidate == type;
               });
    }

    v1::IotStore makeIotStore()
    {
        return v1::IotStore(AppState::get().deviceManager);
    }

    bool demoDbDevicesEnabled()
    {
        const auto& state = AppState::get();
        return state.demo_mode && state.no_devices;
    }

    std::string wireIdFromDbRow(int64_t db_id, const std::string& external_id)
    {
        if (!external_id.empty())
            return external_id;

        std::ostringstream stream;
        stream << std::hex << std::nouppercase << std::setw(16) << std::setfill('0') << db_id;
        return stream.str();
    }

    struct DbDeviceRow
    {
        int64_t id = 0;
        std::string external_id;
        std::string name;
        std::string description;
        std::string device_class;
        bool enabled = true;
        int64_t room_id = 0;
        std::string room_name;
    };

    std::optional<DbDeviceRow> rowFromResult(const drogon::orm::Row& row)
    {
        DbDeviceRow out;
        out.id = row["id"].as<int64_t>();
        out.external_id = row["external_id"].isNull() ? std::string() : row["external_id"].as<std::string>();
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

    std::optional<DbDeviceRow> lookupDbDeviceRow(
        const drogon::orm::DbClientPtr& client,
        const std::string& device_id_param)
    {
        if (!client || device_id_param.empty())
            return std::nullopt;

        auto rows = client->execSqlSync(
            R"SQL(
SELECT d.id, d.external_id, d.name, d.description, d.class, d.enabled,
       r.id AS room_id, r.name AS room_name
FROM device d
LEFT JOIN device_room_map drm ON drm.device_id = d.id
LEFT JOIN room r ON r.id = drm.room_id
WHERE d.archived = 0 AND d.external_id = ?
LIMIT 1
)SQL",
            device_id_param);
        if (!rows.empty())
            return rowFromResult(rows[0]);

        const auto parsed = dev::parseDeviceID(device_id_param);
        if (parsed != 0 && parsed < 1'000'000)
        {
            rows = client->execSqlSync(
                R"SQL(
SELECT d.id, d.external_id, d.name, d.description, d.class, d.enabled,
       r.id AS room_id, r.name AS room_name
FROM device d
LEFT JOIN device_room_map drm ON drm.device_id = d.id
LEFT JOIN room r ON r.id = drm.room_id
WHERE d.archived = 0 AND d.id = ?
LIMIT 1
)SQL",
                static_cast<int64_t>(parsed));
            if (!rows.empty())
                return rowFromResult(rows[0]);
        }

        if (const auto as_int = parseOptionalInt(device_id_param))
        {
            if (*as_int > 0)
            {
                rows = client->execSqlSync(
                    R"SQL(
SELECT d.id, d.external_id, d.name, d.description, d.class, d.enabled,
       r.id AS room_id, r.name AS room_name
FROM device d
LEFT JOIN device_room_map drm ON drm.device_id = d.id
LEFT JOIN room r ON r.id = drm.room_id
WHERE d.archived = 0 AND d.id = ?
LIMIT 1
)SQL",
                    *as_int);
                if (!rows.empty())
                    return rowFromResult(rows[0]);
            }
        }

        return std::nullopt;
    }

    Json::Value deviceJsonFromDbRow(const DbDeviceRow& row)
    {
        Json::Value item;
        item["id"] = wireIdFromDbRow(row.id, row.external_id);
        item["name"] = row.name;
        item["description"] = row.description;
        item["class"] = row.device_class;
        item["classLabel"] = DeviceClassRegistry::labelForClass(row.device_class);
        item["vendor"] = "";
        item["model"] = "";
        item["enabled"] = row.enabled;
        item["connected"] = false;
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

    bool devicesReady(const drogon::orm::DbClientPtr& client, std::string& code)
    {
        if (demoDbDevicesEnabled())
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

    std::optional<std::string> resolveManifestDeviceId(
        const drogon::orm::DbClientPtr& client,
        const std::string& device_id_param)
    {
        if (device_id_param.empty())
            return std::nullopt;

        if (manifestHasDevice(device_id_param))
            return device_id_param;

        const auto parsed = dev::parseDeviceID(device_id_param);
        if (parsed != 0)
        {
            if (parsed < 1'000'000)
            {
                if (const auto from_db = manifestIdForDbRow(client, static_cast<int64_t>(parsed)))
                    return from_db;
            }

            const auto from_parsed = dev::deviceIDToString(parsed);
            if (manifestHasDevice(from_parsed))
                return from_parsed;
        }

        if (const auto as_int = parseOptionalInt(device_id_param))
        {
            if (*as_int > 0 && *as_int < 1'000'000)
            {
                if (const auto from_db = manifestIdForDbRow(client, *as_int))
                    return from_db;
            }
        }

        if (demoDbDevicesEnabled() && client)
        {
            if (const auto row = lookupDbDeviceRow(client, device_id_param))
                return wireIdFromDbRow(row->id, row->external_id);
        }

        return std::nullopt;
    }

    std::optional<Json::Value> lookupDbRoomRef(
        const drogon::orm::DbClientPtr& client,
        const std::string& external_id,
        const std::string& device_name,
        const Json::Value& manifest_room)
    {
        if (!client)
            return std::nullopt;

        auto query_by_external = [&]()
        {
            if (external_id.empty())
                return drogon::orm::Result(nullptr);
            return client->execSqlSync(
                R"SQL(
SELECT r.id, r.name
FROM device d
JOIN device_room_map drm ON drm.device_id = d.id
JOIN room r ON r.id = drm.room_id
WHERE d.external_id = ?
LIMIT 1
)SQL",
                external_id);
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

        auto rows = query_by_external();
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

    void applyDbRoomRef(
        Json::Value& device,
        const drogon::orm::DbClientPtr& client)
    {
        if (!device.isObject())
            return;

        const auto external_id = device.get("id", "").asString();
        const auto device_name = device.get("name", "").asString();
        const auto manifest_room = device.get("room", Json::nullValue);
        const auto db_room = lookupDbRoomRef(client, external_id, device_name, manifest_room);
        if (db_room)
            device["room"] = *db_room;
    }
}

DevicesInternalStore::DevicesInternalStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::optional<std::string> DevicesInternalStore::resolveWireDeviceId(
    const drogon::orm::DbClientPtr& client,
    const std::string& device_id_param)
{
    return resolveManifestDeviceId(client, device_id_param);
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
    if (demoDbDevicesEnabled())
    {
        if (const auto row = lookupDbDeviceRow(m_client, external_id))
            db_row_id = row->id;
    }
    else
    {
        const auto manifest_id = resolveManifestDeviceId(m_client, external_id);
        if (!manifest_id)
            return false;

        auto rows = m_client->execSqlSync(
            R"SQL(
SELECT 1
FROM device d
JOIN device_user_map m ON m.device_id = d.id
WHERE m.user_id = ? AND d.external_id = ?
LIMIT 1
)SQL",
            user_id,
            *manifest_id);
        if (!rows.empty())
            return true;

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

std::optional<Json::Value> DevicesInternalStore::findListedDevice(const std::string& device_id) const
{
    std::string code;
    if (!devicesReady(m_client, code))
        return std::nullopt;

    if (demoDbDevicesEnabled())
    {
        if (const auto row = lookupDbDeviceRow(m_client, device_id))
            return deviceJsonFromDbRow(*row);
        return std::nullopt;
    }

    const auto manifest_id = resolveManifestDeviceId(m_client, device_id);
    if (!manifest_id)
        return std::nullopt;

    const Json::Value listed = makeIotStore().listDevices();
    for (const auto& device : listed)
    {
        if (device.isObject() && device.get("id", "").asString() == *manifest_id)
        {
            Json::Value enriched = device;
            applyDbRoomRef(enriched, m_client);
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
        "SELECT external_id, name FROM device WHERE id = ?",
        device_row_id);
    if (rows.empty())
        return {};

    const auto external_id = rows[0]["external_id"].isNull()
        ? std::string()
        : rows[0]["external_id"].as<std::string>();
    if (!external_id.empty())
        return external_id;

    const auto name = rows[0]["name"].as<std::string>();
    for (const auto& entry : AppState::get().deviceManager.manifestEntries())
    {
        if (entry.config.value("name", "") == name)
            return entry.config.value("id", "");
    }
    return {};
}

std::optional<int64_t> DevicesInternalStore::internalIdFromExternal(const std::string& external_id) const
{
    if (!m_client || external_id.empty())
        return std::nullopt;

    auto rows = m_client->execSqlSync(
        "SELECT id FROM device WHERE external_id = ? LIMIT 1",
        external_id);
    if (!rows.empty())
        return rows[0]["id"].as<int64_t>();

    for (const auto& entry : AppState::get().deviceManager.manifestEntries())
    {
        if (entry.config.value("id", "") != external_id)
            continue;
        const auto name = entry.config.value("name", "");
        rows = m_client->execSqlSync(
            "SELECT id FROM device WHERE name = ? LIMIT 1",
            name);
        if (!rows.empty())
            return rows[0]["id"].as<int64_t>();
    }
    return std::nullopt;
}

bool DevicesInternalStore::nameMatches(const std::string& haystack, const std::string& needle)
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

std::string DevicesInternalStore::makeEventId()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::ostringstream stream;
    stream << "evt_" << ms;
    return stream.str();
}

Json::Value DevicesInternalStore::listDevices(const DeviceListFilter& filter, std::string& code) const
{
    if (!devicesReady(m_client, code))
        return Json::Value();

    if (demoDbDevicesEnabled())
    {
        const auto rows = m_client->execSqlSync(
            R"SQL(
SELECT d.id, d.external_id, d.name, d.description, d.class, d.enabled,
       r.id AS room_id, r.name AS room_name
FROM device d
LEFT JOIN device_room_map drm ON drm.device_id = d.id
LEFT JOIN room r ON r.id = drm.room_id
WHERE d.archived = 0
ORDER BY d.id
)SQL");

        Json::Value items(Json::arrayValue);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto row = rowFromResult(rows[i]);
            if (!row)
                continue;

            if (filter.enabled && *filter.enabled != row->enabled)
                continue;
            if (filter.connected && *filter.connected)
                continue;
            if (filter.device_class && row->device_class != *filter.device_class)
                continue;
            if (filter.room_id && row->room_id != *filter.room_id)
                continue;

            const auto wire_id = wireIdFromDbRow(row->id, row->external_id);
            if (filter.user_id && !deviceAllowedForUser(wire_id, *filter.user_id))
                continue;

            items.append(deviceJsonFromDbRow(*row));
        }

        Json::Value body;
        body["items"] = items;
        body["count"] = static_cast<Json::UInt>(items.size());
        return body;
    }

    const Json::Value listed = makeIotStore().listDevices();
    Json::Value items(Json::arrayValue);
    for (const auto& device : listed)
    {
        if (!device.isObject())
            continue;

        Json::Value normalized = device;
        applyDbRoomRef(normalized, m_client);

        if (filter.enabled && !*filter.enabled && normalized.get("enabled", true).asBool())
            continue;
        if (filter.enabled && *filter.enabled && !normalized.get("enabled", true).asBool())
            continue;
        if (filter.connected && *filter.connected != normalized.get("connected", false).asBool())
            continue;
        if (filter.device_class && normalized.get("class", "").asString() != *filter.device_class)
            continue;
        if (filter.room_id && !roomMatches(normalized.get("room", Json::nullValue), *filter.room_id))
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
    std::string& code) const
{
    if (!devicesReady(m_client, code))
        return Json::Value();

    if (user_id && !deviceAllowedForUser(device_id, *user_id))
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const auto listed = findListedDevice(device_id);
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
    body["capabilities"] = DeviceClassRegistry::capabilitiesForClass(body.get("class", "").asString());
    return body;
}

Json::Value DevicesInternalStore::getState(
    const std::string& device_id,
    const std::optional<int64_t> user_id,
    std::string& code) const
{
    if (!devicesReady(m_client, code))
        return Json::Value();

    if (user_id && !deviceAllowedForUser(device_id, *user_id))
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const auto listed = findListedDevice(device_id);
    if (!listed)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    if (demoDbDevicesEnabled())
    {
        Json::Value body;
        body["deviceId"] = (*listed).get("id", "");
        body["connected"] = false;
        body["state"] = Json::objectValue;
        return body;
    }

    v1::IotStore store = makeIotStore();
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
    if (!devicesReady(m_client, code))
        return Json::Value();

    if (const auto user_id = optionalUserId(body))
    {
        if (*user_id > 0 && !deviceAllowedForUser(device_id, *user_id))
        {
            code = "NOT_FOUND";
            return Json::Value();
        }
    }

    const auto listed = findListedDevice(device_id);
    if (!listed)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const std::string manifest_id = (*listed).get("id", "").asString();

    if (demoDbDevicesEnabled())
    {
        Json::Value response;
        response["deviceId"] = manifest_id;
        response["query"] = query_name;
        response["result"] = Json::objectValue;
        return response;
    }

    v1::IotStore store = makeIotStore();
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
    if (!devicesReady(m_client, code))
        return Json::Value();

    if (const auto user_id = optionalUserId(body))
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
            "userId", "reason", "execMode", "triggeredBy", "params", "repeatIntervalMs"};
        for (const auto& key : body.getMemberNames())
        {
            if (k_meta.count(key) == 0)
                params[key] = body[key];
        }
    }

    const auto listed = findListedDevice(device_id);
    if (!listed)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const std::string manifest_id = (*listed).get("id", "").asString();

    if (demoDbDevicesEnabled())
    {
        Json::Value response;
        response["ok"] = true;
        response["deviceId"] = manifest_id;
        response["action"] = action_name;
        response["state"] = Json::objectValue;
        response["eventId"] = makeEventId();
        return response;
    }

    const auto result = makeIotStore().invokeDevice(manifest_id, action_name, params, code);
    if (!code.empty())
        return Json::Value();

    Json::Value response;
    response["ok"] = true;
    response["deviceId"] = manifest_id;
    response["action"] = action_name;
    if (result.isObject())
        response["state"] = result;
    response["eventId"] = makeEventId();
    return response;
}

Json::Value DevicesInternalStore::listEvents(const EventsListFilter& filter, std::string& code) const
{
    if (!devicesReady(m_client, code))
        return Json::Value();

    if (demoDbDevicesEnabled())
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
        if (const auto manifest_id = resolveManifestDeviceId(m_client, *filter.device_id))
            event_device_id = *manifest_id;
        else
            event_device_id = *filter.device_id;
    }
    const Json::Value all = makeIotStore().listEvents(event_device_id);
    Json::Value items(Json::arrayValue);

    for (const auto& event : all)
    {
        if (!event.isObject())
            continue;
        const auto type = event.get("type", "").asString();
        if (!containsType(filter.types, type))
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
    std::string& code) const
{
    DeviceListFilter filter;
    filter.room_id = room_id;
    filter.enabled = std::nullopt;
    if (user_id)
        filter.user_id = *user_id;

    const Json::Value listed = listDevices(filter, code);
    if (!code.empty())
        return std::nullopt;

    std::vector<ResolvedDevice> matches;
    for (const auto& item : listed["items"])
    {
        if (!item.isObject())
            continue;
        const auto name = item.get("name", "").asString();
        if (!nameMatches(name, device_name))
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
    if (const auto user_id = optionalUserId(body))
        filter.user_id = *user_id;
    if (body.isMember("roomId"))
    {
        if (body["roomId"].isInt64())
            filter.room_id = body["roomId"].asInt64();
        else if (body["roomId"].isInt())
            filter.room_id = body["roomId"].asInt();
    }
    filter.enabled = std::nullopt;

    const Json::Value listed = listDevices(filter, code);
    if (!code.empty())
        return Json::Value();

    Json::Value items(Json::arrayValue);
    for (const auto& item : listed["items"])
    {
        if (!item.isObject())
            continue;

        const auto class_name = item.get("class", "").asString();
        const auto caps = DeviceClassRegistry::capabilitiesForClass(class_name);

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
    const auto resolved = resolveDeviceByName(room_id, body["device"].asString(), optionalUserId(body), code);
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
    const auto resolved = resolveDeviceByName(room_id, body["device"].asString(), optionalUserId(body), code);
    if (!resolved)
        return Json::Value();

    return queryDevice(resolved->device_id, body["query"].asString(), body, code);
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
