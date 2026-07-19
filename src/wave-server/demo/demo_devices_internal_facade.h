#pragma once

#include "../facade/devices_internal_facade.h"

WAVE_NAMESPACE_BEGIN

class DemoDevicesInternalFacade :
    public facade::IDevicesInternalFacade
{
public:
    Json::Value listDevices(
        const facade::DeviceListFilter& filter,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    Json::Value getDevice(
        const std::string& device_id,
        std::optional<int64_t> user_id,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    Json::Value getState(
        const std::string& device_id,
        std::optional<int64_t> user_id,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    Json::Value queryDevice(
        const std::string& device_id,
        const std::string& query_name,
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    Json::Value invokeAction(
        const std::string& device_id,
        const std::string& action_name,
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    Json::Value listEvents(
        const facade::EventsListFilter& filter,
        std::string& code,
        const db::DbClientPtr& client) override;

    Json::Value toolListDevices(
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    Json::Value toolControlDevice(
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    Json::Value toolQueryDevice(
        const Json::Value& body,
        std::string& code,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;
};

WAVE_NAMESPACE_END
