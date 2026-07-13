#pragma once

#include <cstddef>
#include <cstdint>
#include <future>
#include <vector>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

struct AudioFormat
{
    uint32_t sampleRate = 0;
    uint32_t sampleSize = 0; // bits per sample
    uint32_t channels = 0;
};

struct AudioFrame
{
    uint64_t timestamp = 0;
    std::vector<int16_t> samples;
};

// Microphone / audio input.
class IAudioSource
{
public:
    virtual ~IAudioSource() = default;

    virtual AudioFormat getSourceFormat() const = 0;

    virtual void setAudioQueueSize(size_t size) = 0;
    virtual size_t getAudioQueueSize() const = 0;

    // Peek the newest queued frame (does not consume). For meters / snapshots.
    virtual bool getLatestFrame(AudioFrame& outFrame) = 0;
    virtual std::future<void> getLatestFrameAsync(AudioFrame& outFrame) = 0;

    // Consume the oldest queued frame (FIFO). For recording / streaming.
    virtual bool popFrame(AudioFrame& outFrame) = 0;
};

// Speaker / audio output for playback and two-way talk.
class IAudioSink
{
public:
    virtual ~IAudioSink() = default;

    virtual AudioFormat getSinkFormat() const = 0;

    virtual bool playFrame(const AudioFrame& frame) = 0;
    virtual std::future<bool> playFrameAsync(const AudioFrame& frame) = 0;

    virtual void stopPlayback() = 0;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
