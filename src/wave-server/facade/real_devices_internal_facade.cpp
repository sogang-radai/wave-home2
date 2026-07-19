#include "real_devices_internal_facade.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

Json::Value RealDevicesInternalFacade::listDevices(
    const DeviceListFilter& filter,
    std::string& code,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).listDevices(filter, code, std::nullopt);
}

Json::Value RealDevicesInternalFacade::getDevice(
    const std::string& device_id,
    std::optional<int64_t> user_id,
    std::string& code,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).getDevice(device_id, user_id, code, std::nullopt);
}

Json::Value RealDevicesInternalFacade::getState(
    const std::string& device_id,
    std::optional<int64_t> user_id,
    std::string& code,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).getState(device_id, user_id, code, std::nullopt);
}

Json::Value RealDevicesInternalFacade::queryDevice(
    const std::string& device_id,
    const std::string& query_name,
    const Json::Value& body,
    std::string& code,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).queryDevice(device_id, query_name, body, code);
}

Json::Value RealDevicesInternalFacade::invokeAction(
    const std::string& device_id,
    const std::string& action_name,
    const Json::Value& body,
    std::string& code,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).invokeAction(device_id, action_name, body, code);
}

Json::Value RealDevicesInternalFacade::listEvents(
    const EventsListFilter& filter,
    std::string& code,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).listEvents(filter, code);
}

Json::Value RealDevicesInternalFacade::toolListDevices(
    const Json::Value& body,
    std::string& code,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).toolListDevices(body, code);
}

Json::Value RealDevicesInternalFacade::toolControlDevice(
    const Json::Value& body,
    std::string& code,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).toolControlDevice(body, code);
}

Json::Value RealDevicesInternalFacade::toolQueryDevice(
    const Json::Value& body,
    std::string& code,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).toolQueryDevice(body, code);
}

} // namespace facade
WAVE_NAMESPACE_END
