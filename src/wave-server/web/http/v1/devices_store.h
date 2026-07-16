#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class DevicesStore
{
public:
    explicit DevicesStore(db::DbClientPtr client);

    Json::Value listDevices() const;
    std::optional<Json::Value> findDevice(const std::string& external_id) const;

    std::optional<Json::Value> createDevice(const Json::Value& body, std::string& error, std::string& field, std::string& code);
    std::optional<Json::Value> updateDevice(const std::string& external_id, const Json::Value& body, std::string& error, std::string& code);
    bool deleteDevice(const std::string& external_id, std::string& error);

    std::optional<Json::Value> assignRoom(const std::string& external_id, int64_t room_id, std::string& error, std::string& code);
    std::optional<Json::Value> unassignRoom(const std::string& external_id, std::string& error);

private:
    db::DbClientPtr m_client;

    static bool isInputClass(const std::string& device_class);
    std::string makeHexId() const;
    static std::string jsonToText(const Json::Value& value);
    static bool parseJsonText(const std::string& text, Json::Value& out);

    Json::Value rowToDeviceJson(const drogon::orm::Row& row, std::optional<int64_t> room_id) const;
    std::optional<int64_t> findRoomIdForDevice(int64_t device_id) const;
    bool roomExists(int64_t room_id) const;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
