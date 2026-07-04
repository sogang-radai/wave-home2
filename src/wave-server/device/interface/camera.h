#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <vector>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

struct CameraFrame
{
    enum class Format
    {
        Unknown,
        Jpeg,
        Png,
        Rgb24,
        Bgr24,
        Nv12,
        Yuyv,
    };

    uint64_t timestamp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::Unknown;
    std::vector<uint8_t> data;
};

class IImageProvider
{
public:
    virtual ~IImageProvider() = default;

    virtual bool captureFrame(CameraFrame& outFrame) = 0;
    virtual std::future<void> captureFrameAsync(CameraFrame& outFrame) = 0;
};

struct CameraStreamProfile
{
    std::string name;
    std::string uri;
    std::string codec;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
};

class IVideoStreamProvider
{
public:
    virtual ~IVideoStreamProvider() = default;

    virtual bool enumerateStreamProfiles(std::vector<CameraStreamProfile>& outProfiles) = 0;
    virtual bool getStreamUri(std::string_view profile, std::string& outUri) = 0;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
