#include "demo_iot_facade.h"

#include "../app/app_state.h"
#include "demo_device_backend.h"
#include "demo_session_registry.h"
#include "demo_session_writes.h"

WAVE_NAMESPACE_BEGIN

bool DemoIotFacade::devicesReady() const
{
    return static_cast<bool>(AppState::get().db());
}

Json::Value DemoIotFacade::getSummary(const std::string& runtime_id, std::string& code)
{
    return DemoDeviceBackend(AppState::get().db()).getSummary(runtime_id, code);
}

Json::Value DemoIotFacade::listDevices(const std::string& runtime_id, std::string& code)
{
    return DemoDeviceBackend(AppState::get().db()).listDevices(runtime_id, code);
}

Json::Value DemoIotFacade::getState(
    const std::string& device_id,
    const std::string& runtime_id,
    std::string& code)
{
    return DemoDeviceBackend(AppState::get().db()).getState(runtime_id, device_id, code);
}

Json::Value DemoIotFacade::queryDevice(
    const std::string& device_id,
    const std::string& query_name,
    const std::string& runtime_id,
    std::string& code)
{
    return DemoDeviceBackend(AppState::get().db()).queryDevice(runtime_id, device_id, query_name, code);
}

Json::Value DemoIotFacade::invokeAction(
    const std::string& device_id,
    const std::string& action_name,
    const Json::Value& body,
    const std::string& runtime_id,
    std::string& code)
{
    return DemoDeviceBackend(AppState::get().db()).invokeAction(runtime_id, device_id, action_name, body, code);
}

Json::Value DemoIotFacade::listEvents(const std::string& /*device_id*/)
{
    return Json::Value(Json::arrayValue);
}

Json::Value DemoIotFacade::getRadarGestureSet(
    const std::string& device_id,
    const std::string& runtime_id,
    std::string& code)
{
    code.clear();
    Json::Value body;
    body["deviceId"] = device_id;
    body["gestureSetId"] = Json::Value();

    bool from_session = false;
    if (const auto session = DemoSessionRegistry::instance().get(runtime_id))
    {
        const auto it = session->radar_gesture_sets.find(device_id);
        if (it != session->radar_gesture_sets.end())
        {
            from_session = true;
            if (!it->second.empty())
                body["gestureSetId"] = it->second;
        }
    }

    auto& state = AppState::get();
    if (!from_session && state.hasGestureStore())
    {
        const auto mapped = state.gestureStore().getRadarGestureSet(device_id, code);
        if (mapped.isMember("gestureSetId"))
            body["gestureSetId"] = mapped["gestureSetId"];
        code.clear();
    }
    return body;
}

bool DemoIotFacade::setRadarGestureSet(
    const std::string& device_id,
    const std::string& set_id,
    const std::string& runtime_id,
    std::string& code)
{
    code.clear();
    auto locked_session = DemoSessionRegistry::instance().lockSession(runtime_id);
    locked_session->radar_gesture_sets[device_id] = set_id;
    return true;
}

Json::Value DemoIotFacade::listSpeechOverlays(const std::string& runtime_id)
{
    return demoListSpeechOverlays(runtime_id);
}

WAVE_NAMESPACE_END
