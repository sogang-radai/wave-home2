#include "frame_aggregator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include <net.h>

WAVE_NAMESPACE_BEGIN
NN_NAMESPACE_BEGIN

namespace
{
    void apply_softmax(std::vector<float>& logits)
    {
        if (logits.empty())
            return;

        const float max_val = *std::max_element(logits.begin(), logits.end());
        float sum_exp = 0.0f;

        for (float& val : logits)
        {
            val = std::exp(val - max_val);
            sum_exp += val;
        }

        if (sum_exp > 0.0f)
        {
            for (float& val : logits)
                val /= sum_exp;
        }
    }

    void extract_flat(const ncnn::Mat& mat, uint32_t expected_size, std::vector<float>& out)
    {
        out.assign(expected_size, 0.f);
        const int total = mat.w * mat.h * mat.c * mat.d;
        const int copy_count = std::min(static_cast<int>(expected_size), total);
        const float* src = static_cast<const float*>(mat.data);
        if (src && copy_count > 0)
            std::memcpy(out.data(), src, static_cast<size_t>(copy_count) * sizeof(float));
    }
}

struct FrameAggregator::Impl
{
    std::string name;
    std::string networkType;
    std::string paramPath;
    std::string binPath;
    std::string inputName = "in0";
    std::string outputName = "out0";

    uint32_t embeddingSize = 0;
    uint32_t sequenceLength = 0;
    uint32_t outputSize = 0;
    bool loaded = false;

    ncnn::Net temporalNet;
    ncnn::Mat inputMat;

    bool parseConfig(std::string_view base_dir, const json& config, std::string& out_error);
    bool loadModel(std::string& out_error);

    void runAggregator(const std::vector<float>& embedding_matrix, std::vector<float>& out_scores);
};

bool FrameAggregator::Impl::parseConfig(
    std::string_view base_dir,
    const json& config,
    std::string& out_error)
{
    try
    {
        if (!config.is_object())
        {
            out_error = "temporal_aggregator config must be a JSON object";
            return false;
        }

        for (const char* key : {"name", "type", "sequence_length", "embedding_size", "param_path", "bin_path", "output_size"})
        {
            if (!config.contains(key))
            {
                out_error = std::string("temporal_aggregator config missing required field '") + key + "'";
                return false;
            }
        }

        name = config["name"].get<std::string>();
        networkType = config["type"].get<std::string>();
        if (networkType != "1dcnn")
        {
            out_error = "temporal_aggregator type must be '1dcnn'";
            return false;
        }

        sequenceLength = config["sequence_length"].get<uint32_t>();
        if (sequenceLength == 0)
        {
            out_error = "sequence_length must be greater than 0";
            return false;
        }

        embeddingSize = config["embedding_size"].get<uint32_t>();
        if (embeddingSize == 0)
        {
            out_error = "embedding_size must be greater than 0";
            return false;
        }

        outputSize = config["output_size"].get<uint32_t>();
        if (outputSize == 0)
        {
            out_error = "output_size must be greater than 0";
            return false;
        }

        inputName = config.value("input_name", "in0");
        outputName = config.value("output_name", "out0");

        const std::filesystem::path base_path = base_dir.empty()
            ? std::filesystem::current_path()
            : std::filesystem::path(base_dir);
        paramPath = (base_path / config["param_path"].get<std::string>()).string();
        binPath = (base_path / config["bin_path"].get<std::string>()).string();

        return true;
    }
    catch (const json::exception& e)
    {
        out_error = std::string("temporal_aggregator config parse error: ") + e.what();
        return false;
    }
}

bool FrameAggregator::Impl::loadModel(std::string& out_error)
{
    if (!std::filesystem::exists(paramPath))
    {
        out_error = "Model param file not found: " + paramPath;
        return false;
    }
    if (!std::filesystem::exists(binPath))
    {
        out_error = "Model bin file not found: " + binPath;
        return false;
    }
    if (temporalNet.load_param(paramPath.c_str()) != 0)
    {
        out_error = "Failed to load param: " + paramPath;
        return false;
    }
    if (temporalNet.load_model(binPath.c_str()) != 0)
    {
        out_error = "Failed to load bin: " + binPath;
        return false;
    }

    loaded = true;
    return true;
}

void FrameAggregator::Impl::runAggregator(
    const std::vector<float>& embedding_matrix,
    std::vector<float>& out_scores)
{
    const size_t expected_input = static_cast<size_t>(embeddingSize) * sequenceLength;
    if (embedding_matrix.size() != expected_input)
        throw std::runtime_error("embedding_matrix size mismatch for temporal aggregator");

    const int seq_len = static_cast<int>(sequenceLength);
    inputMat.create(static_cast<int>(embeddingSize), seq_len);
    for (int t = 0; t < seq_len; ++t)
    {
        float* row = inputMat.row(t);
        std::memcpy(
            row,
            embedding_matrix.data() + static_cast<size_t>(t) * embeddingSize,
            static_cast<size_t>(embeddingSize) * sizeof(float));
    }

    ncnn::Extractor extractor = temporalNet.create_extractor();
    extractor.input(inputName.c_str(), inputMat);

    ncnn::Mat output;
    if (extractor.extract(outputName.c_str(), output) != 0)
        throw std::runtime_error("temporal_aggregator extract failed");

    extract_flat(output, outputSize, out_scores);
    apply_softmax(out_scores);
}

FrameAggregator::FrameAggregator() :
    m_impl(nullptr)
{
}

FrameAggregator::~FrameAggregator()
{
    shutdown();
}

bool FrameAggregator::init(std::string_view base_dir, const json& config, std::string& out_error)
{
    shutdown();

    auto impl = std::make_unique<Impl>();
    if (!impl->parseConfig(base_dir, config, out_error))
        return false;

    if (!impl->loadModel(out_error))
        return false;

    m_impl = std::move(impl);
    return true;
}

void FrameAggregator::shutdown()
{
    m_impl.reset();
}

uint32_t FrameAggregator::getOutputSize() const
{
    assert(m_impl);
    return m_impl->outputSize;
}

void FrameAggregator::evaluate(
    const std::vector<float>& embedding_matrix,
    std::vector<float>& out_scores)
{
    assert(m_impl);
    m_impl->runAggregator(embedding_matrix, out_scores);
}

NN_NAMESPACE_END
WAVE_NAMESPACE_END
