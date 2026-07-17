#pragma once

#include "../facade/iot_facade.h"

WAVE_NAMESPACE_BEGIN

class DemoIotFacade :
    public facade::IIotFacade
{
public:
    bool devicesReady() const override;

    Json::Value getSummary(const std::string& runtime_id, std::string& code) override;
    Json::Value listDevices(const std::string& runtime_id, std::string& code) override;
    Json::Value getState(
        const std::string& device_id,
        const std::string& runtime_id,
        std::string& code) override;
    Json::Value queryDevice(
        const std::string& device_id,
        const std::string& query_name,
        const std::string& runtime_id,
        std::string& code) override;
    Json::Value invokeAction(
        const std::string& device_id,
        const std::string& action_name,
        const Json::Value& body,
        const std::string& runtime_id,
        std::string& code) override;
    Json::Value listEvents(const std::string& device_id) override;

    Json::Value getRadarGestureSet(
        const std::string& device_id,
        const std::string& runtime_id,
        std::string& code) override;
    bool setRadarGestureSet(
        const std::string& device_id,
        const std::string& set_id,
        const std::string& runtime_id,
        std::string& code) override;
    Json::Value listSpeechOverlays(const std::string& runtime_id) override;
};

WAVE_NAMESPACE_END
