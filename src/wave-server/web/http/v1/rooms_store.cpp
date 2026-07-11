#include "rooms_store.h"

#include <algorithm>
#include <cctype>

#include "../../../device/device_wire_id.hpp"
#include "session_store.h"
#include "../../../core/logger.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {
namespace
{
    std::string trimCopy(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\n\r");
        return value.substr(start, end - start + 1);
    }
}

RoomsStore::RoomsStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

int64_t RoomsStore::nextRoomId() const
{
    auto rows = m_client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM room");
    return rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();
}

Json::Value RoomsStore::roomJson(const RoomView& room) const
{
    Json::Value value;
    value["id"] = dev::wireIdForDbRow(room.id);
    value["name"] = room.name;
    value["description"] = room.description;
    return value;
}

std::optional<RoomView> RoomsStore::findRoom(int64_t room_id) const
{
    auto rows = m_client->execSqlSync(
        "SELECT id, name, description FROM room WHERE id = ? LIMIT 1",
        room_id);
    if (rows.empty())
        return std::nullopt;

    RoomView room;
    room.id = rows[0]["id"].as<int64_t>();
    room.name = rows[0]["name"].as<std::string>();
    room.description = rows[0]["description"].as<std::string>();
    return room;
}

Json::Value RoomsStore::listRooms() const
{
    Json::Value body(Json::arrayValue);
    auto rows = m_client->execSqlSync("SELECT id, name, description FROM room ORDER BY id");
    for (const auto& row : rows)
    {
        RoomView room;
        room.id = row["id"].as<int64_t>();
        room.name = row["name"].as<std::string>();
        room.description = row["description"].as<std::string>();
        body.append(roomJson(room));
    }
    return body;
}

std::optional<RoomView> RoomsStore::createRoom(
    const std::string& name,
    const std::string& description,
    std::string& error,
    std::string& field)
{
    const auto room_name = trimCopy(name);
    if (room_name.empty())
    {
        error = "방 이름을 입력해주세요.";
        field = "name";
        return std::nullopt;
    }

    const auto desc = trimCopy(description).empty() ? room_name : trimCopy(description);
    const auto id = nextRoomId();
    try
    {
        m_client->execSqlSync(
            "INSERT INTO room (id, name, description) VALUES (?, ?, ?)",
            id,
            room_name,
            desc);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to create room: {}", e.what());
        error = "방 생성에 실패했습니다.";
        return std::nullopt;
    }

    RoomView room;
    room.id = id;
    room.name = room_name;
    room.description = desc;
    return room;
}

std::optional<RoomView> RoomsStore::updateRoom(
    int64_t room_id,
    const Json::Value& body,
    std::string& error,
    std::string& field)
{
    auto existing = findRoom(room_id);
    if (!existing)
    {
        error = "방을 찾을 수 없습니다.";
        return std::nullopt;
    }

    auto next_name = existing->name;
    auto next_desc = existing->description;
    if (body.isMember("name"))
    {
        if (!body["name"].isString())
        {
            error = "방 이름을 입력해주세요.";
            field = "name";
            return std::nullopt;
        }
        next_name = trimCopy(body["name"].asString());
        if (next_name.empty())
        {
            error = "방 이름을 입력해주세요.";
            field = "name";
            return std::nullopt;
        }
    }
    if (body.isMember("description"))
    {
        if (!body["description"].isString())
            next_desc = next_name;
        else
            next_desc = trimCopy(body["description"].asString()).empty() ? next_name : trimCopy(body["description"].asString());
    }

    m_client->execSqlSync(
        "UPDATE room SET name = ?, description = ? WHERE id = ?",
        next_name,
        next_desc,
        room_id);

    RoomView room;
    room.id = room_id;
    room.name = next_name;
    room.description = next_desc;
    return room;
}

bool RoomsStore::deleteRoom(int64_t room_id, std::string& error, std::string& code)
{
    if (!findRoom(room_id))
    {
        error = "방을 찾을 수 없습니다.";
        code = "NOT_FOUND";
        return false;
    }

    auto device_rows = m_client->execSqlSync(
        R"SQL(
SELECT COUNT(*) AS cnt
FROM device_room_map drm
JOIN device d ON d.id = drm.device_id
WHERE drm.room_id = ? AND d.archived = 0
)SQL",
        room_id);
    if (!device_rows.empty() && device_rows[0]["cnt"].as<int64_t>() > 0)
    {
        error = "이 방에 연결된 기기가 있어 삭제할 수 없습니다.";
        code = "ROOM_HAS_DEVICES";
        return false;
    }

    try
    {
        m_client->execSqlSync("DELETE FROM room_user_map WHERE room_id = ?", room_id);
        m_client->execSqlSync("DELETE FROM room WHERE id = ?", room_id);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to delete room: {}", e.what());
        error = "방 삭제에 실패했습니다.";
        code = "DELETE_FAILED";
        return false;
    }
}

Json::Value RoomsStore::listMembers(int64_t room_id) const
{
    Json::Value body(Json::arrayValue);
    if (!findRoom(room_id))
        return body;

    auto rows = m_client->execSqlSync(
        "SELECT user_id FROM room_user_map WHERE room_id = ? ORDER BY user_id",
        room_id);
    for (const auto& row : rows)
        body.append(static_cast<Json::Int64>(row["user_id"].as<int64_t>()));
    return body;
}

std::optional<Json::Value> RoomsStore::updateMembers(
    int64_t room_id,
    const std::vector<int64_t>& account_ids,
    std::string& error,
    std::string& code)
{
    if (!findRoom(room_id))
    {
        error = "방을 찾을 수 없습니다.";
        code = "NOT_FOUND";
        return std::nullopt;
    }

    SessionStore sessions(m_client);
    for (const auto account_id : account_ids)
    {
        if (!sessions.findAccount(account_id))
        {
            error = "구성원을 찾을 수 없습니다.";
            code = "NOT_FOUND";
            return std::nullopt;
        }
    }

    try
    {
        m_client->execSqlSync("DELETE FROM room_user_map WHERE room_id = ?", room_id);
        for (const auto account_id : account_ids)
        {
            m_client->execSqlSync(
                "INSERT INTO room_user_map (room_id, user_id) VALUES (?, ?)",
                room_id,
                account_id);
        }
        return listMembers(room_id);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to update room members: {}", e.what());
        error = "구역 멤버 저장에 실패했습니다.";
        code = "SAVE_FAILED";
        return std::nullopt;
    }
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
