#include "sleep_pipeline.h"

#include <algorithm>
#include <iterator>
#include <utility>

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
}

SleepPipeline::SleepPipeline() = default;

SleepPipeline::~SleepPipeline()
{
    shutdown();
}

bool SleepPipeline::init(std::string_view base_dir, const json& config, std::string& out_error)
{
    shutdown();

    try
    {
        if (!config.is_object())
        {
            out_error = "sleep model config must be a JSON object";
            return false;
        }

        for (const char* key : {"frame_encoder", "bed_model", "toss_model"})
        {
            if (!config.contains(key))
            {
                out_error = std::string("sleep model config missing required field '") + key + "'";
                return false;
            }
        }

        const json& bed_model = config["bed_model"];
        const json& toss_model = config["toss_model"];

        if (!bed_model.contains("temporal_aggregator") || !toss_model.contains("temporal_aggregator"))
        {
            out_error = "bed_model and toss_model must contain 'temporal_aggregator'";
            return false;
        }

        const json& bed_agg = bed_model["temporal_aggregator"];
        const json& toss_agg = toss_model["temporal_aggregator"];

        m_modelName = config.value("model_name", std::string("SleepNet"));
        m_tossActiveStatus = config.value("toss_active_status", 2);

        m_bedWindow = bed_agg.at("sequence_length").get<uint32_t>();
        m_tossWindow = toss_agg.at("sequence_length").get<uint32_t>();
        if (m_bedWindow == 0 || m_tossWindow == 0)
        {
            out_error = "bed/toss sequence_length must be greater than 0";
            return false;
        }

        const size_t queue_size = std::max(m_bedWindow, m_tossWindow) * 4;

        if (!m_encoder.init(base_dir, config["frame_encoder"], queue_size, out_error))
            return false;

        if (!m_bedAggregator.init(base_dir, bed_agg, out_error))
            return false;

        if (!m_tossAggregator.init(base_dir, toss_agg, out_error))
            return false;

        m_embeddingSize = m_encoder.getEmbeddingSize();

        m_bedLabels = bed_model.value("labels", std::vector<std::string>{});
        m_tossLabels = toss_model.value("labels", std::vector<std::string>{});
        m_tossWeightVector = toss_model.value("toss_weight_vector", std::vector<float>{});

        m_initialized = true;
        return true;
    }
    catch (const json::exception& e)
    {
        out_error = std::string("sleep model config parse error: ") + e.what();
        shutdown();
        return false;
    }
}

void SleepPipeline::shutdown()
{
    m_encoder.shutdown();
    m_bedAggregator.shutdown();
    m_tossAggregator.shutdown();

    m_modelName.clear();
    m_bedLabels.clear();
    m_tossLabels.clear();
    m_tossWeightVector.clear();

    m_embeddingSize = 0;
    m_bedWindow = 0;
    m_tossWindow = 0;
    m_tossActiveStatus = -1;
    m_initialized = false;
}

void SleepPipeline::pushFrame(const dev::RadarPointCloud& frame)
{
    if (!m_initialized)
        return;

    m_encoder.pushFrame(frame);
}

void SleepPipeline::pushFrame(dev::RadarPointCloud&& frame)
{
    if (!m_initialized)
        return;

    m_encoder.pushFrame(std::move(frame));
}

bool SleepPipeline::evaluate(SleepResult& out_result)
{
    if (!m_initialized)
        return false;

    std::vector<float> bed_matrix;
    if (!m_encoder.getEmbeddingMatrix(m_bedWindow, bed_matrix))
        return false;

    out_result = SleepResult{};

    std::vector<uint64_t> indices;
    m_encoder.enumerateFrameIndices(indices);
    if (!indices.empty())
        out_result.frameIndex = *std::max_element(indices.begin(), indices.end());

    m_bedAggregator.evaluate(bed_matrix, out_result.statusScores);
    out_result.statusClass = argmax(out_result.statusScores);

    if (out_result.statusClass == m_tossActiveStatus)
    {
        std::vector<float> toss_matrix;
        bool have_toss = false;

        if (m_tossWindow <= m_bedWindow)
        {
            const size_t tail = static_cast<size_t>(m_tossWindow) * m_embeddingSize;
            toss_matrix.assign(bed_matrix.end() - static_cast<ptrdiff_t>(tail), bed_matrix.end());
            have_toss = true;
        }
        else
        {
            have_toss = m_encoder.getEmbeddingMatrix(m_tossWindow, toss_matrix);
        }

        if (have_toss)
        {
            m_tossAggregator.evaluate(toss_matrix, out_result.tossScores);
            out_result.tossClass = argmax(out_result.tossScores);

            float toss_index = 0.0f;
            const size_t count = std::min(out_result.tossScores.size(), m_tossWeightVector.size());
            for (size_t i = 0; i < count; ++i)
                toss_index += out_result.tossScores[i] * m_tossWeightVector[i];

            out_result.tossIndex = toss_index;
            out_result.tossValid = true;
        }
    }

    return true;
}

const std::string& SleepPipeline::getModelName() const
{
    return m_modelName;
}

const std::vector<std::string>& SleepPipeline::getBedLabels() const
{
    return m_bedLabels;
}

const std::vector<std::string>& SleepPipeline::getTossLabels() const
{
    return m_tossLabels;
}

const std::vector<float>& SleepPipeline::getTossWeightVector() const
{
    return m_tossWeightVector;
}

uint32_t SleepPipeline::getEmbeddingSize() const
{
    return m_embeddingSize;
}

uint32_t SleepPipeline::getBedWindow() const
{
    return m_bedWindow;
}

uint32_t SleepPipeline::getTossWindow() const
{
    return m_tossWindow;
}

int32_t SleepPipeline::getTossActiveStatus() const
{
    return m_tossActiveStatus;
}

size_t SleepPipeline::getFrameCount() const
{
    if (!m_initialized)
        return 0;

    return m_encoder.getFrameCount();
}

FrameEncoderWindowMode SleepPipeline::getEncoderWindowMode() const
{
    return m_encoder.getWindowMode();
}

void SleepPipeline::setEncoderWindowMode(FrameEncoderWindowMode mode)
{
    m_encoder.setWindowMode(mode);
}

NN_NAMESPACE_END
WAVE_NAMESPACE_END
