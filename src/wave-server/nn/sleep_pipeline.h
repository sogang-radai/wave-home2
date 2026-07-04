#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "frame_aggregator.h"
#include "frame_encoder.h"

#include "../core/json.h"
#include "../device/interface/radar.h"

WAVE_NAMESPACE_BEGIN
NN_NAMESPACE_BEGIN

struct SleepResult
{
    uint64_t frameIndex = 0;

    int32_t statusClass = -1;
    std::vector<float> statusScores;

    bool tossValid = false;
    int32_t tossClass = -1;
    std::vector<float> tossScores;
    float tossIndex = 0.0f;
};

class SleepPipeline
{
public:
    SleepPipeline();
    ~SleepPipeline();

    bool init(std::string_view base_dir, const json& config, std::string& out_error);
    void shutdown();

    void pushFrame(const dev::RadarPointCloud& frame);
    void pushFrame(dev::RadarPointCloud&& frame);

    bool evaluate(SleepResult& out_result);

    const std::string& getModelName() const;

    const std::vector<std::string>& getBedLabels() const;
    const std::vector<std::string>& getTossLabels() const;
    const std::vector<float>& getTossWeightVector() const;

    uint32_t getEmbeddingSize() const;
    uint32_t getBedWindow() const;
    uint32_t getTossWindow() const;
    int32_t getTossActiveStatus() const;

    size_t getFrameCount() const;

private:
    FrameEncoder m_encoder;
    FrameAggregator m_bedAggregator;
    FrameAggregator m_tossAggregator;

    std::string m_modelName;
    std::vector<std::string> m_bedLabels;
    std::vector<std::string> m_tossLabels;
    std::vector<float> m_tossWeightVector;

    uint32_t m_embeddingSize = 0;
    uint32_t m_bedWindow = 0;
    uint32_t m_tossWindow = 0;
    int32_t m_tossActiveStatus = -1;
    bool m_initialized = false;
};

NN_NAMESPACE_END
WAVE_NAMESPACE_END
