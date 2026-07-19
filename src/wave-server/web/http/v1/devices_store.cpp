#include "devices_store.h"
#include "../../../db/database.h"

#include <cstdio>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../core/logger.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../device/device.h"
#include "../../../device/device_wire_id.hpp"
#include "../../../service/power_manager.h"
#include "rooms_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {
namespace
{
    const char* kInputClasses[] = {
        "srs_r4sn",
        "wave_mic",
        "wave_cam",
        "reolink_e1_pro",
        "droid_cam",
        "wave_station",
    };

    std::string trim_copy(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\n\r");
        return value.substr(start, end - start + 1);
    }

    Json::Value nlohmann_to_json_value(const json& value)
    {
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream stream(value.dump());
        Json::Value out;
        if (!Json::parseFromStream(builder, stream, &out, &errors))
            return Json::Value(Json::objectValue);
        return out;
    }

    Json::Value manifest_config_to_device_json(const json& cfg)
    {
        Json::Value device;
        device["id"] = cfg.value("id", std::string());
        device["name"] = cfg.value("name", std::string());
        device["description"] = cfg.value("description", std::string());
        device["enabled"] = cfg.value("enabled", true);
        device["class"] = cfg.value("class", std::string());
        device["room_id"] = Json::nullValue;

        if (cfg.contains("interface") && cfg["interface"].is_object())
            device["interface"] = nlohmann_to_json_value(cfg["interface"]);
        else
            device["interface"] = Json::Value(Json::objectValue);

        if (cfg.contains("settings") && cfg["settings"].is_object())
            device["settings"] = nlohmann_to_json_value(cfg["settings"]);

        return device;
    }

    bool is_input_class_local(const std::string& device_class)
    {
        for (const auto* cls : kInputClasses)
        {
            if (device_class == cls)
                return true;
        }
        return false;
    }

    void append_device_to_buckets(
        const Json::Value& device,
        Json::Value& input,
        Json::Value& output)
    {
        if (is_input_class_local(device["class"].asString()))
            input.append(device);
        else
            output.append(device);
    }

    std::optional<int64_t> resolve_room_id_from_json(const Json::Value& value)
    {
        if (value.isString())
        {
            const auto parsed = dev::parseRoomID(value.asString());
            if (parsed == 0)
                return std::nullopt;
            return static_cast<int64_t>(parsed);
        }
        if (value.isInt64())
            return value.asInt64();
        if (value.isInt())
            return value.asInt();
        return std::nullopt;
    }
}

DevicesStore::DevicesStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

bool DevicesStore::parse_json_text(const std::string& text, Json::Value& out)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(text);
    return Json::parseFromStream(builder, stream, &out, &errors);
}

std::string DevicesStore::json_to_text(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

bool DevicesStore::is_input_class(const std::string& device_class)
{
    for (const auto* cls : kInputClasses)
    {
        if (device_class == cls)
            return true;
    }
    return false;
}

std::string DevicesStore::makeHexId() const
{
    auto rows = m_client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM device");
    const auto next_id = rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();
    return dev::wireIdForDbRow(next_id);
}

bool DevicesStore::roomExists(int64_t room_id) const
{
    RoomsStore rooms(m_client);
    return rooms.findRoom(room_id).has_value();
}

std::optional<int64_t> DevicesStore::findRoomIdForDevice(int64_t device_id) const
{
    auto rows = m_client->execSqlSync(
        "SELECT room_id FROM device_room_map WHERE device_id = ? LIMIT 1",
        device_id);
    if (rows.empty())
        return std::nullopt;
    return rows[0]["room_id"].as<int64_t>();
}

Json::Value DevicesStore::rowToDeviceJson(const drogon::orm::Row& row, std::optional<int64_t> room_id) const
{
    Json::Value device;
    const auto db_id = row["id"].as<int64_t>();
    device["id"] = dev::wireIdForDbRow(db_id, row["name"].as<std::string>());
    device["name"] = row["name"].as<std::string>();
    device["description"] = row["description"].as<std::string>();
    device["enabled"] = row["enabled"].as<int>() != 0;
    device["class"] = row["class"].as<std::string>();

    if (room_id)
        device["room_id"] = dev::wireIdForDbRow(*room_id);
    else
        device["room_id"] = Json::Value(Json::nullValue);

    Json::Value interface_json;
    if (!parse_json_text(row["interface_json"].as<std::string>(), interface_json))
        interface_json = Json::Value(Json::objectValue);
    device["interface"] = interface_json;

    if (!row["settings_json"].isNull())
    {
        Json::Value settings_json;
        if (parse_json_text(row["settings_json"].as<std::string>(), settings_json))
            device["settings"] = settings_json;
    }

    return device;
}

Json::Value DevicesStore::listDevices() const
{
    Json::Value body;
    Json::Value input(Json::arrayValue);
    Json::Value output(Json::arrayValue);

    std::unordered_map<int64_t, Json::Value> db_by_id;

    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT d.id, d.name, d.description, d.class, d.enabled, d.interface_json, d.settings_json
FROM device d
WHERE d.archived = 0
ORDER BY d.id
)SQL");

    const bool hide_demo_cameras = demoVirtualDevicesEnabled();
    for (const auto& row : rows)
    {
        const auto device_class = row["class"].as<std::string>();
        if (hide_demo_cameras && isDemoHiddenDeviceClass(device_class))
            continue;
        const auto device_id = row["id"].as<int64_t>();
        const auto room_id = findRoomIdForDevice(device_id);
        db_by_id.emplace(device_id, rowToDeviceJson(row, room_id));
    }

    std::unordered_set<std::string> emitted;

    const auto& manifest = ws::AppState::get().deviceManager.manifestEntries();
    for (const auto& entry : manifest)
    {
        const auto wire_id = entry.config.value("id", std::string());
        if (wire_id.empty())
            continue;

        Json::Value device;
        if (const auto db_id = dev::dbIdForWireId(m_client, wire_id))
        {
            const auto db_it = db_by_id.find(*db_id);
            if (db_it != db_by_id.end())
                device = db_it->second;
        }
        if (!device.isObject() || device.empty())
            device = manifest_config_to_device_json(entry.config);

        if (hide_demo_cameras && isDemoHiddenDeviceClass(device.get("class", "").asString()))
            continue;

        emitted.insert(wire_id);
        append_device_to_buckets(device, input, output);
    }

    for (const auto& [db_id, device] : db_by_id)
    {
        const auto wire_id = device.get("id", "").asString();
        if (emitted.contains(wire_id))
            continue;
        (void)db_id;
        if (hide_demo_cameras && isDemoHiddenDeviceClass(device.get("class", "").asString()))
            continue;
        append_device_to_buckets(device, input, output);
    }

    body["input_devices"] = input;
    body["output_devices"] = output;
    return body;
}

std::optional<Json::Value> DevicesStore::findDevice(const std::string& wire_id) const
{
    const auto db_id = dev::dbIdForWireId(m_client, wire_id);
    if (!db_id)
        return std::nullopt;

    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT d.id, d.name, d.description, d.class, d.enabled, d.interface_json, d.settings_json
FROM device d
WHERE d.id = ? AND d.archived = 0
LIMIT 1
)SQL",
        *db_id);
    if (rows.empty())
        return std::nullopt;

    if (demoVirtualDevicesEnabled()
        && isDemoHiddenDeviceClass(rows[0]["class"].as<std::string>()))
        return std::nullopt;

    const auto device_id = rows[0]["id"].as<int64_t>();
    return rowToDeviceJson(rows[0], findRoomIdForDevice(device_id));
}

std::optional<Json::Value> DevicesStore::createDevice(
    const Json::Value& body,
    std::string& error,
    std::string& field,
    std::string& code)
{
    if (!body.isMember("name") || !body["name"].isString())
    {
        error = "기기 이름을 입력해주세요.";
        field = "name";
        code = "INVALID_NAME";
        return std::nullopt;
    }

    const auto name = trim_copy(body["name"].asString());
    if (name.empty())
    {
        error = "기기 이름을 입력해주세요.";
        field = "name";
        code = "INVALID_NAME";
        return std::nullopt;
    }

    if (!body.isMember("class") || !body["class"].isString() || body["class"].asString().empty())
    {
        error = "기기 class를 입력해주세요.";
        field = "class";
        code = "INVALID_DEVICE_CLASS";
        return std::nullopt;
    }

    std::optional<int64_t> room_id;
    if (body.isMember("room_id") && !body["room_id"].isNull())
    {
        room_id = resolve_room_id_from_json(body["room_id"]);
        if (!room_id || !roomExists(*room_id))
        {
            error = "방을 찾을 수 없습니다.";
            code = "NOT_FOUND";
            return std::nullopt;
        }
    }

    const auto wire_id = body.isMember("id") && body["id"].isString() && !body["id"].asString().empty()
        ? body["id"].asString()
        : makeHexId();
    const auto description = body.isMember("description") && body["description"].isString()
        ? trim_copy(body["description"].asString())
        : name;
    const auto device_class = body["class"].asString();
    const bool enabled = !body.isMember("enabled") || body["enabled"].asBool();
    const auto interface_json = body.isMember("interface") ? body["interface"] : Json::Value(Json::objectValue);
    const bool has_settings = body.isMember("settings");
    const auto settings_json = has_settings ? body["settings"] : Json::Value();

    auto id_rows = m_client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM device");
    const auto internal_id = id_rows.empty() ? 1 : id_rows[0]["next_id"].as<int64_t>();

    try
    {
        if (has_settings)
        {
            m_client->execSqlSync(
                R"SQL(
INSERT INTO device (id, name, description, class, archived, enabled, interface_json, settings_json)
VALUES (?, ?, ?, ?, 0, ?, ?, ?)
)SQL",
                internal_id,
                name,
                description.empty() ? name : description,
                device_class,
                enabled ? 1 : 0,
                json_to_text(interface_json),
                json_to_text(settings_json));
        }
        else
        {
            m_client->execSqlSync(
                R"SQL(
INSERT INTO device (id, name, description, class, archived, enabled, interface_json, settings_json)
VALUES (?, ?, ?, ?, 0, ?, ?, NULL)
)SQL",
                internal_id,
                name,
                description.empty() ? name : description,
                device_class,
                enabled ? 1 : 0,
                json_to_text(interface_json));
        }

        if (room_id)
        {
            m_client->execSqlSync(
                "INSERT INTO device_room_map (device_id, room_id) VALUES (?, ?)",
                internal_id,
                *room_id);
        }
    }
    catch (const std::exception& e)
    {
        WLOG_ERROR("Failed to create device: {}", e.what());
        error = "기기 등록에 실패했습니다.";
        code = "CREATE_FAILED";
        return std::nullopt;
    }

    if (dev::manifestHasWireId(wire_id))
        return findDevice(wire_id);

    auto created = m_client->execSqlSync(
        R"SQL(
SELECT d.id, d.name, d.description, d.class, d.enabled, d.interface_json, d.settings_json
FROM device d
WHERE d.id = ?
LIMIT 1
)SQL",
        internal_id);
    if (created.empty())
        return std::nullopt;

    auto device = rowToDeviceJson(created[0], room_id);
    if (!dev::manifestHasWireId(wire_id))
        device["id"] = wire_id;
    return device;
}

std::optional<Json::Value> DevicesStore::updateDevice(
    const std::string& wire_id,
    const Json::Value& body,
    std::string& error,
    std::string& code)
{
    const auto db_id = dev::dbIdForWireId(m_client, wire_id);
    if (!db_id)
    {
        error = "기기를 찾을 수 없습니다.";
        code = "NOT_FOUND";
        return std::nullopt;
    }

    auto rows = m_client->execSqlSync(
        "SELECT id, name, description, class, enabled, interface_json, settings_json FROM device WHERE id = ? AND archived = 0 LIMIT 1",
        *db_id);
    if (rows.empty())
    {
        error = "기기를 찾을 수 없습니다.";
        code = "NOT_FOUND";
        return std::nullopt;
    }

    const auto internal_id = rows[0]["id"].as<int64_t>();
    auto name = rows[0]["name"].as<std::string>();
    auto description = rows[0]["description"].as<std::string>();
    auto device_class = rows[0]["class"].as<std::string>();
    auto enabled = rows[0]["enabled"].as<int>() != 0;

    Json::Value interface_json;
    parse_json_text(rows[0]["interface_json"].as<std::string>(), interface_json);
    Json::Value settings_json;
    const bool had_settings = !rows[0]["settings_json"].isNull()
        && parse_json_text(rows[0]["settings_json"].as<std::string>(), settings_json);

    if (body.isMember("name"))
    {
        name = trim_copy(body["name"].asString());
        if (name.empty())
        {
            error = "기기 이름을 입력해주세요.";
            code = "INVALID_NAME";
            return std::nullopt;
        }
    }
    if (body.isMember("description") && body["description"].isString())
        description = trim_copy(body["description"].asString()).empty() ? name : trim_copy(body["description"].asString());
    if (body.isMember("class") && body["class"].isString())
        device_class = body["class"].asString();
    if (body.isMember("enabled"))
        enabled = body["enabled"].asBool();
    if (body.isMember("interface"))
        interface_json = body["interface"];
    if (body.isMember("settings") && body["settings"].isObject())
    {
        if (!had_settings)
            settings_json = Json::Value(Json::objectValue);
        for (const auto& name : body["settings"].getMemberNames())
            settings_json[name] = body["settings"][name];
    }

    if (body.isMember("room_id"))
    {
        m_client->execSqlSync("DELETE FROM device_room_map WHERE device_id = ?", internal_id);
        if (!body["room_id"].isNull())
        {
            const auto room_id = resolve_room_id_from_json(body["room_id"]);
            if (!room_id || !roomExists(*room_id))
            {
                error = "방을 찾을 수 없습니다.";
                code = "NOT_FOUND";
                return std::nullopt;
            }
            m_client->execSqlSync(
                "INSERT INTO device_room_map (device_id, room_id) VALUES (?, ?)",
                internal_id,
                *room_id);
        }
    }

    const bool has_settings = body.isMember("settings") || had_settings;
    try
    {
        if (has_settings)
        {
            m_client->execSqlSync(
                R"SQL(
UPDATE device
SET name = ?, description = ?, class = ?, enabled = ?, interface_json = ?, settings_json = ?
WHERE id = ?
)SQL",
                name,
                description,
                device_class,
                enabled ? 1 : 0,
                json_to_text(interface_json),
                json_to_text(settings_json),
                internal_id);
        }
        else
        {
            m_client->execSqlSync(
                R"SQL(
UPDATE device
SET name = ?, description = ?, class = ?, enabled = ?, interface_json = ?
WHERE id = ?
)SQL",
                name,
                description,
                device_class,
                enabled ? 1 : 0,
                json_to_text(interface_json),
                internal_id);
        }
    }
    catch (const std::exception& e)
    {
        WLOG_ERROR("Failed to update device: {}", e.what());
        error = "기기 수정에 실패했습니다.";
        code = "UPDATE_FAILED";
        return std::nullopt;
    }

    if (body.isMember("settings") && device_class == "tuya_ep2h")
    {
        const bool metering = !settings_json.isMember("metering") || settings_json["metering"].asBool();
        service::PowerManager::get().setMeteringEnabled(wire_id, metering);
    }

    return findDevice(wire_id);
}

bool DevicesStore::deleteDevice(const std::string& wire_id, std::string& error)
{
    const auto db_id = dev::dbIdForWireId(m_client, wire_id);
    if (!db_id)
    {
        error = "기기를 찾을 수 없습니다.";
        return false;
    }

    const auto internal_id = *db_id;
    try
    {
        m_client->execSqlSync("DELETE FROM device_room_map WHERE device_id = ?", internal_id);
        m_client->execSqlSync("DELETE FROM device_user_map WHERE device_id = ?", internal_id);
        m_client->execSqlSync("DELETE FROM device WHERE id = ?", internal_id);
        return true;
    }
    catch (const std::exception& e)
    {
        WLOG_ERROR("Failed to delete device: {}", e.what());
        error = "기기 삭제에 실패했습니다.";
        return false;
    }
}

std::optional<Json::Value> DevicesStore::assignRoom(
    const std::string& wire_id,
    int64_t room_id,
    std::string& error,
    std::string& code)
{
    if (!roomExists(room_id))
    {
        error = "방을 찾을 수 없습니다.";
        code = "NOT_FOUND";
        return std::nullopt;
    }

    const auto db_id = dev::dbIdForWireId(m_client, wire_id);
    if (!db_id)
    {
        error = "기기를 찾을 수 없습니다.";
        code = "NOT_FOUND";
        return std::nullopt;
    }

    const auto internal_id = *db_id;
    m_client->execSqlSync("DELETE FROM device_room_map WHERE device_id = ?", internal_id);
    m_client->execSqlSync(
        "INSERT INTO device_room_map (device_id, room_id) VALUES (?, ?)",
        internal_id,
        room_id);
    return findDevice(wire_id);
}

std::optional<Json::Value> DevicesStore::unassignRoom(const std::string& wire_id, std::string& error)
{
    const auto db_id = dev::dbIdForWireId(m_client, wire_id);
    if (!db_id)
    {
        error = "기기를 찾을 수 없습니다.";
        return std::nullopt;
    }

    m_client->execSqlSync(
        "DELETE FROM device_room_map WHERE device_id = ?",
        *db_id);
    return findDevice(wire_id);
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
