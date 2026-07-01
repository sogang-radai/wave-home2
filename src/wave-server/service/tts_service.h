#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../core/json.h"

#define TTS_NAMESPACE_BEGIN namespace tts {
#define TTS_NAMESPACE_END }

WAVE_NAMESPACE_BEGIN
TTS_NAMESPACE_BEGIN

enum Result
{
    SUCCESS = 0,
    ERROR_INVALID_CONFIG,
    ERROR_NOT_INITIALIZED,
    ERROR_INVALID_LOCALE,
    ERROR_INVALID_SPEAKER,
    ERROR_INVALID_INPUT,
    ERROR_MODEL_LOAD,
    ERROR_GENERATION,
};

struct Speaker
{
    uint32_t speakerID = 0;
    std::string_view name;
    std::string_view description;
    std::string_view character;
    std::string_view gender;
};

struct Capability
{
    std::string_view locale;
    std::string_view name;
    std::string_view language;
    std::string_view country;
    std::vector<Speaker> speakers;
};

struct Input
{
    std::string_view locale;
    std::string_view text;
    uint32_t speakerID = 0;
    float speed = 1.0f;
    float pitch = 1.0f;
    float volume = 1.0f;
    int32_t numSteps = 0;
};

class Service
{
public:
    Service();
    ~Service();

    Result init(std::string_view base_dir, const json& config);
    void shutdown();

    const std::vector<Capability>& enumerateCapabilities() const;

    int32_t sampleRate(std::string_view locale) const;

    Result generate(const Input& input, std::vector<float>& out_audio);
    std::future<Result> generateAsync(const Input& input, std::vector<float>& out_audio);

    using GenerateStreamCallback = std::function<void(const float* buffer, size_t size, size_t chunk)>;

    Result generateStream(const Input& input, GenerateStreamCallback&& callback);
    std::future<Result> generateStreamAsync(const Input& input, GenerateStreamCallback&& callback);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

TTS_NAMESPACE_END
WAVE_NAMESPACE_END
