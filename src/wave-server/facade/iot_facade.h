#pragma once

#include <string>

#include <json/json.h>

#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

class IIotFacade
{
public:
    virtual ~IIotFacade() = default;

    virtual bool devicesReady() const = 0;

    virtual Json::Value getSummary(const std::string& runtime_id, std::string& code) = 0;
    virtual Json::Value listDevices(const std::string& runtime_id, std::string& code) = 0;
    virtual Json::Value getState(
        const std::string& device_id,
        const std::string& runtime_id,
        std::string& code) = 0;
    virtual Json::Value queryDevice(
        const std::string& device_id,
        const std::string& query_name,
        const std::string& runtime_id,
        std::string& code) = 0;
    virtual Json::Value invokeAction(
        const std::string& device_id,
        const std::string& action_name,
        const Json::Value& body,
        const std::string& runtime_id,
        std::string& code) = 0;
    virtual Json::Value listEvents(const std::string& device_id) = 0;

    virtual Json::Value getRadarGestureSet(
        const std::string& device_id,
        const std::string& runtime_id,
        std::string& code) = 0;
    virtual bool setRadarGestureSet(
        const std::string& device_id,
        const std::string& set_id,
        const std::string& runtime_id,
        std::string& code) = 0;
    virtual Json::Value listSpeechOverlays(const std::string& runtime_id) = 0;
};

} // namespace facade
WAVE_NAMESPACE_END
