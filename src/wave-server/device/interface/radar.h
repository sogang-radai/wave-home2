#pragma once

#include <vector>
#include <future>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

struct RadarPointCloud
{
    struct Point
    {
        float x;
        float y;
        float z;
        float doppler;
        float power;
    };

    struct Target
    {
        uint32_t targetId;
        float minX;
        float maxX;
        float minY;
        float maxY;
        float minZ;
        float maxZ;
        std::vector<uint16_t> pointIndices;
    };

    uint64_t frameIndex;
    uint64_t timestamp;

    std::vector<Point> points;
    std::vector<Target> targets;
};

class IRadarPointCloudProvider
{
public:
    virtual ~IRadarPointCloudProvider() = default;

    virtual void setPointCloudQueueSize(size_t size) = 0;
    virtual size_t getPointCloudQueueSize() const = 0;

    virtual void enumeratePointCloudFrameIndices(std::vector<uint64_t>& indices) = 0;

    virtual bool isPointCloudFrameAvailable(uint64_t frame_idx) const = 0;
    virtual bool getPointCloudFrame(uint64_t frame_idx, RadarPointCloud& out_frame) = 0;
    virtual std::future<void> getLatestPointCloudFrameAsync(RadarPointCloud& out_frame) = 0;
};

struct RadarIQ
{
    float real;
    float imag;
};

struct RadarIQRequest
{
    float azimuth;
    float elevation;
    float distance;
};

struct RadarIQResponse
{
    RadarIQ iq;
};

class IRadarIQProvider
{
public:
    virtual ~IRadarIQProvider() = default;

    virtual std::future<void> requestIQAsync(const std::vector<RadarIQRequest>& requests, std::vector<RadarIQResponse>& out_responses) = 0;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END