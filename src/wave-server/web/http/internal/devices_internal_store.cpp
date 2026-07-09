#include "devices_internal_store.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <unordered_set>

#include "../../../app/app_state.h"
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
        if (id_value.isString())
            return parseOptionalInt(id_value.asString()) == room_id;
        if (id_value.isInt64())
            return id_value.asInt64() == room_id;
        if (id_value.isInt())
            return id_value.asInt() == room_id;
        return false;
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

    bool devicesReady(std::string& code)
    {
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
}

DevicesInternalStore::DevicesInternalStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
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

    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT 1
FROM device d
JOIN device_user_map m ON m.device_id = d.id
WHERE m.user_id = ? AND d.external_id = ?
LIMIT 1
)SQL",
        user_id,
        external_id);
    if (!rows.empty())
        return true;

    const auto internal_id = internalIdFromExternal(external_id);
    if (!internal_id)
        return false;

    rows = m_client->execSqlSync(
        R"SQL(
SELECT 1
FROM device_user_map
WHERE user_id = ? AND device_id = ?
LIMIT 1
)SQL",
        user_id,
        *internal_id);
    return !rows.empty();
}

std::optional<Json::Value> DevicesInternalStore::findListedDevice(const std::string& device_id) const
{
    std::string code;
    if (!devicesReady(code))
        return std::nullopt;

    const Json::Value listed = makeIotStore().listDevices();
    for (const auto& device : listed)
    {
        if (device.isObject() && device.get("id", "").asString() == device_id)
            return device;
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
    if (!devicesReady(code))
        return Json::Value();

    const Json::Value listed = makeIotStore().listDevices();
    Json::Value items(Json::arrayValue);
    for (const auto& device : listed)
    {
        if (!device.isObject())
            continue;

        if (filter.enabled && !*filter.enabled && device.get("enabled", true).asBool())
            continue;
        if (filter.enabled && *filter.enabled && !device.get("enabled", true).asBool())
            continue;
        if (filter.connected && *filter.connected != device.get("connected", false).asBool())
            continue;
        if (filter.device_class && device.get("class", "").asString() != *filter.device_class)
            continue;
        if (filter.room_id && !roomMatches(device.get("room", Json::nullValue), *filter.room_id))
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

Json::Value DevicesInternalStore::getDevice(
    const std::string& device_id,
    const std::optional<int64_t> user_id,
    std::string& code) const
{
    if (!devicesReady(code))
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
    if (!devicesReady(code))
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

    v1::IotStore store = makeIotStore();
    const auto class_name = (*listed).get("class", "").asString();
    const auto query_name = class_name == "samsung_g7" || class_name == "tizen_tv" ? "state" : "status";
    const auto state = store.queryDevice(device_id, query_name, code);
    if (!code.empty())
        return Json::Value();

    Json::Value body;
    body["deviceId"] = device_id;
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
    if (!devicesReady(code))
        return Json::Value();

    if (const auto user_id = optionalUserId(body))
    {
        if (*user_id > 0 && !deviceAllowedForUser(device_id, *user_id))
        {
            code = "NOT_FOUND";
            return Json::Value();
        }
    }

    if (!findListedDevice(device_id))
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    v1::IotStore store = makeIotStore();
    const auto result = store.queryDevice(device_id, query_name, code);
    if (!code.empty())
        return Json::Value();

    Json::Value response;
    response["deviceId"] = device_id;
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
    if (!devicesReady(code))
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

    const auto result = makeIotStore().invokeDevice(device_id, action_name, params, code);
    if (!code.empty())
        return Json::Value();

    Json::Value response;
    response["ok"] = true;
    response["deviceId"] = device_id;
    response["action"] = action_name;
    if (result.isObject())
        response["state"] = result;
    response["eventId"] = makeEventId();
    return response;
}

Json::Value DevicesInternalStore::listEvents(const EventsListFilter& filter, std::string& code) const
{
    if (!devicesReady(code))
        return Json::Value();

    const int limit = std::max(1, std::min(filter.limit, 200));
    const Json::Value all = makeIotStore().listEvents(filter.device_id.value_or(""));
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
