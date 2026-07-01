#pragma once

#include <cstdint>
#include <string>

#include "../device.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

struct TizenTvInterfaceConfig
{
    std::string host;
    uint16_t port = 8002;
    std::string name;
};

class TizenTv : public OutputDevice
{
public:
    TizenTv(
        DeviceID id,
        RoomID roomId,
        std::string name,
        std::string description,
        bool enabled,
        TizenTvInterfaceConfig interfaceConfig);
    ~TizenTv() override;

    std::string getClass() const override { return "tizen_tv"; }

    bool init() override;
    void shutdown() override;

    const TizenTvInterfaceConfig& getInterfaceConfig() const { return m_interfaceConfig; }

    std::vector<OutputCapability> getCapabilities() const override;
    std::string getCapabilitiesJson() const override;
    bool invoke(const std::string& action, const std::string& paramsJson, std::string& outError) override;

private:
    TizenTvInterfaceConfig m_interfaceConfig;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
