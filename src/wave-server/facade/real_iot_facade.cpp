#include "real_iot_facade.h"

#include "../app/app_state.h"
#include "../web/http/v1/iot_store.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

bool RealIotFacade::devicesReady() const
{
    return web::v1::IotStore(AppState::get().deviceManager).devicesAvailable();
}

Json::Value RealIotFacade::getSummary(const std::string& /*runtime_id*/, std::string& code)
{
    code.clear();
    return web::v1::IotStore(AppState::get().deviceManager).getSummary();
}

Json::Value RealIotFacade::listDevices(const std::string& /*runtime_id*/, std::string& code)
{
    code.clear();
    return web::v1::IotStore(AppState::get().deviceManager).listDevices();
}

Json::Value RealIotFacade::getState(
    const std::string& device_id,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    return web::v1::IotStore(AppState::get().deviceManager).queryDevice(device_id, "status", code);
}

Json::Value RealIotFacade::queryDevice(
    const std::string& device_id,
    const std::string& query_name,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    return web::v1::IotStore(AppState::get().deviceManager).queryDevice(device_id, query_name, code);
}

Json::Value RealIotFacade::invokeAction(
    const std::string& device_id,
    const std::string& action_name,
    const Json::Value& body,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    Json::Value params = body;
    if (body.isMember("params") && body["params"].isObject())
        params = body["params"];
    return web::v1::IotStore(AppState::get().deviceManager).invokeDevice(device_id, action_name, params, code);
}

Json::Value RealIotFacade::listEvents(const std::string& device_id)
{
    return web::v1::IotStore(AppState::get().deviceManager).listEvents(device_id);
}

Json::Value RealIotFacade::getRadarGestureSet(
    const std::string& device_id,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    auto& state = AppState::get();
    if (!state.hasGestureStore())
    {
        code = "UNAVAILABLE";
        return Json::Value();
    }
    return state.gestureStore().getRadarGestureSet(device_id, code);
}

bool RealIotFacade::setRadarGestureSet(
    const std::string& device_id,
    const std::string& set_id,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    auto& state = AppState::get();
    if (!state.hasGestureStore())
    {
        code = "UNAVAILABLE";
        return false;
    }
    state.gestureStore().setRadarGestureSet(device_id, set_id, code);
    return code != "NOT_FOUND" && code != "UNAVAILABLE";
}

Json::Value RealIotFacade::listSpeechOverlays(const std::string& /*runtime_id*/)
{
    return Json::Value(Json::objectValue);
}

} // namespace facade
WAVE_NAMESPACE_END
