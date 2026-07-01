#pragma once

#include <cstdint>
#include <future>
#include <vector>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

struct AudioFrame
{
    uint64_t timestamp = 0;
    std::vector<int16_t> samples;
};

class IAudioProvider
{
public:
    virtual ~IAudioProvider() = default;

    virtual uint32_t getSampleRate() const = 0;
    virtual uint32_t getSampleSize() const = 0;
    virtual uint32_t getChannels() const = 0;

    virtual void setAudioQueueSize(size_t size) = 0;
    virtual size_t getAudioQueueSize() const = 0;

    virtual bool getLatestFrame(AudioFrame& outFrame) = 0;
    virtual std::future<void> getLatestFrameAsync(AudioFrame& outFrame) = 0;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
