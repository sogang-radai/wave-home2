#include "stt_service.h"

#include "../core/task_queue.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include <sherpa-onnx/c-api/cxx-api.h>

WAVE_NAMESPACE_BEGIN
STT_NAMESPACE_BEGIN

namespace
{
    using SherpaOnlineRecognizer = sherpa_onnx::cxx::OnlineRecognizer;
    using SherpaOnlineStream = sherpa_onnx::cxx::OnlineStream;
    using SherpaOnlineRecognizerConfig = sherpa_onnx::cxx::OnlineRecognizerConfig;

    struct StoredCapability
    {
        std::string locale;
        std::string name;
        std::string language;
        std::string country;
        std::string modelType;
        uint32_t sampleRate = 0;
        bool streaming = false;

        Capability view() const
        {
            return {
                locale,
                name,
                language,
                country,
                modelType,
                sampleRate,
                streaming,
            };
        }
    };

    struct ActiveStream
    {
        bool open = false;
        std::optional<SherpaOnlineStream> stream;
        Service::PartialResultCallback callback;
    };

    struct LanguageRuntime
    {
        std::string locale;
        std::string modelType;
        uint32_t sampleRate = 0;
        bool streaming = false;
        std::optional<SherpaOnlineRecognizer> recognizer;
        ActiveStream activeStream;
    };

    bool localeEquals(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
            return false;

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                std::tolower(static_cast<unsigned char>(rhs[i])))
            {
                return false;
            }
        }
        return true;
    }

    std::string joinPath(const std::filesystem::path& base, std::string_view relative)
    {
        return (base / std::filesystem::path(relative)).string();
    }

    bool fileExists(const std::filesystem::path& path)
    {
        return std::filesystem::is_regular_file(path);
    }

    bool parseEndpoint(const json& endpoint_json, SherpaOnlineRecognizerConfig& out_config)
    {
        if (!endpoint_json.is_object())
            return false;

        out_config.enable_endpoint = true;
        out_config.rule1_min_trailing_silence =
            endpoint_json.value("rule1_min_trailing_silence", out_config.rule1_min_trailing_silence);
        out_config.rule2_min_trailing_silence =
            endpoint_json.value("rule2_min_trailing_silence", out_config.rule2_min_trailing_silence);
        out_config.rule3_min_utterance_length =
            endpoint_json.value("rule3_min_utterance_length", out_config.rule3_min_utterance_length);
        return true;
    }

    bool buildStreamingZipformerConfig(
        const std::filesystem::path& model_dir,
        const json& language_json,
        int32_t sample_rate,
        int32_t num_threads,
        SherpaOnlineRecognizerConfig& out_config)
    {
        for (const char* key : {"tokens", "encoder", "decoder", "joiner"})
        {
            if (!language_json.contains(key))
                return false;
        }

        const auto model_path = [&model_dir](const std::string& filename) {
            return joinPath(model_dir, filename);
        };

        const std::string tokens = language_json["tokens"].get<std::string>();
        const std::string encoder = language_json["encoder"].get<std::string>();
        const std::string decoder = language_json["decoder"].get<std::string>();
        const std::string joiner = language_json["joiner"].get<std::string>();

        if (!fileExists(model_dir / tokens) ||
            !fileExists(model_dir / encoder) ||
            !fileExists(model_dir / decoder) ||
            !fileExists(model_dir / joiner))
        {
            return false;
        }

        out_config = SherpaOnlineRecognizerConfig{};
        out_config.feat_config.sample_rate = sample_rate;
        out_config.model_config.transducer.encoder = model_path(encoder);
        out_config.model_config.transducer.decoder = model_path(decoder);
        out_config.model_config.transducer.joiner = model_path(joiner);
        out_config.model_config.tokens = model_path(tokens);
        out_config.model_config.num_threads = num_threads;
        out_config.model_config.debug = false;

        if (language_json.contains("bpe_vocab"))
        {
            const std::string bpe_vocab = language_json["bpe_vocab"].get<std::string>();
            if (!bpe_vocab.empty() && fileExists(model_dir / bpe_vocab))
            {
                out_config.model_config.bpe_vocab = model_path(bpe_vocab);
                out_config.model_config.modeling_unit = "bpe";
            }
        }

        out_config.decoding_method = language_json.value("decoding_method", "greedy_search");

        if (language_json.contains("endpoint"))
        {
            if (!parseEndpoint(language_json["endpoint"], out_config))
                return false;
        }

        return true;
    }

    Result validateAudioInput(const LanguageRuntime* runtime, const AudioInput& input)
    {
        if (runtime == nullptr)
            return ERROR_INVALID_LOCALE;

        if (input.samples == nullptr || input.sampleCount == 0)
            return ERROR_INVALID_INPUT;

        if (input.sampleRate == 0)
            return ERROR_INVALID_INPUT;

        if (runtime->sampleRate == 0)
            return ERROR_INVALID_CONFIG;

        return SUCCESS;
    }

    void decodeReadyFrames(LanguageRuntime& runtime, SherpaOnlineStream& stream)
    {
        while (runtime.recognizer->IsReady(&stream))
            runtime.recognizer->Decode(&stream);
    }

    RecognizeResult makeRecognizeResult(
        LanguageRuntime& runtime,
        SherpaOnlineStream& stream)
    {
        RecognizeResult out_result;
        const auto result = runtime.recognizer->GetResult(&stream);
        out_result.text = result.text;
        out_result.isEndpoint = runtime.recognizer->IsEndpoint(&stream);
        return out_result;
    }

    void emitPartialResult(LanguageRuntime& runtime, SherpaOnlineStream& stream)
    {
        if (!runtime.activeStream.callback)
            return;

        runtime.activeStream.callback(makeRecognizeResult(runtime, stream));
    }
}

struct Service::Impl
{
    bool initialized = false;
    std::filesystem::path baseDir;
    int32_t numThreads = 2;
    int32_t defaultSampleRate = 16000;

    std::vector<StoredCapability> capabilityStorage;
    std::vector<Capability> capabilities;
    std::vector<LanguageRuntime> engines;

    Result loadLanguage(
        const json& language_json,
        const std::filesystem::path& base_dir,
        int32_t num_threads,
        int32_t default_sample_rate)
    {
        if (!language_json.is_object())
            return ERROR_INVALID_CONFIG;

        if (!language_json.value("enabled", false))
            return SUCCESS;

        for (const char* key :
             {"locale", "name", "language", "country", "path", "model_type", "streaming",
              "tokens", "encoder", "decoder", "joiner"})
        {
            if (!language_json.contains(key))
                return ERROR_INVALID_CONFIG;
        }

        const std::string model_type = language_json["model_type"].get<std::string>();
        if (model_type != "streaming_zipformer_transducer")
            return ERROR_INVALID_CONFIG;

        if (!language_json["streaming"].get<bool>())
            return ERROR_INVALID_CONFIG;

        const std::string locale = language_json["locale"].get<std::string>();
        const std::string model_subdir = language_json["path"].get<std::string>();
        const std::filesystem::path model_dir =
            base_dir / "models" / "stt" / model_subdir;

        const uint32_t sample_rate = language_json.value(
            "sample_rate",
            static_cast<uint32_t>(default_sample_rate));

        SherpaOnlineRecognizerConfig recognizer_config;
        if (!buildStreamingZipformerConfig(
                model_dir,
                language_json,
                static_cast<int32_t>(sample_rate),
                num_threads,
                recognizer_config))
        {
            return ERROR_MODEL_LOAD;
        }

        auto recognizer = SherpaOnlineRecognizer::Create(recognizer_config);
        if (!recognizer.Get())
            return ERROR_MODEL_LOAD;

        StoredCapability stored_capability;
        stored_capability.locale = locale;
        stored_capability.name = language_json["name"].get<std::string>();
        stored_capability.language = language_json["language"].get<std::string>();
        stored_capability.country = language_json["country"].get<std::string>();
        stored_capability.modelType = model_type;
        stored_capability.sampleRate = sample_rate;
        stored_capability.streaming = true;

        LanguageRuntime runtime;
        runtime.locale = locale;
        runtime.modelType = model_type;
        runtime.sampleRate = sample_rate;
        runtime.streaming = true;
        runtime.recognizer = std::move(recognizer);

        capabilityStorage.push_back(std::move(stored_capability));
        capabilities.push_back(capabilityStorage.back().view());
        engines.push_back(std::move(runtime));
        return SUCCESS;
    }

    LanguageRuntime* findEngine(std::string_view locale)
    {
        for (auto& engine : engines)
        {
            if (localeEquals(engine.locale, locale))
                return &engine;
        }
        return nullptr;
    }

    const LanguageRuntime* findEngine(std::string_view locale) const
    {
        for (const auto& engine : engines)
        {
            if (localeEquals(engine.locale, locale))
                return &engine;
        }
        return nullptr;
    }
};

Service::Service() :
    m_impl(std::make_unique<Impl>())
{
}

Service::~Service()
{
    shutdown();
}

Result Service::init(std::string_view base_dir, const json& config)
{
    shutdown();

    if (!config.is_object() || !config.contains("languages") || !config["languages"].is_array())
        return ERROR_INVALID_CONFIG;

    const std::filesystem::path base_path = base_dir.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(base_dir);

    m_impl->baseDir = base_path;
    m_impl->numThreads = config.value("num_threads", 2);
    m_impl->defaultSampleRate = config.value("sample_rate", 16000);

    for (const auto& language_json : config["languages"])
    {
        const Result load_result = m_impl->loadLanguage(
            language_json,
            base_path,
            m_impl->numThreads,
            m_impl->defaultSampleRate);
        if (load_result != SUCCESS)
            return load_result;
    }

    if (m_impl->engines.empty())
        return ERROR_MODEL_LOAD;

    m_impl->initialized = true;
    return SUCCESS;
}

void Service::shutdown()
{
    if (!m_impl)
        return;

    m_impl->engines.clear();
    m_impl->capabilities.clear();
    m_impl->capabilityStorage.clear();
    m_impl->initialized = false;
}

const std::vector<Capability>& Service::enumerateCapabilities() const
{
    return m_impl->capabilities;
}

Result Service::recognize(
    std::string_view locale,
    const AudioInput& input,
    RecognizeResult& out_result)
{
    if (!m_impl->initialized)
        return ERROR_NOT_INITIALIZED;

    LanguageRuntime* runtime = m_impl->findEngine(locale);
    const Result validation = validateAudioInput(runtime, input);
    if (validation != SUCCESS)
        return validation;

    try
    {
        SherpaOnlineStream stream = runtime->recognizer->CreateStream();
        stream.AcceptWaveform(
            static_cast<int32_t>(input.sampleRate),
            input.samples,
            static_cast<int32_t>(input.sampleCount));
        stream.InputFinished();

        decodeReadyFrames(*runtime, stream);
        out_result = makeRecognizeResult(*runtime, stream);
        return SUCCESS;
    }
    catch (const std::exception&)
    {
        return ERROR_RECOGNITION;
    }
}

std::future<Result> Service::recognizeAsync(
    std::string_view locale,
    const AudioInput& input,
    RecognizeResult& out_result)
{
    const std::string locale_copy(locale);
    return TaskQueue::enqueueAsync([this, locale_copy, input, &out_result]() {
        return recognize(locale_copy, input, out_result);
    });
}

Result Service::beginRecognizeStream(
    std::string_view locale,
    PartialResultCallback&& callback)
{
    if (!m_impl->initialized)
        return ERROR_NOT_INITIALIZED;

    LanguageRuntime* runtime = m_impl->findEngine(locale);
    if (runtime == nullptr)
        return ERROR_INVALID_LOCALE;

    if (!runtime->streaming || !runtime->recognizer.has_value())
        return ERROR_INVALID_CONFIG;

    if (runtime->activeStream.open)
        return ERROR_INVALID_INPUT;

    try
    {
        runtime->activeStream.stream = runtime->recognizer->CreateStream();
        runtime->activeStream.callback = std::move(callback);
        runtime->activeStream.open = true;
        return SUCCESS;
    }
    catch (const std::exception&)
    {
        runtime->activeStream = {};
        return ERROR_RECOGNITION;
    }
}

Result Service::pushAudio(std::string_view locale, const AudioInput& input)
{
    if (!m_impl->initialized)
        return ERROR_NOT_INITIALIZED;

    LanguageRuntime* runtime = m_impl->findEngine(locale);
    const Result validation = validateAudioInput(runtime, input);
    if (validation != SUCCESS)
        return validation;

    if (!runtime->activeStream.open || !runtime->activeStream.stream.has_value())
        return ERROR_INVALID_INPUT;

    try
    {
        SherpaOnlineStream& stream = *runtime->activeStream.stream;
        stream.AcceptWaveform(
            static_cast<int32_t>(input.sampleRate),
            input.samples,
            static_cast<int32_t>(input.sampleCount));

        decodeReadyFrames(*runtime, stream);
        emitPartialResult(*runtime, stream);

        if (runtime->recognizer->IsEndpoint(&stream))
        {
            runtime->recognizer->Reset(&stream);
        }

        return SUCCESS;
    }
    catch (const std::exception&)
    {
        return ERROR_RECOGNITION;
    }
}

void Service::endRecognizeStream(std::string_view locale)
{
    if (!m_impl->initialized)
        return;

    LanguageRuntime* runtime = m_impl->findEngine(locale);
    if (runtime == nullptr || !runtime->activeStream.open || !runtime->activeStream.stream)
        return;

    try
    {
        SherpaOnlineStream& stream = *runtime->activeStream.stream;
        stream.InputFinished();
        decodeReadyFrames(*runtime, stream);
        emitPartialResult(*runtime, stream);
    }
    catch (const std::exception&)
    {
    }

    runtime->activeStream = {};
}

STT_NAMESPACE_END
WAVE_NAMESPACE_END
