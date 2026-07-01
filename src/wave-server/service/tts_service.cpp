#include "tts_service.h"

#include "../core/task_queue.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include <sherpa-onnx/c-api/cxx-api.h>

WAVE_NAMESPACE_BEGIN
TTS_NAMESPACE_BEGIN

namespace
{
    using SherpaOfflineTts = sherpa_onnx::cxx::OfflineTts;
    using SherpaGenerationConfig = sherpa_onnx::cxx::GenerationConfig;
    using SherpaOfflineTtsConfig = sherpa_onnx::cxx::OfflineTtsConfig;

    struct StoredSpeaker
    {
        uint32_t speakerID = 0;
        std::string name;
        std::string description;
        std::string character;
        std::string gender;

        Speaker view() const
        {
            return {
                speakerID,
                name,
                description,
                character,
                gender,
            };
        }
    };

    struct StoredCapability
    {
        std::string locale;
        std::string name;
        std::string language;
        std::string country;
        std::vector<StoredSpeaker> speakers;

        Capability view() const
        {
            Capability capability;
            capability.locale = locale;
            capability.name = name;
            capability.language = language;
            capability.country = country;
            capability.speakers.reserve(speakers.size());
            for (const auto& speaker : speakers)
                capability.speakers.push_back(speaker.view());
            return capability;
        }
    };

    struct LanguageRuntime
    {
        std::string locale;
        std::string lang;
        std::optional<SherpaOfflineTts> tts;
        int32_t sampleRate = 0;
        std::vector<StoredSpeaker> speakers;
    };

    struct StreamContext
    {
        Service::GenerateStreamCallback* callback = nullptr;
        size_t chunk = 0;
    };

    std::string localeToLang(std::string_view locale)
    {
        const size_t dash = locale.find('-');
        const std::string_view lang_part =
            dash == std::string_view::npos ? locale : locale.substr(0, dash);

        std::string lang;
        lang.reserve(lang_part.size());
        for (char ch : lang_part)
            lang.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        return lang;
    }

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

    bool parseSpeakers(const json& speakers_json, std::vector<StoredSpeaker>& out_speakers)
    {
        if (!speakers_json.is_array())
            return false;

        out_speakers.clear();
        out_speakers.reserve(speakers_json.size());
        for (const auto& speaker_json : speakers_json)
        {
            if (!speaker_json.is_object())
                return false;

            StoredSpeaker speaker;
            speaker.speakerID = speaker_json.value("sid", 0);
            speaker.name = speaker_json.value("name", "");
            speaker.description = speaker_json.value("description", "");
            speaker.character = speaker_json.value("character", "");
            speaker.gender = speaker_json.value("gender", "");
            out_speakers.push_back(std::move(speaker));
        }
        return true;
    }

    bool buildSupertonicConfig(
        const std::filesystem::path& model_dir,
        int32_t num_threads,
        SherpaOfflineTtsConfig& out_config)
    {
        const auto model_path = [&model_dir](const char* filename) {
            return joinPath(model_dir, filename);
        };

        const std::array<const char*, 7> required_files = {
            "duration_predictor.int8.onnx",
            "text_encoder.int8.onnx",
            "vector_estimator.int8.onnx",
            "vocoder.int8.onnx",
            "tts.json",
            "unicode_indexer.bin",
            "voice.bin",
        };

        for (const char* filename : required_files)
        {
            if (!fileExists(model_dir / filename))
                return false;
        }

        out_config = SherpaOfflineTtsConfig{};
        out_config.model.supertonic.duration_predictor =
            model_path("duration_predictor.int8.onnx");
        out_config.model.supertonic.text_encoder = model_path("text_encoder.int8.onnx");
        out_config.model.supertonic.vector_estimator =
            model_path("vector_estimator.int8.onnx");
        out_config.model.supertonic.vocoder = model_path("vocoder.int8.onnx");
        out_config.model.supertonic.tts_json = model_path("tts.json");
        out_config.model.supertonic.unicode_indexer = model_path("unicode_indexer.bin");
        out_config.model.supertonic.voice_style = model_path("voice.bin");
        out_config.model.num_threads = num_threads;
        out_config.model.debug = false;
        return true;
    }

    SherpaGenerationConfig makeGenerationConfig(
        const LanguageRuntime& runtime,
        const Input& input)
    {
        SherpaGenerationConfig gen_config;
        gen_config.sid = static_cast<int32_t>(input.speakerID);
        gen_config.speed = input.speed;
        gen_config.num_steps = input.numSteps > 0 ? input.numSteps : 8;
        gen_config.extra["lang"] = runtime.lang;
        return gen_config;
    }

    int32_t streamProgressCallback(
        const float* samples,
        int32_t num_samples,
        float /*progress*/,
        void* arg)
    {
        auto* context = static_cast<StreamContext*>(arg);
        if (context != nullptr && context->callback != nullptr && num_samples > 0)
            (*context->callback)(samples, static_cast<size_t>(num_samples), context->chunk++);

        return 1;
    }

    Result validateInput(const LanguageRuntime* runtime, const Input& input)
    {
        if (input.text.empty())
            return ERROR_INVALID_INPUT;

        if (runtime == nullptr)
            return ERROR_INVALID_LOCALE;

        if (runtime->speakers.empty())
            return ERROR_INVALID_SPEAKER;

        const bool speaker_found = std::any_of(
            runtime->speakers.begin(),
            runtime->speakers.end(),
            [&](const StoredSpeaker& speaker) { return speaker.speakerID == input.speakerID; });
        if (!speaker_found)
            return ERROR_INVALID_SPEAKER;

        return SUCCESS;
    }
}

struct Service::Impl
{
    bool initialized = false;
    std::filesystem::path baseDir;
    int32_t numThreads = 2;

    std::vector<StoredCapability> capabilityStorage;
    std::vector<Capability> capabilities;
    std::vector<LanguageRuntime> engines;
    std::unordered_map<std::string, size_t> localeToEngine;

    Result loadLanguage(
        const json& language_json,
        const std::filesystem::path& base_dir,
        int32_t num_threads)
    {
        if (!language_json.is_object())
            return ERROR_INVALID_CONFIG;

        if (!language_json.value("enabled", false))
            return SUCCESS;

        for (const char* key : {"locale", "name", "language", "country", "path", "speakers"})
        {
            if (!language_json.contains(key))
                return ERROR_INVALID_CONFIG;
        }

        const std::string locale = language_json["locale"].get<std::string>();
        const std::string model_subdir = language_json["path"].get<std::string>();
        const std::filesystem::path model_dir =
            base_dir / "models" / "tts" / model_subdir;

        std::vector<StoredSpeaker> speakers;
        if (!parseSpeakers(language_json["speakers"], speakers) || speakers.empty())
            return ERROR_INVALID_CONFIG;

        SherpaOfflineTtsConfig tts_config;
        if (!buildSupertonicConfig(model_dir, num_threads, tts_config))
            return ERROR_MODEL_LOAD;

        auto tts = SherpaOfflineTts::Create(tts_config);
        if (tts.SampleRate() <= 0)
            return ERROR_MODEL_LOAD;

        StoredCapability stored_capability;
        stored_capability.locale = locale;
        stored_capability.name = language_json["name"].get<std::string>();
        stored_capability.language = language_json["language"].get<std::string>();
        stored_capability.country = language_json["country"].get<std::string>();
        stored_capability.speakers = speakers;

        LanguageRuntime runtime;
        runtime.locale = locale;
        runtime.lang = localeToLang(locale);
        runtime.tts = std::move(tts);
        runtime.sampleRate = runtime.tts->SampleRate();
        runtime.speakers = std::move(speakers);

        capabilityStorage.push_back(std::move(stored_capability));
        capabilities.push_back(capabilityStorage.back().view());

        const size_t engine_index = engines.size();
        localeToEngine.emplace(runtime.locale, engine_index);
        engines.push_back(std::move(runtime));
        return SUCCESS;
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

    for (const auto& language_json : config["languages"])
    {
        const Result load_result = m_impl->loadLanguage(
            language_json,
            base_path,
            m_impl->numThreads);
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
    m_impl->localeToEngine.clear();
    m_impl->initialized = false;
}

const std::vector<Capability>& Service::enumerateCapabilities() const
{
    return m_impl->capabilities;
}

int32_t Service::sampleRate(std::string_view locale) const
{
    if (!m_impl->initialized)
        return 0;

    const LanguageRuntime* runtime = m_impl->findEngine(locale);
    if (runtime == nullptr)
        return 0;

    return runtime->sampleRate;
}

Result Service::generate(const Input& input, std::vector<float>& out_audio)
{
    if (!m_impl->initialized)
        return ERROR_NOT_INITIALIZED;

    const LanguageRuntime* runtime = m_impl->findEngine(input.locale);
    const Result validation = validateInput(runtime, input);
    if (validation != SUCCESS)
        return validation;

    const SherpaGenerationConfig gen_config = makeGenerationConfig(*runtime, input);

    try
    {
        const auto audio = runtime->tts->Generate(std::string(input.text), gen_config);
        if (audio.samples.empty())
            return ERROR_GENERATION;

        out_audio = audio.samples;
        return SUCCESS;
    }
    catch (const std::exception&)
    {
        return ERROR_GENERATION;
    }
}

std::future<Result> Service::generateAsync(const Input& input, std::vector<float>& out_audio)
{
    return TaskQueue::enqueueAsync([this, input, &out_audio]() {
        return generate(input, out_audio);
    });
}

Result Service::generateStream(const Input& input, GenerateStreamCallback&& callback)
{
    if (!m_impl->initialized)
        return ERROR_NOT_INITIALIZED;

    const LanguageRuntime* runtime = m_impl->findEngine(input.locale);
    const Result validation = validateInput(runtime, input);
    if (validation != SUCCESS)
        return validation;

    const SherpaGenerationConfig gen_config = makeGenerationConfig(*runtime, input);
    StreamContext context;
    context.callback = &callback;

    try
    {
        const auto audio = runtime->tts->Generate(
            std::string(input.text),
            gen_config,
            streamProgressCallback,
            &context);
        if (audio.samples.empty())
            return ERROR_GENERATION;

        return SUCCESS;
    }
    catch (const std::exception&)
    {
        return ERROR_GENERATION;
    }
}

std::future<Result> Service::generateStreamAsync(const Input& input, GenerateStreamCallback&& callback)
{
    return TaskQueue::enqueueAsync([this, input, callback = std::move(callback)]() mutable {
        return generateStream(input, std::move(callback));
    });
}

TTS_NAMESPACE_END
WAVE_NAMESPACE_END
