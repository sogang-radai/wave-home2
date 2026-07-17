#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <json/json.h>

#include "../core/coredefs.h"
#include "../db/database.h"
#include "../web/http/internal/devices_internal_store.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

using DeviceListFilter = web::internal::DeviceListFilter;
using EventsListFilter = web::internal::EventsListFilter;
using ResolvedDevice = web::internal::ResolvedDevice;

class IDevicesInternalFacade
{
public:
    virtual ~IDevicesInternalFacade() = default;

    virtual Json::Value listDevices(
        const DeviceListFilter& filter,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value getDevice(
        const std::string& device_id,
        std::optional<int64_t> user_id,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value getState(
        const std::string& device_id,
        std::optional<int64_t> user_id,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value queryDevice(
        const std::string& device_id,
        const std::string& query_name,
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value invokeAction(
        const std::string& device_id,
        const std::string& action_name,
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value listEvents(
        const EventsListFilter& filter,
        std::string& code,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value toolListDevices(
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value toolControlDevice(
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value toolQueryDevice(
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;
};

} // namespace facade
WAVE_NAMESPACE_END
