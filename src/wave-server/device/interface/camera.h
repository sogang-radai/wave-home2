#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

struct CameraFrame
{
    uint64_t timestamp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string format;
    std::vector<uint8_t> data;
};

class ICameraProvider
{
public:
    virtual ~ICameraProvider() = default;

    virtual bool captureFrame(CameraFrame& outFrame) = 0;
    virtual std::future<void> captureFrameAsync(CameraFrame& outFrame) = 0;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
