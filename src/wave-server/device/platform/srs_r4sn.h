#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "../device.h"
#include "../interface/radar.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

class SRSR4SN :
    public Device,
    public Queryable,
    public IRadarPointCloudProvider,
    public IRadarIQProvider
{
public:
    struct Config
    {
        std::string host;
        bool pointCloudEnabled = true;
        uint16_t pointCloudPort = 29172;
        bool iqEnabled = true;
        uint16_t iqPort = 29171;
    };

    struct Settings
    {
        float angleZ = 0.0f;
        float angleY = 0.0f;
        float minX = 0.0f;
        float maxX = 0.0f;
        float minY = 0.0f;
        float maxY = 0.0f;
        float minZ = 0.0f;
        float maxZ = 0.0f;
    };

    SRSR4SN();
    ~SRSR4SN() override;

    const Config& getConfig() const;
    const Settings& getSettings() const;

    // Device
    int init(const json& config) override;
    void shutdown() override;

    std::string_view getClass() const override;

    // Queryable
    json query(std::string_view name, const json& params) override;
    std::future<json> queryAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    // IRadarPointCloudProvider
    void setPointCloudQueueSize(size_t size) override;
    size_t getPointCloudQueueSize() const override;

    void enumeratePointCloudFrameIndices(std::vector<uint64_t>& indices) override;

    bool isPointCloudFrameAvailable(uint64_t frameIdx) const override;
    bool getPointCloudFrame(uint64_t frameIdx, RadarPointCloud& outFrame) override;
    std::future<void> getLatestPointCloudFrameAsync(RadarPointCloud& outFrame) override;

    // IRadarIQProvider
    std::future<void> requestIQAsync(
        const std::vector<RadarIQRequest>& requests,
        std::vector<RadarIQResponse>& outResponses) override;

private:
    struct Impl;
    struct IqImpl;

    void registerQueries();

    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<IqImpl> m_iqImpl;
    Config m_config;
    Settings m_settings;
    size_t m_pointCloudQueueSize = 512;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
