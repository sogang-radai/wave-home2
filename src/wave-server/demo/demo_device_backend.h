#pragma once

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

bool demoVirtualDevicesEnabled();

std::string resolveDemoRuntimeId(
    const drogon::HttpRequestPtr& req,
    const Json::Value* body = nullptr);

class DemoDeviceBackend
{
public:
    explicit DemoDeviceBackend(drogon::orm::DbClientPtr client);

    Json::Value listDevices(const std::string& runtime_id, std::string& code) const;
    /** Session-scoped summary for IoT header cards (devices / rules / events). */
    Json::Value getSummary(const std::string& runtime_id, std::string& code) const;
    Json::Value getState(const std::string& runtime_id, const std::string& device_id, std::string& code) const;
    Json::Value queryDevice(
        const std::string& runtime_id,
        const std::string& device_id,
        const std::string& query_name,
        std::string& code) const;
    Json::Value invokeAction(
        const std::string& runtime_id,
        const std::string& device_id,
        const std::string& action_name,
        const Json::Value& body,
        std::string& code);

private:
    Json::Value deviceRowToJson(const drogon::orm::Row& row, const Json::Value& runtime_state) const;
    Json::Value stateForDevice(const std::string& runtime_id, const std::string& device_id, const std::string& device_class) const;
    void saveState(const std::string& runtime_id, const std::string& device_id, const Json::Value& state) const;

    drogon::orm::DbClientPtr m_client;
};

WAVE_NAMESPACE_END
