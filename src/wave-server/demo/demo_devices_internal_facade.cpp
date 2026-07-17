#include "demo_devices_internal_facade.h"

#include "demo_device_backend.h"

WAVE_NAMESPACE_BEGIN

Json::Value DemoDevicesInternalFacade::listDevices(
    const facade::DeviceListFilter& filter,
    std::string& code,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).listDevices(
        filter, code, runtime_id.empty() ? std::nullopt : std::optional<std::string>(runtime_id));
}

Json::Value DemoDevicesInternalFacade::getDevice(
    const std::string& device_id,
    std::optional<int64_t> user_id,
    std::string& code,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).getDevice(
        device_id,
        user_id,
        code,
        runtime_id.empty() ? std::nullopt : std::optional<std::string>(runtime_id));
}

Json::Value DemoDevicesInternalFacade::getState(
    const std::string& device_id,
    std::optional<int64_t> user_id,
    std::string& code,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return web::internal::DevicesInternalStore(client).getState(
        device_id,
        user_id,
        code,
        runtime_id.empty() ? std::nullopt : std::optional<std::string>(runtime_id));
}

Json::Value DemoDevicesInternalFacade::queryDevice(
    const std::string& device_id,
    const std::string& query_name,
    const Json::Value& /*body*/,
    std::string& code,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return DemoDeviceBackend(client).queryDevice(runtime_id, device_id, query_name, code);
}

Json::Value DemoDevicesInternalFacade::invokeAction(
    const std::string& device_id,
    const std::string& action_name,
    const Json::Value& body,
    std::string& code,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return DemoDeviceBackend(client).invokeAction(runtime_id, device_id, action_name, body, code);
}

Json::Value DemoDevicesInternalFacade::listEvents(
    const facade::EventsListFilter& /*filter*/,
    std::string& code,
    const db::DbClientPtr& /*client*/)
{
    code.clear();
    Json::Value body;
    body["items"] = Json::Value(Json::arrayValue);
    body["count"] = 0;
    return body;
}

Json::Value DemoDevicesInternalFacade::toolListDevices(
    const Json::Value& body,
    std::string& code,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    Json::Value enriched = body;
    if (!runtime_id.empty())
        enriched["demoRuntimeId"] = runtime_id;
    return web::internal::DevicesInternalStore(client).toolListDevices(enriched, code);
}

Json::Value DemoDevicesInternalFacade::toolControlDevice(
    const Json::Value& body,
    std::string& code,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    Json::Value enriched = body;
    if (!runtime_id.empty())
        enriched["demoRuntimeId"] = runtime_id;
    return web::internal::DevicesInternalStore(client).toolControlDevice(enriched, code);
}

Json::Value DemoDevicesInternalFacade::toolQueryDevice(
    const Json::Value& body,
    std::string& code,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    Json::Value enriched = body;
    if (!runtime_id.empty())
        enriched["demoRuntimeId"] = runtime_id;
    return web::internal::DevicesInternalStore(client).toolQueryDevice(enriched, code);
}

WAVE_NAMESPACE_END
