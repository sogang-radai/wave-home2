#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct MicDeviceInfo
{
    int32_t index = -1;
    uint32_t deviceId = 0;
    std::string name;
    int32_t maxInputChannels = 0;
    double defaultSampleRate = 0.0;
    bool isDefault = false;
};

class MicCapture
{
public:
    using ChunkCallback = std::function<void(const float* samples, size_t count)>;

    MicCapture();
    ~MicCapture();

    MicCapture(const MicCapture&) = delete;
    MicCapture& operator=(const MicCapture&) = delete;

    static std::vector<MicDeviceInfo> listInputDevices();
    static int32_t defaultInputDeviceIndex();

    bool open(
        int32_t deviceIndex,
        int32_t sampleRate,
        int32_t channels,
        ChunkCallback callback,
        bool useExplicitDevice = false);
    void close();
    bool isOpen() const;

    int32_t deviceIndex() const;
    int32_t sampleRate() const;
    int32_t channels() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
