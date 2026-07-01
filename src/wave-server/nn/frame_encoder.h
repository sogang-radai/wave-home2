#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../core/json.h"
#include "../device/interface/radar.h"

WAVE_NAMESPACE_BEGIN
NN_NAMESPACE_BEGIN

struct FrameEncoderNormalizeSettings
{
    enum class CoordinateMode
    {
        Cartesian = 0,
        Polar = 1
    };

    enum class PowerMode
    {
        Linear = 0,
        Db = 1,
    };

    CoordinateMode coordinateMode = CoordinateMode::Cartesian;
    PowerMode powerMode = PowerMode::Db;
    union
    {
        struct
        {
            float minX = 0.0f;
            float maxX = 0.0f;
            float minY = 0.0f;
            float maxY = 0.0f;
            float minZ = 0.0f;
            float maxZ = 0.0f;
        } cartesian;
        struct
        {
            float minR = 0.0f;
            float maxR = 0.0f;
            float minTheta = 0.0f;
            float maxTheta = 0.0f;
            float minPhi = 0.0f;
            float maxPhi = 0.0f;
        } polar;
    };

    union
    {
        struct
        {
            float minPower = 0.0f;
            float maxPower = 0.0f;
        } linear;
        struct
        {
            float minPowerDb = 0.0f;
            float maxPowerDb = 0.0f;
        } db;
    };

    float minDoppler = 0.0f;
    float maxDoppler = 0.0f;
};

class FrameEncoder
{
public:
    FrameEncoder();
    ~FrameEncoder();

    bool init(std::string_view base_dir, const json& config, std::string& out_error);
    void shutdown();

    uint32_t getEmbeddingSize() const;
    uint32_t getSequenceLength() const;
    const FrameEncoderNormalizeSettings& getNormalizeSettings() const;

    void setQueueSize(size_t size);
    size_t getQueueSize() const;

    void pushFrame(const dev::RadarPointCloud& frame);
    void pushFrame(dev::RadarPointCloud&& frame);

    void enumerateFrameIndices(std::vector<uint64_t>& indices) const;
    bool isFrameAvailable(uint64_t frame_idx) const;
    bool getFrameEmbedding(uint64_t frame_idx, std::vector<float>& out_embedding) const;

    bool getEmbeddingMatrix(std::vector<float>& out_mat); // float[sequence_length][embedding_size], oldest row first

private:
    struct Impl;

    std::unique_ptr<Impl> m_impl;
};

NN_NAMESPACE_END
WAVE_NAMESPACE_END
