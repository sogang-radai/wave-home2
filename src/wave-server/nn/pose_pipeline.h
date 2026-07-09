#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../core/json.h"

WAVE_NAMESPACE_BEGIN
NN_NAMESPACE_BEGIN

// COCO pose keypoint index (YOLO11-pose / Ultralytics default).
enum class PoseKeypointId : int32_t
{
    Nose = 0,
    LeftEye = 1,
    RightEye = 2,
    LeftEar = 3,
    RightEar = 4,
    LeftShoulder = 5,
    RightShoulder = 6,
    LeftElbow = 7,
    RightElbow = 8,
    LeftWrist = 9,
    RightWrist = 10,
    LeftHip = 11,
    RightHip = 12,
    LeftKnee = 13,
    RightKnee = 14,
    LeftAnkle = 15,
    RightAnkle = 16,
};

struct PoseKeypoint
{
    float x = 0.0f;
    float y = 0.0f;
    float confidence = 0.0f;
};

struct PosePerson
{
    int32_t label = 0;
    float confidence = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::vector<PoseKeypoint> keypoints;
};

struct PoseResult
{
    uint64_t timestampMs = 0;
    int imageWidth = 0;
    int imageHeight = 0;
    std::vector<PosePerson> persons;
};

// Loaded from bin/models/pose/model.json (see bin/models/pose/model.json).
struct PoseModelPaths
{
    std::string paramPath;
    std::string binPath;
    std::string inputName = "in0";
    std::string outputName = "out0";
    // Ultralytics ncnn export: single fused tensor (56 x 8400) = bbox + class + 17*3 kpts.
    // Manual pnnx workflow (yolo11_pose.cpp) uses separate out0/out1 instead.
    std::string outputLayout = "fused";
    uint32_t outputChannels = 56;
    uint32_t outputAnchors = 8400;
};

struct PoseModelThresholds
{
    float probThreshold = 0.25f;
    float nmsThreshold = 0.45f;
    float keypointThreshold = 0.2f;
};

struct PoseModelSpec
{
    std::string modelName;
    std::string modelVersion;
    std::string ultralyticsSource;
    std::string ultralyticsRelease;
    std::string ultralyticsWeightsUrl;
    std::string weightsPath;
    uint32_t inputSize = 640;
    uint32_t stride = 32;
    uint32_t classCount = 1;
    uint32_t keypointCount = 17;
    uint32_t keypointDims = 3;
    std::vector<std::string> classNames;
    PoseModelPaths paths;
    PoseModelThresholds thresholds;
};

// RGB8 image buffer, row-major, 3 channels.
struct RgbImageView
{
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int strideBytes = 0;
};

// YOLO11-pose inference via ncnn (post-process in C++, see yolo11_pose.cpp).
class PosePipeline
{
public:
    PosePipeline();
    ~PosePipeline();

    PosePipeline(const PosePipeline&) = delete;
    PosePipeline& operator=(const PosePipeline&) = delete;

    bool init(std::string_view base_dir, const json& config, std::string& out_error);
    void shutdown();

    bool isInitialized() const;

    const PoseModelSpec& getModelSpec() const;

    // Runs letterbox preprocess, ncnn forward, and YOLO11 pose decode/NMS.
    bool infer(const RgbImageView& image, PoseResult& out_result);

    bool inferJpeg(const uint8_t* jpeg_data, size_t jpeg_size, PoseResult& out_result);

private:
    struct Impl;

    std::unique_ptr<Impl> m_impl;
    PoseModelSpec m_spec;
    bool m_initialized = false;
};

NN_NAMESPACE_END
WAVE_NAMESPACE_END
