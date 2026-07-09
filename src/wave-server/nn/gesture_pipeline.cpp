#include "gesture_pipeline.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

WAVE_NAMESPACE_BEGIN
NN_NAMESPACE_BEGIN

namespace
{
    int32_t argmax(const std::vector<float>& scores)
    {
        if (scores.empty())
            return -1;

        const auto it = std::max_element(scores.begin(), scores.end());
        return static_cast<int32_t>(std::distance(scores.begin(), it));
    }

    uint64_t nowMs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    SchmittTriggerConfig parseTriggerConfig(const json& value)
    {
        SchmittTriggerConfig cfg;
        if (!value.is_object())
            return cfg;

        cfg.highThreshold = value.value("high_threshold", cfg.highThreshold);
        cfg.lowThreshold = value.value("low_threshold", cfg.lowThreshold);
        cfg.highHoldMs = value.value("high_hold_ms", cfg.highHoldMs);
        cfg.lowHoldMs = value.value("low_hold_ms", cfg.lowHoldMs);
        cfg.cooldownMs = value.value("cooldown_ms", cfg.cooldownMs);
        return cfg;
    }

    ControlSignalType parseSignalType(const json& value)
    {
        if (!value.is_string())
            return ControlSignalType::Single;

        const std::string type = value.get<std::string>();
        if (type == "repeat")
            return ControlSignalType::Repeat;
        if (type == "toggle")
            return ControlSignalType::Toggle;
        return ControlSignalType::Single;
    }
}

GesturePipeline::GesturePipeline() = default;

GesturePipeline::~GesturePipeline()
{
    shutdown();
}

bool GesturePipeline::init(std::string_view base_dir, const json& set_config, std::string& out_error)
{
    shutdown();

    try
    {
        if (!set_config.is_object())
        {
            out_error = "gesture set config must be a JSON object";
            return false;
        }

        if (!set_config.contains("model_path") || !set_config["model_path"].is_string())
        {
            out_error = "gesture set config missing model_path";
            return false;
        }

        if (!set_config.contains("classes") || !set_config["classes"].is_array())
        {
            out_error = "gesture set config missing classes";
            return false;
        }

        const std::filesystem::path set_dir(base_dir);
        const std::filesystem::path model_json_path = set_dir / set_config["model_path"].get<std::string>();
        if (!std::filesystem::exists(model_json_path))
        {
            out_error = "model config not found: " + model_json_path.string();
            return false;
        }

        json model_config;
        {
            std::ifstream in(model_json_path);
            in >> model_config;
        }

        if (!model_config.contains("frame_encoder") || !model_config.contains("temporal_aggregator"))
        {
            out_error = "model config must contain frame_encoder and temporal_aggregator";
            return false;
        }

        const json& temporal = model_config["temporal_aggregator"];
        m_sequenceLength = temporal.at("sequence_length").get<uint32_t>();
        if (m_sequenceLength == 0)
        {
            out_error = "sequence_length must be greater than 0";
            return false;
        }

        const std::filesystem::path model_dir = model_json_path.parent_path();
        const size_t queue_size = static_cast<size_t>(m_sequenceLength) * 4;

        if (!m_encoder.init(model_dir.string(), model_config["frame_encoder"], queue_size, out_error))
            return false;

        if (!m_aggregator.init(model_dir.string(), temporal, out_error))
            return false;

        m_embeddingSize = m_encoder.getEmbeddingSize();
        m_classCount = temporal.value("output_size", 0u);
        m_setName = set_config.value("name", std::string("Gesture Set"));
        m_modelName = model_config.value("model_name", std::string("GestureNet"));

        m_classes.clear();
        m_triggers.clear();
        for (const auto& item : set_config["classes"])
        {
            GestureClassConfig cls;
            cls.classId = item.value("class_id", -1);
            cls.name = item.value("name", std::string());
            cls.trigger = parseTriggerConfig(item.value("trigger", json::object()));
            cls.signalType = parseSignalType(item.value("signal_type", json()));
            cls.repeatIntervalMs = item.value("repeat_interval_ms", 0u);
            if (item.contains("action"))
                cls.action = item["action"];
            m_classes.push_back(cls);
            m_triggers.push_back(TriggerState{});
        }

        if (m_classCount == 0)
            m_classCount = static_cast<uint32_t>(m_classes.size());

        m_initialized = true;
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = std::string("gesture pipeline init error: ") + e.what();
        shutdown();
        return false;
    }
}

void GesturePipeline::shutdown()
{
    m_encoder.shutdown();
    m_aggregator.shutdown();
    m_signalQueue.clear();

    m_setName.clear();
    m_modelName.clear();
    m_classes.clear();
    m_triggers.clear();

    m_sequenceLength = 0;
    m_embeddingSize = 0;
    m_classCount = 0;
    m_initialized = false;
}

void GesturePipeline::pushFrame(const dev::RadarPointCloud& frame)
{
    if (!m_initialized)
        return;

    m_encoder.pushFrame(frame);
}

void GesturePipeline::pushFrame(dev::RadarPointCloud&& frame)
{
    if (!m_initialized)
        return;

    m_encoder.pushFrame(std::move(frame));
}

bool GesturePipeline::evaluate(GestureResult& out_result)
{
    if (!m_initialized)
        return false;

    std::vector<float> matrix;
    if (!m_encoder.getEmbeddingMatrix(m_sequenceLength, matrix))
        return false;

    out_result = GestureResult{};
    std::vector<uint64_t> indices;
    m_encoder.enumerateFrameIndices(indices);
    if (!indices.empty())
        out_result.frameIndex = *std::max_element(indices.begin(), indices.end());

    m_aggregator.evaluate(matrix, out_result.scores);
    out_result.topClass = argmax(out_result.scores);

    const uint64_t ts_ms = nowMs();
    const size_t class_count = std::min(m_classes.size(), out_result.scores.size());

    for (size_t i = 0; i < class_count; ++i)
    {
        const auto& cls = m_classes[i];
        auto& state = m_triggers[i];
        const float score = out_result.scores[i];
        const auto& cfg = cls.trigger;

        if (score >= cfg.highThreshold)
        {
            if (state.highSinceMs == 0)
                state.highSinceMs = ts_ms;
            state.lowSinceMs = 0;

            const bool held = ts_ms - state.highSinceMs >= cfg.highHoldMs;
            if (held && !state.active)
            {
                state.active = true;
                if (ts_ms - state.lastFireMs >= cfg.cooldownMs)
                {
                    state.lastFireMs = ts_ms;
                    ControlSignal signal;
                    signal.frameIndex = out_result.frameIndex;
                    signal.timestampMs = ts_ms;
                    signal.classId = cls.classId;
                    signal.className = cls.name;
                    signal.type = cls.signalType;
                    signal.toggleState = state.toggleState;
                    signal.action = cls.action;
                    m_signalQueue.push_back(signal);
                }
            }
        }
        else if (score <= cfg.lowThreshold)
        {
            if (state.lowSinceMs == 0)
                state.lowSinceMs = ts_ms;
            state.highSinceMs = 0;

            if (state.active && ts_ms - state.lowSinceMs >= cfg.lowHoldMs)
                state.active = false;
        }
    }

    return true;
}

bool GesturePipeline::popSignal(ControlSignal& out_signal)
{
    if (m_signalQueue.empty())
        return false;

    out_signal = m_signalQueue.front();
    m_signalQueue.pop_front();
    return true;
}

size_t GesturePipeline::pendingSignalCount() const
{
    return m_signalQueue.size();
}

void GesturePipeline::clearSignals()
{
    m_signalQueue.clear();
}

const std::string& GesturePipeline::getSetName() const
{
    return m_setName;
}

const std::string& GesturePipeline::getModelName() const
{
    return m_modelName;
}

const std::vector<GestureClassConfig>& GesturePipeline::getClasses() const
{
    return m_classes;
}

uint32_t GesturePipeline::getClassCount() const
{
    return m_classCount;
}

uint32_t GesturePipeline::getSequenceLength() const
{
    return m_sequenceLength;
}

uint32_t GesturePipeline::getEmbeddingSize() const
{
    return m_embeddingSize;
}

size_t GesturePipeline::getFrameCount() const
{
    if (!m_initialized)
        return 0;
    return m_encoder.getFrameCount();
}

bool GesturePipeline::isClassActive(int32_t class_id) const
{
    for (size_t i = 0; i < m_classes.size(); ++i)
    {
        if (m_classes[i].classId == class_id)
            return i < m_triggers.size() && m_triggers[i].active;
    }
    return false;
}

NN_NAMESPACE_END
WAVE_NAMESPACE_END
