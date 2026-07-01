#pragma once

#include <cstdint>
#include <future>
#include <string>

#include "../device.h"
#include "../interface/audio.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

struct WaveMicInterfaceConfig
{
    std::string host;
    uint16_t port = 0;
};

struct WaveMicSettings
{
    uint32_t sampleRate = 16000;
    uint32_t sampleSize = 16;
    uint32_t channels = 1;
};

class WaveMic :
    public InputDevice,
    public IAudioProvider
{
public:
    WaveMic(
        DeviceID id,
        RoomID roomId,
        std::string name,
        std::string description,
        bool enabled,
        WaveMicInterfaceConfig interfaceConfig,
        WaveMicSettings settings);
    ~WaveMic() override;

    std::string getClass() const override { return "wave_mic"; }

    bool init() override;
    void shutdown() override;

    const WaveMicInterfaceConfig& getInterfaceConfig() const { return m_interfaceConfig; }
    const WaveMicSettings& getSettings() const { return m_settings; }

    uint32_t getSampleRate() const override;
    uint32_t getSampleSize() const override;
    uint32_t getChannels() const override;

    void setAudioQueueSize(size_t size) override;
    size_t getAudioQueueSize() const override;

    bool getLatestFrame(AudioFrame& outFrame) override;
    std::future<void> getLatestFrameAsync(AudioFrame& outFrame) override;

private:
    WaveMicInterfaceConfig m_interfaceConfig;
    WaveMicSettings m_settings;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
