#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "../core/json.h"
#include "frame_aggregator.h"
#include "frame_encoder.h"
#include "../device/interface/radar.h"

WAVE_NAMESPACE_BEGIN
NN_NAMESPACE_BEGIN

struct SchmittTriggerConfig
{
    float highThreshold = 0.8f;
    float lowThreshold = 0.2f;
    uint32_t highHoldMs = 500;
    uint32_t lowHoldMs = 500;
    uint32_t cooldownMs = 3000;
};

enum class ControlSignalType
{
    Single,
    Repeat,
    Toggle,
};

struct GestureClassConfig
{
    int32_t classId = -1;
    std::string name;
    SchmittTriggerConfig trigger;
    ControlSignalType signalType = ControlSignalType::Single;
    uint32_t repeatIntervalMs = 0;
    json action;
};

struct ControlSignal
{
    uint64_t frameIndex = 0;
    uint64_t timestampMs = 0;
    int32_t classId = -1;
    std::string className;
    ControlSignalType type = ControlSignalType::Single;
    bool toggleState = false;
    json action;
};

struct GestureResult
{
    uint64_t frameIndex = 0;
    int32_t topClass = -1;
    std::vector<float> scores;
};

class GesturePipeline
{
public:
    GesturePipeline();
    ~GesturePipeline();

    bool init(std::string_view base_dir, const json& set_config, std::string& out_error);
    void shutdown();

    void pushFrame(const dev::RadarPointCloud& frame);
    void pushFrame(dev::RadarPointCloud&& frame);

    bool evaluate(GestureResult& out_result);

    bool popSignal(ControlSignal& out_signal);
    size_t pendingSignalCount() const;
    void clearSignals();

    const std::string& getSetName() const;
    const std::string& getModelName() const;
    const std::vector<GestureClassConfig>& getClasses() const;

    uint32_t getClassCount() const;
    uint32_t getSequenceLength() const;
    uint32_t getEmbeddingSize() const;
    size_t getFrameCount() const;

private:
    struct TriggerState
    {
        bool active = false;
        uint64_t highSinceMs = 0;
        uint64_t lowSinceMs = 0;
        uint64_t lastFireMs = 0;
        bool toggleState = false;
    };

    FrameEncoder m_encoder;
    FrameAggregator m_aggregator;

    std::string m_setName;
    std::string m_modelName;
    std::vector<GestureClassConfig> m_classes;
    std::vector<TriggerState> m_triggers;
    std::deque<ControlSignal> m_signalQueue;

    uint32_t m_sequenceLength = 0;
    uint32_t m_embeddingSize = 0;
    uint32_t m_classCount = 0;
    bool m_initialized = false;
};

NN_NAMESPACE_END
WAVE_NAMESPACE_END
