#include "frame_encoder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

#include <net.h>

WAVE_NAMESPACE_BEGIN
NN_NAMESPACE_BEGIN

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    struct NormalizedPoint
    {
        union
        {
            struct
            {
                float x;
                float y;
                float z;
            };
            struct
            {
                float r;
                float theta;
                float phi;
            };
        };
        float doppler;
        float power;
    };

    bool parse_range(const json& arr, float& out_min, float& out_max, const char* name)
    {
        if (!arr.is_array() || arr.size() != 2)
            return false;

        out_min = arr[0].get<float>();
        out_max = arr[1].get<float>();
        (void)name;
        return true;
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

struct FrameEncoder::Impl
{
    std::string name;
    std::string networkType;
    std::string paramPath;
    std::string binPath;
    std::string inputName = "in0";
    std::string outputName = "out0";

    uint32_t embeddingSize = 0;
    uint32_t sequenceLength = 0;
    FrameEncoderNormalizeSettings normalizeSettings {};
    bool loaded = false;

    ncnn::Net pointNet;
    ncnn::Mat inputMat;

    size_t maxQueueSize = 0;

    struct CachedFrame
    {
        uint64_t frameIndex = 0;
        std::vector<float> embedding;
    };

    std::deque<std::unique_ptr<CachedFrame>> frameQueue;
    std::unordered_map<uint64_t, CachedFrame*> frameMap;

    std::vector<float> embeddingMatrixCache;
    bool matrixCacheValid = false;
    uint64_t cacheWindowBegin = 0;
    uint64_t cacheWindowEnd = 0;

    bool parseConfig(std::string_view base_dir, const json& config, std::string& out_error);
    bool loadModel(std::string& out_error);

    float normalizeAxisValue(float value, float min_val, float max_val) const;
    float normalizePowerValue(float power) const;
    std::array<float, 3> cartesianToPolar(float x, float y, float z) const;
    void normalizePoints(
        const std::vector<dev::RadarPointCloud::Point>& points,
        std::vector<NormalizedPoint>& out) const;

    void runEncoder(const std::vector<NormalizedPoint>& points, std::vector<float>& out_embedding);
    void upsertFrame(const dev::RadarPointCloud& frame);
    void trimQueueIfNeeded();

    const CachedFrame* findFrame(uint64_t frame_idx) const;
    void invalidateMatrixCache();
    bool findLatestConsecutiveWindow(uint64_t& out_begin, uint64_t& out_end) const;
    bool getEmbeddingMatrix(std::vector<float>& out_mat);
};

bool FrameEncoder::Impl::parseConfig(
    std::string_view base_dir,
    const json& config,
    std::string& out_error)
{
    try
    {
        if (!config.is_object())
        {
            out_error = "frame_encoder config must be a JSON object";
            return false;
        }

        for (const char* key : {"name", "type", "sequence_length", "normalization", "param_path", "bin_path", "output_size"})
        {
            if (!config.contains(key))
            {
                out_error = std::string("frame_encoder config missing required field '") + key + "'";
                return false;
            }
        }

        name = config["name"].get<std::string>();
        networkType = config["type"].get<std::string>();
        if (networkType != "pointnet")
        {
            out_error = "frame_encoder type must be 'pointnet'";
            return false;
        }

        sequenceLength = config["sequence_length"].get<uint32_t>();
        if (sequenceLength == 0)
        {
            out_error = "sequence_length must be greater than 0";
            return false;
        }

        embeddingSize = config["output_size"].get<uint32_t>();
        if (embeddingSize == 0)
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

        const auto& norm = config["normalization"];
        if (!norm.is_object() || !norm.contains("coordinate_mode") || !norm.contains("power_mode") || !norm.contains("ranges"))
        {
            out_error = "frame_encoder normalization must include coordinate_mode, power_mode, and ranges";
            return false;
        }

        const std::string coordinate_mode = norm["coordinate_mode"].get<std::string>();
        if (coordinate_mode == "cartesian")
            normalizeSettings.coordinateMode = FrameEncoderNormalizeSettings::CoordinateMode::Cartesian;
        else if (coordinate_mode == "polar")
            normalizeSettings.coordinateMode = FrameEncoderNormalizeSettings::CoordinateMode::Polar;
        else
        {
            out_error = "coordinate_mode must be 'cartesian' or 'polar'";
            return false;
        }

        const std::string power_mode = norm["power_mode"].get<std::string>();
        if (power_mode == "linear")
            normalizeSettings.powerMode = FrameEncoderNormalizeSettings::PowerMode::Linear;
        else if (power_mode == "db")
            normalizeSettings.powerMode = FrameEncoderNormalizeSettings::PowerMode::Db;
        else
        {
            out_error = "power_mode must be 'linear' or 'db'";
            return false;
        }

        const auto& ranges = norm["ranges"];
        if (!ranges.is_object())
        {
            out_error = "normalization ranges must be an object";
            return false;
        }

        if (!parse_range(ranges["x"], normalizeSettings.cartesian.minX, normalizeSettings.cartesian.maxX, "x") ||
            !parse_range(ranges["y"], normalizeSettings.cartesian.minY, normalizeSettings.cartesian.maxY, "y") ||
            !parse_range(ranges["z"], normalizeSettings.cartesian.minZ, normalizeSettings.cartesian.maxZ, "z") ||
            !parse_range(ranges["doppler"], normalizeSettings.minDoppler, normalizeSettings.maxDoppler, "doppler"))
        {
            out_error = "normalization ranges x, y, z, and doppler must be [min, max] arrays";
            return false;
        }

        if (normalizeSettings.powerMode == FrameEncoderNormalizeSettings::PowerMode::Linear)
        {
            if (!parse_range(
                    ranges["power"],
                    normalizeSettings.linear.minPower,
                    normalizeSettings.linear.maxPower,
                    "power"))
            {
                out_error = "normalization range power must be a [min, max] array";
                return false;
            }
        }
        else if (!parse_range(
                     ranges["power"],
                     normalizeSettings.db.minPowerDb,
                     normalizeSettings.db.maxPowerDb,
                     "power"))
        {
            out_error = "normalization range power must be a [min, max] array";
            return false;
        }

        maxQueueSize = sequenceLength;
        return true;
    }
    catch (const json::exception& e)
    {
        out_error = std::string("frame_encoder config parse error: ") + e.what();
        return false;
    }
}

bool FrameEncoder::Impl::loadModel(std::string& out_error)
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
    if (pointNet.load_param(paramPath.c_str()) != 0)
    {
        out_error = "Failed to load param: " + paramPath;
        return false;
    }
    if (pointNet.load_model(binPath.c_str()) != 0)
    {
        out_error = "Failed to load bin: " + binPath;
        return false;
    }

    loaded = true;
    return true;
}

float FrameEncoder::Impl::normalizeAxisValue(float value, float min_val, float max_val) const
{
    if (max_val <= min_val)
        return 0.f;
    return (value - min_val) / (max_val - min_val) * 2.f - 1.f;
}

float FrameEncoder::Impl::normalizePowerValue(float power) const
{
    float v = power;
    if (normalizeSettings.powerMode == FrameEncoderNormalizeSettings::PowerMode::Db)
        v = 10.f * std::log10(power + 1.f);

    if (normalizeSettings.powerMode == FrameEncoderNormalizeSettings::PowerMode::Linear)
    {
        return normalizeAxisValue(
            v,
            normalizeSettings.linear.minPower,
            normalizeSettings.linear.maxPower);
    }

    return normalizeAxisValue(
        v,
        normalizeSettings.db.minPowerDb,
        normalizeSettings.db.maxPowerDb);
}

std::array<float, 3> FrameEncoder::Impl::cartesianToPolar(float x, float y, float z) const
{
    const float r = std::sqrt(x * x + y * y + z * z);
    const float theta = std::atan2(y, x);
    const float phi = std::atan2(z, std::sqrt(x * x + y * y));
    return {r, theta, phi};
}

void FrameEncoder::Impl::normalizePoints(
    const std::vector<dev::RadarPointCloud::Point>& points,
    std::vector<NormalizedPoint>& out) const
{
    out.clear();
    out.reserve(points.size());

    for (const auto& point : points)
    {
        NormalizedPoint np {};
        if (normalizeSettings.coordinateMode == FrameEncoderNormalizeSettings::CoordinateMode::Polar)
        {
            const auto [r, theta, phi] = cartesianToPolar(point.x, point.y, point.z);
            const float max_range = std::max({
                normalizeSettings.cartesian.maxX - normalizeSettings.cartesian.minX,
                normalizeSettings.cartesian.maxY - normalizeSettings.cartesian.minY,
                normalizeSettings.cartesian.maxZ - normalizeSettings.cartesian.minZ,
            });
            np.r = normalizeAxisValue(r, 0.f, max_range);
            np.theta = normalizeAxisValue(theta, -kPi, kPi);
            np.phi = normalizeAxisValue(phi, -kPi * 0.5f, kPi * 0.5f);
        }
        else
        {
            np.x = normalizeAxisValue(
                point.x,
                normalizeSettings.cartesian.minX,
                normalizeSettings.cartesian.maxX);
            np.y = normalizeAxisValue(
                point.y,
                normalizeSettings.cartesian.minY,
                normalizeSettings.cartesian.maxY);
            np.z = normalizeAxisValue(
                point.z,
                normalizeSettings.cartesian.minZ,
                normalizeSettings.cartesian.maxZ);
        }

        np.doppler = normalizeAxisValue(
            point.doppler,
            normalizeSettings.minDoppler,
            normalizeSettings.maxDoppler);
        np.power = normalizePowerValue(point.power);
        out.push_back(np);
    }
}

void FrameEncoder::Impl::runEncoder(
    const std::vector<NormalizedPoint>& points,
    std::vector<float>& out_embedding)
{
    const int point_count = static_cast<int>(points.size());
    if (point_count == 0)
    {
        out_embedding.assign(embeddingSize, 0.f);
        return;
    }

    inputMat.create(point_count, 5);
    for (int i = 0; i < point_count; ++i)
    {
        const NormalizedPoint& p = points[static_cast<size_t>(i)];
        if (normalizeSettings.coordinateMode == FrameEncoderNormalizeSettings::CoordinateMode::Polar)
        {
            inputMat.row(0)[i] = p.r;
            inputMat.row(1)[i] = p.theta;
            inputMat.row(2)[i] = p.phi;
        }
        else
        {
            inputMat.row(0)[i] = p.x;
            inputMat.row(1)[i] = p.y;
            inputMat.row(2)[i] = p.z;
        }
        inputMat.row(3)[i] = p.doppler;
        inputMat.row(4)[i] = p.power;
    }

    ncnn::Extractor extractor = pointNet.create_extractor();
    extractor.input(inputName.c_str(), inputMat);

    ncnn::Mat output;
    if (extractor.extract(outputName.c_str(), output) != 0)
        throw std::runtime_error("frame_encoder extract failed");

    extract_flat(output, embeddingSize, out_embedding);
}

void FrameEncoder::Impl::upsertFrame(const dev::RadarPointCloud& frame)
{
    const uint64_t frame_idx = frame.frameIndex;

    std::vector<NormalizedPoint> normalized_points;
    normalizePoints(frame.points, normalized_points);

    const auto it = frameMap.find(frame_idx);
    if (it != frameMap.end())
    {
        invalidateMatrixCache();
        runEncoder(normalized_points, it->second->embedding);
        return;
    }

    if (matrixCacheValid && frame_idx != cacheWindowEnd + 1)
        invalidateMatrixCache();

    auto owned = std::make_unique<CachedFrame>();
    owned->frameIndex = frame_idx;
    runEncoder(normalized_points, owned->embedding);

    CachedFrame* cached = owned.get();
    frameQueue.push_back(std::move(owned));
    frameMap.emplace(frame_idx, cached);

    trimQueueIfNeeded();
}

void FrameEncoder::Impl::invalidateMatrixCache()
{
    matrixCacheValid = false;
    cacheWindowBegin = 0;
    cacheWindowEnd = 0;
}

bool FrameEncoder::Impl::findLatestConsecutiveWindow(uint64_t& out_begin, uint64_t& out_end) const
{
    if (frameMap.empty())
        return false;

    out_end = 0;
    for (const auto& [frame_idx, frame] : frameMap)
    {
        (void)frame;
        out_end = std::max(out_end, frame_idx);
    }

    if (out_end + 1 < sequenceLength)
        return false;

    out_begin = out_end - (sequenceLength - 1);
    for (uint64_t frame_idx = out_begin; frame_idx <= out_end; ++frame_idx)
    {
        if (frameMap.find(frame_idx) == frameMap.end())
            return false;
    }

    return true;
}

bool FrameEncoder::Impl::getEmbeddingMatrix(std::vector<float>& out_mat)
{
    uint64_t begin = 0;
    uint64_t end = 0;
    if (!findLatestConsecutiveWindow(begin, end))
        return false;

    const size_t matrix_size = static_cast<size_t>(sequenceLength) * embeddingSize;
    if (embeddingMatrixCache.size() != matrix_size)
        embeddingMatrixCache.assign(matrix_size, 0.f);

    if (matrixCacheValid &&
        cacheWindowBegin + 1 == begin &&
        cacheWindowEnd + 1 == end)
    {
        const size_t row_bytes = static_cast<size_t>(embeddingSize) * sizeof(float);
        const size_t shift_bytes = static_cast<size_t>(sequenceLength - 1) * row_bytes;
        std::memmove(
            embeddingMatrixCache.data(),
            embeddingMatrixCache.data() + embeddingSize,
            shift_bytes);

        const CachedFrame* frame = findFrame(end);
        if (!frame)
            return false;

        std::memcpy(
            embeddingMatrixCache.data() + shift_bytes,
            frame->embedding.data(),
            row_bytes);

        cacheWindowBegin = begin;
        cacheWindowEnd = end;
        out_mat = embeddingMatrixCache;
        return true;
    }

    if (matrixCacheValid && cacheWindowBegin == begin && cacheWindowEnd == end)
    {
        out_mat = embeddingMatrixCache;
        return true;
    }

    for (uint64_t row = 0; row < sequenceLength; ++row)
    {
        const CachedFrame* frame = findFrame(begin + row);
        if (!frame)
            return false;

        std::memcpy(
            embeddingMatrixCache.data() + static_cast<size_t>(row) * embeddingSize,
            frame->embedding.data(),
            static_cast<size_t>(embeddingSize) * sizeof(float));
    }

    cacheWindowBegin = begin;
    cacheWindowEnd = end;
    matrixCacheValid = true;
    out_mat = embeddingMatrixCache;
    return true;
}

void FrameEncoder::Impl::trimQueueIfNeeded()
{
    const size_t min_size = std::max(maxQueueSize, static_cast<size_t>(sequenceLength));
    while (frameMap.size() > min_size)
    {
        const uint64_t oldest = frameQueue.front()->frameIndex;
        if (matrixCacheValid && oldest >= cacheWindowBegin && oldest <= cacheWindowEnd)
            invalidateMatrixCache();

        frameMap.erase(oldest);
        frameQueue.pop_front();
    }
}

const FrameEncoder::Impl::CachedFrame* FrameEncoder::Impl::findFrame(uint64_t frame_idx) const
{
    const auto it = frameMap.find(frame_idx);
    return it != frameMap.end() ? it->second : nullptr;
}

FrameEncoder::FrameEncoder() :
    m_impl(nullptr)
{
}

FrameEncoder::~FrameEncoder()
{
    shutdown();
}

bool FrameEncoder::init(std::string_view base_dir, const json& config, std::string& out_error)
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

void FrameEncoder::shutdown()
{
    m_impl.reset();
}

uint32_t FrameEncoder::getEmbeddingSize() const
{
    assert(m_impl);
    return m_impl->embeddingSize;
}

uint32_t FrameEncoder::getSequenceLength() const
{
    assert(m_impl);
    return m_impl->sequenceLength;
}

const FrameEncoderNormalizeSettings& FrameEncoder::getNormalizeSettings() const
{
    assert(m_impl);
    return m_impl->normalizeSettings;
}

void FrameEncoder::setQueueSize(size_t size)
{
    assert(m_impl);
    m_impl->maxQueueSize = std::max(size, static_cast<size_t>(m_impl->sequenceLength));
    m_impl->trimQueueIfNeeded();
}

size_t FrameEncoder::getQueueSize() const
{
    assert(m_impl);
    return m_impl->maxQueueSize;
}

void FrameEncoder::pushFrame(const dev::RadarPointCloud& frame)
{
    assert(m_impl);
    m_impl->upsertFrame(frame);
}

void FrameEncoder::pushFrame(dev::RadarPointCloud&& frame)
{
    assert(m_impl);
    m_impl->upsertFrame(frame);
}

void FrameEncoder::enumerateFrameIndices(std::vector<uint64_t>& indices) const
{
    assert(m_impl);
    indices.clear();
    indices.reserve(m_impl->frameQueue.size());
    for (const auto& frame : m_impl->frameQueue)
        indices.push_back(frame->frameIndex);
}

bool FrameEncoder::isFrameAvailable(uint64_t frame_idx) const
{
    assert(m_impl);
    return m_impl->findFrame(frame_idx) != nullptr;
}

bool FrameEncoder::getFrameEmbedding(uint64_t frame_idx, std::vector<float>& out_embedding) const
{
    assert(m_impl);
    const auto* frame = m_impl->findFrame(frame_idx);
    if (!frame)
        return false;

    out_embedding = frame->embedding;
    return true;
}

bool FrameEncoder::getEmbeddingMatrix(std::vector<float>& out_mat)
{
    assert(m_impl);
    return m_impl->getEmbeddingMatrix(out_mat);
}

NN_NAMESPACE_END
WAVE_NAMESPACE_END
