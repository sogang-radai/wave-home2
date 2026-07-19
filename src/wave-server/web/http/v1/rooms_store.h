#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

struct RoomView
{
    int64_t id = 0;
    std::string name;
    std::string description;
};

class RoomsStore
{
public:
    explicit RoomsStore(db::DbClientPtr client);

    Json::Value listRooms() const;
    std::optional<RoomView> findRoom(int64_t room_id) const;

    std::optional<RoomView> createRoom(const std::string& name, const std::string& description, std::string& error, std::string& field);
    std::optional<RoomView> updateRoom(int64_t room_id, const Json::Value& body, std::string& error, std::string& field);
    bool deleteRoom(int64_t room_id, std::string& error, std::string& code);

    Json::Value listMembers(int64_t room_id) const;
    std::optional<Json::Value> updateMembers(int64_t room_id, const std::vector<int64_t>& account_ids, std::string& error, std::string& code);

private:
    db::DbClientPtr m_client;

    int64_t nextRoomId() const;
    Json::Value roomJson(const RoomView& room) const;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
