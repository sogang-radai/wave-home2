#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../core/json.h"

#define STT_NAMESPACE_BEGIN namespace stt {
#define STT_NAMESPACE_END }

WAVE_NAMESPACE_BEGIN
STT_NAMESPACE_BEGIN

enum Result
{
    SUCCESS = 0,
    ERROR_INVALID_CONFIG,
    ERROR_NOT_INITIALIZED,
    ERROR_INVALID_LOCALE,
    ERROR_INVALID_INPUT,
    ERROR_MODEL_LOAD,
    ERROR_RECOGNITION,
};

struct Capability
{
    std::string_view locale;
    std::string_view name;
    std::string_view language;
    std::string_view country;
    std::string_view modelType;
    uint32_t sampleRate = 0;
    bool streaming = false;
};

struct RecognizeResult
{
    std::string text;
    bool isEndpoint = false;
};

struct AudioInput
{
    const float* samples = nullptr;
    size_t sampleCount = 0;
    uint32_t sampleRate = 16000;
};

class Service
{
public:
    Service();
    ~Service();

    Result init(std::string_view base_dir, const json& config);
    void shutdown();

    const std::vector<Capability>& enumerateCapabilities() const;

    Result recognize(
        std::string_view locale,
        const AudioInput& input,
        RecognizeResult& out_result);
    std::future<Result> recognizeAsync(
        std::string_view locale,
        const AudioInput& input,
        RecognizeResult& out_result);

    using PartialResultCallback = std::function<void(const RecognizeResult& result)>;

    Result beginRecognizeStream(std::string_view locale, PartialResultCallback&& callback);
    Result pushAudio(std::string_view locale, const AudioInput& input);
    void endRecognizeStream(std::string_view locale);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

STT_NAMESPACE_END
WAVE_NAMESPACE_END
