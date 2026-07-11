#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {

struct DeviceListFilter
{
    std::optional<int64_t> user_id;
    std::optional<int64_t> room_id;
    std::optional<std::string> device_class;
    std::optional<bool> connected;
    std::optional<bool> enabled;
};

struct EventsListFilter
{
    std::vector<std::string> types;
    std::optional<std::string> device_id;
    std::optional<std::string> from;
    std::optional<std::string> to;
    int limit = 50;
};

struct ResolvedDevice
{
    std::string device_id;
    std::string device_name;
    std::string device_class;
};

class DevicesInternalStore
{
public:
    explicit DevicesInternalStore(drogon::orm::DbClientPtr client);

    Json::Value listDevices(
        const DeviceListFilter& filter,
        std::string& code,
        const std::optional<std::string>& demo_runtime_id = std::nullopt) const;
    Json::Value getDevice(
        const std::string& device_id,
        std::optional<int64_t> user_id,
        std::string& code,
        const std::optional<std::string>& demo_runtime_id = std::nullopt) const;
    Json::Value getState(
        const std::string& device_id,
        std::optional<int64_t> user_id,
        std::string& code,
        const std::optional<std::string>& demo_runtime_id = std::nullopt) const;
    Json::Value queryDevice(
        const std::string& device_id,
        const std::string& query_name,
        const Json::Value& body,
        std::string& code) const;
    Json::Value invokeAction(
        const std::string& device_id,
        const std::string& action_name,
        const Json::Value& body,
        std::string& code) const;
    Json::Value listEvents(const EventsListFilter& filter, std::string& code) const;

    Json::Value toolListDevices(const Json::Value& body, std::string& code) const;
    Json::Value toolControlDevice(const Json::Value& body, std::string& code) const;
    Json::Value toolQueryDevice(const Json::Value& body, std::string& code) const;

    std::optional<ResolvedDevice> resolveDeviceByName(
        int64_t room_id,
        const std::string& device_name,
        std::optional<int64_t> user_id,
        std::string& code,
        const std::optional<std::string>& demo_runtime_id = std::nullopt) const;

    static std::optional<std::string> resolveWireDeviceId(
        const drogon::orm::DbClientPtr& client,
        const std::string& device_id_param);

private:
    drogon::orm::DbClientPtr m_client;

    bool deviceAllowedForUser(const std::string& external_id, int64_t user_id) const;
    std::optional<Json::Value> findListedDevice(
        const std::string& device_id,
        const std::optional<std::string>& demo_runtime_id = std::nullopt) const;
    std::string externalIdFromDb(int64_t device_row_id) const;
    std::optional<int64_t> internalIdFromExternal(const std::string& external_id) const;
    static bool nameMatches(const std::string& haystack, const std::string& needle);
    static std::string makeEventId();
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
