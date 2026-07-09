#pragma once

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

// Raw IR mark/space timings in microseconds (mark first, then alternating).
struct IrTimingFrame
{
    std::vector<uint16_t> timingsUs;
    uint32_t carrierHz = 0;
    uint16_t repeat = 0;
    bool overflow = false;
    std::string matchedCommandId;
    std::chrono::steady_clock::time_point receivedAt {};
    bool valid = false;
};

class IIrTransmitter
{
public:
    virtual ~IIrTransmitter() = default;

    virtual int transmitTimings(
        const std::vector<uint16_t>& timingsUs,
        uint32_t carrierHz = 38000,
        uint16_t repeat = 0) = 0;

    virtual std::future<int> transmitTimingsAsync(
        const std::vector<uint16_t>& timingsUs,
        uint32_t carrierHz = 38000,
        uint16_t repeat = 0) = 0;
};

class IIrReceiver
{
public:
    virtual ~IIrReceiver() = default;

    virtual bool getLatestIr(IrTimingFrame& outFrame) = 0;
    virtual bool waitForIr(IrTimingFrame& outFrame, uint32_t timeoutMs) = 0;

    virtual std::future<bool> getLatestIrAsync(IrTimingFrame& outFrame) = 0;
    virtual std::future<bool> waitForIrAsync(IrTimingFrame& outFrame, uint32_t timeoutMs) = 0;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
