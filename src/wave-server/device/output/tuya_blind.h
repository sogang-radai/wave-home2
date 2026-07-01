#pragma once

#include <future>
#include <string>

#include "../device.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

class TuyaBlind : public OutputDevice
{
public:
    struct InterfaceConfig
    {
        std::string host;
        std::string deviceId;
        std::string localKey;
        std::string version;
    };

    TuyaBlind(const json& config);
    ~TuyaBlind() override;

    std::string getClass() const override;

    int init() override;
    void shutdown() override;

    const InterfaceConfig& getInterfaceConfig() const;

    const std::vector<DeviceFeature>& enumerateFeatures() const override;
    const std::vector<DeviceQuery>& enumerateQueries() const override;
    const json& getFeature(std::string_view name) const override;

    int invoke(std::string_view name, const json& params) override;
    std::future<int> invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    int query(std::string_view name, const json& params, json& outResult) override;
    std::future<int> queryAsync(std::string_view name, const json& params, json& outResult, uint32_t timeout_ms = 1000) override;

private:
    InterfaceConfig m_interfaceConfig;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
