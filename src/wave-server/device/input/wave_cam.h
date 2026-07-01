#pragma once

#include <cstdint>
#include <future>
#include <string>

#include "../device.h"
#include "../interface/camera.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

struct WaveCamInterfaceConfig
{
    std::string transport;
    std::string backend;
    std::string device;
    std::string host;
    uint16_t port = 0;
};

class WaveCam :
    public InputDevice,
    public ICameraProvider
{
public:
    WaveCam(
        DeviceID id,
        RoomID roomId,
        std::string name,
        std::string description,
        bool enabled,
        WaveCamInterfaceConfig interfaceConfig);
    ~WaveCam() override;

    std::string getClass() const override { return "wave_cam"; }

    bool init() override;
    void shutdown() override;

    const WaveCamInterfaceConfig& getInterfaceConfig() const { return m_interfaceConfig; }

    bool captureFrame(CameraFrame& outFrame) override;
    std::future<void> captureFrameAsync(CameraFrame& outFrame) override;

private:
    WaveCamInterfaceConfig m_interfaceConfig;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
