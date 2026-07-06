#include "devices_store.h"

#include <cstdio>
#include <random>
#include <sstream>

#include "../../../core/logger.h"
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
        "ir_reciever",
        "reolink_e1_pro",
        "wave_station",
    };

    std::string trimCopy(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\n\r");
        return value.substr(start, end - start + 1);
    }
}

DevicesStore::DevicesStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

bool DevicesStore::parseJsonText(const std::string& text, Json::Value& out)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(text);
    return Json::parseFromStream(builder, stream, &out, &errors);
}

std::string DevicesStore::jsonToText(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

bool DevicesStore::isInputClass(const std::string& device_class)
{
    for (const auto* cls : kInputClasses)
    {
        if (device_class == cls)
            return true;
    }
    return false;
}

std::string DevicesStore::makeHexId()
{
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    const auto value = dist(rng);
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
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
    device["id"] = row["external_id"].as<std::string>();
    device["name"] = row["name"].as<std::string>();
    device["description"] = row["description"].as<std::string>();
    device["enabled"] = row["enabled"].as<int>() != 0;
    device["class"] = row["class"].as<std::string>();

    if (room_id)
        device["room_id"] = static_cast<Json::Int64>(*room_id);
    else
        device["room_id"] = Json::Value(Json::nullValue);

    Json::Value interface_json;
    if (!parseJsonText(row["interface_json"].as<std::string>(), interface_json))
        interface_json = Json::Value(Json::objectValue);
    device["interface"] = interface_json;

    if (!row["settings_json"].isNull())
    {
        Json::Value settings_json;
        if (parseJsonText(row["settings_json"].as<std::string>(), settings_json))
            device["settings"] = settings_json;
    }

    return device;
}

Json::Value DevicesStore::listDevices() const
{
    Json::Value body;
    Json::Value input(Json::arrayValue);
    Json::Value output(Json::arrayValue);

    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT d.id, d.external_id, d.name, d.description, d.class, d.enabled, d.interface_json, d.settings_json
FROM device d
WHERE d.archived = 0
ORDER BY d.id
)SQL");

    for (const auto& row : rows)
    {
        const auto device_id = row["id"].as<int64_t>();
        const auto room_id = findRoomIdForDevice(device_id);
        const auto device = rowToDeviceJson(row, room_id);
        if (isInputClass(device["class"].asString()))
            input.append(device);
        else
            output.append(device);
    }

    body["input_devices"] = input;
    body["output_devices"] = output;
    return body;
}

std::optional<Json::Value> DevicesStore::findDevice(const std::string& external_id) const
{
    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT d.id, d.external_id, d.name, d.description, d.class, d.enabled, d.interface_json, d.settings_json
FROM device d
WHERE d.external_id = ? AND d.archived = 0
LIMIT 1
)SQL",
        external_id);
    if (rows.empty())
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

    const auto name = trimCopy(body["name"].asString());
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
        room_id = body["room_id"].asInt64();
        if (!roomExists(*room_id))
        {
            error = "방을 찾을 수 없습니다.";
            code = "NOT_FOUND";
            return std::nullopt;
        }
    }

    const auto external_id = body.isMember("id") && body["id"].isString() && !body["id"].asString().empty()
        ? body["id"].asString()
        : makeHexId();
    const auto description = body.isMember("description") && body["description"].isString()
        ? trimCopy(body["description"].asString())
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
INSERT INTO device (id, external_id, name, description, class, archived, enabled, interface_json, settings_json)
VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?)
)SQL",
                internal_id,
                external_id,
                name,
                description.empty() ? name : description,
                device_class,
                enabled ? 1 : 0,
                jsonToText(interface_json),
                jsonToText(settings_json));
        }
        else
        {
            m_client->execSqlSync(
                R"SQL(
INSERT INTO device (id, external_id, name, description, class, archived, enabled, interface_json, settings_json)
VALUES (?, ?, ?, ?, ?, 0, ?, ?, NULL)
)SQL",
                internal_id,
                external_id,
                name,
                description.empty() ? name : description,
                device_class,
                enabled ? 1 : 0,
                jsonToText(interface_json));
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
        LOG_ERROR("Failed to create device: {}", e.what());
        error = "기기 등록에 실패했습니다.";
        code = "CREATE_FAILED";
        return std::nullopt;
    }

    return findDevice(external_id);
}

std::optional<Json::Value> DevicesStore::updateDevice(
    const std::string& external_id,
    const Json::Value& body,
    std::string& error,
    std::string& code)
{
    auto rows = m_client->execSqlSync(
        "SELECT id, external_id, name, description, class, enabled, interface_json, settings_json FROM device WHERE external_id = ? AND archived = 0 LIMIT 1",
        external_id);
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
    parseJsonText(rows[0]["interface_json"].as<std::string>(), interface_json);
    Json::Value settings_json;
    const bool had_settings = !rows[0]["settings_json"].isNull()
        && parseJsonText(rows[0]["settings_json"].as<std::string>(), settings_json);

    if (body.isMember("name"))
    {
        name = trimCopy(body["name"].asString());
        if (name.empty())
        {
            error = "기기 이름을 입력해주세요.";
            code = "INVALID_NAME";
            return std::nullopt;
        }
    }
    if (body.isMember("description") && body["description"].isString())
        description = trimCopy(body["description"].asString()).empty() ? name : trimCopy(body["description"].asString());
    if (body.isMember("class") && body["class"].isString())
        device_class = body["class"].asString();
    if (body.isMember("enabled"))
        enabled = body["enabled"].asBool();
    if (body.isMember("interface"))
        interface_json = body["interface"];
    if (body.isMember("settings"))
        settings_json = body["settings"];

    if (body.isMember("room_id"))
    {
        m_client->execSqlSync("DELETE FROM device_room_map WHERE device_id = ?", internal_id);
        if (!body["room_id"].isNull())
        {
            const auto room_id = body["room_id"].asInt64();
            if (!roomExists(room_id))
            {
                error = "방을 찾을 수 없습니다.";
                code = "NOT_FOUND";
                return std::nullopt;
            }
            m_client->execSqlSync(
                "INSERT INTO device_room_map (device_id, room_id) VALUES (?, ?)",
                internal_id,
                room_id);
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
                jsonToText(interface_json),
                jsonToText(settings_json),
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
                jsonToText(interface_json),
                internal_id);
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to update device: {}", e.what());
        error = "기기 수정에 실패했습니다.";
        code = "UPDATE_FAILED";
        return std::nullopt;
    }

    return findDevice(external_id);
}

bool DevicesStore::deleteDevice(const std::string& external_id, std::string& error)
{
    auto rows = m_client->execSqlSync(
        "SELECT id FROM device WHERE external_id = ? AND archived = 0 LIMIT 1",
        external_id);
    if (rows.empty())
    {
        error = "기기를 찾을 수 없습니다.";
        return false;
    }

    const auto internal_id = rows[0]["id"].as<int64_t>();
    try
    {
        m_client->execSqlSync("DELETE FROM device_room_map WHERE device_id = ?", internal_id);
        m_client->execSqlSync("DELETE FROM device_user_map WHERE device_id = ?", internal_id);
        m_client->execSqlSync("DELETE FROM device WHERE id = ?", internal_id);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to delete device: {}", e.what());
        error = "기기 삭제에 실패했습니다.";
        return false;
    }
}

std::optional<Json::Value> DevicesStore::assignRoom(
    const std::string& external_id,
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

    auto rows = m_client->execSqlSync(
        "SELECT id FROM device WHERE external_id = ? AND archived = 0 LIMIT 1",
        external_id);
    if (rows.empty())
    {
        error = "기기를 찾을 수 없습니다.";
        code = "NOT_FOUND";
        return std::nullopt;
    }

    const auto internal_id = rows[0]["id"].as<int64_t>();
    m_client->execSqlSync("DELETE FROM device_room_map WHERE device_id = ?", internal_id);
    m_client->execSqlSync(
        "INSERT INTO device_room_map (device_id, room_id) VALUES (?, ?)",
        internal_id,
        room_id);
    return findDevice(external_id);
}

std::optional<Json::Value> DevicesStore::unassignRoom(const std::string& external_id, std::string& error)
{
    auto rows = m_client->execSqlSync(
        "SELECT id FROM device WHERE external_id = ? AND archived = 0 LIMIT 1",
        external_id);
    if (rows.empty())
    {
        error = "기기를 찾을 수 없습니다.";
        return std::nullopt;
    }

    m_client->execSqlSync(
        "DELETE FROM device_room_map WHERE device_id = ?",
        rows[0]["id"].as<int64_t>());
    return findDevice(external_id);
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
