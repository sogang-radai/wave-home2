#include <csignal>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <util/arg_parser.h>
#include <util/exe_path.h>

#include <sherpa-onnx/c-api/cxx-api.h>

#include "core/json.h"
#include "core/task_queue.h"
#include "mic_capture.h"
#include "service/stt_service.h"

namespace
{
using ws::json;
using ws::stt::AudioInput;
using ws::stt::Capability;
using ws::stt::RecognizeResult;
using ws::stt::Result;
using ws::stt::Service;

std::atomic<bool> g_stopMicCapture{false};

void handleStopSignal(int /*signal*/)
{
    g_stopMicCapture.store(true);
}

std::string resultToString(Result result)
{
    switch (result)
    {
    case Result::SUCCESS:
        return "SUCCESS";
    case Result::ERROR_INVALID_CONFIG:
        return "ERROR_INVALID_CONFIG";
    case Result::ERROR_NOT_INITIALIZED:
        return "ERROR_NOT_INITIALIZED";
    case Result::ERROR_INVALID_LOCALE:
        return "ERROR_INVALID_LOCALE";
    case Result::ERROR_INVALID_INPUT:
        return "ERROR_INVALID_INPUT";
    case Result::ERROR_MODEL_LOAD:
        return "ERROR_MODEL_LOAD";
    case Result::ERROR_RECOGNITION:
        return "ERROR_RECOGNITION";
    }
    return "UNKNOWN";
}

std::filesystem::path defaultBaseDir()
{
    const std::filesystem::path exe_dir = getExecutableDir();
    if (std::filesystem::is_regular_file(exe_dir / "models/stt/stt.json"))
        return exe_dir;

    const std::filesystem::path parent = exe_dir.parent_path();
    if (std::filesystem::is_regular_file(parent / "models/stt/stt.json"))
        return parent;

    return std::filesystem::current_path();
}

std::filesystem::path defaultWavPath(const std::filesystem::path& base_dir)
{
    return base_dir / "models/stt/ko-kr/test_wavs/0.wav";
}

json loadJsonFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Failed to open JSON file: " + path.string());

    json value;
    file >> value;
    return value;
}

void printCapabilities(const Service& service)
{
    for (const Capability& capability : service.enumerateCapabilities())
    {
        std::cout << capability.locale << " | "
                  << capability.name << " | "
                  << capability.language << " / "
                  << capability.country << " | "
                  << capability.sampleRate << " Hz | "
                  << capability.modelType
                  << (capability.streaming ? " (streaming)" : "") << "\n";
    }
}

const Capability* findCapability(const Service& service, std::string_view locale)
{
    for (const Capability& capability : service.enumerateCapabilities())
    {
        if (capability.locale == locale)
            return &capability;
    }
    return nullptr;
}

std::optional<sherpa_onnx::cxx::Wave> loadWaveFile(const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path))
    {
        std::cerr << "WAV file not found: " << path << "\n";
        return std::nullopt;
    }

    sherpa_onnx::cxx::Wave wave = sherpa_onnx::cxx::ReadWave(path.string());
    if (wave.samples.empty())
    {
        std::cerr << "Failed to read WAV: " << path << "\n";
        return std::nullopt;
    }

    return wave;
}

void printMicDevices()
{
    MicCapture capture;
    const std::vector<MicDeviceInfo> devices = MicCapture::listInputDevices();
    if (devices.empty())
    {
        std::cout << "No input devices found.\n";
        return;
    }

    std::cout << "Input devices:\n";
    for (const MicDeviceInfo& device : devices)
    {
        std::cout << (device.isDefault ? "* " : "  ")
                  << "[" << device.index << "] "
                  << device.name
                  << " | in-ch=" << device.maxInputChannels
                  << " | default-rate=" << static_cast<int32_t>(device.defaultSampleRate)
                  << " Hz\n";
    }
}

struct StreamOutputState
{
    bool interaction = false;
    std::string lastPartial;
    int32_t segment = 0;
};

void printStreamResult(const StreamOutputState& state, const RecognizeResult& result)
{
    if (state.interaction)
    {
        if (!result.text.empty() && result.text != state.lastPartial)
        {
            std::cout << "\r[live] " << result.text << std::flush;
        }

        if (result.isEndpoint)
        {
            if (!result.text.empty())
            {
                std::cout << "\n[" << state.segment << "] " << result.text << "\n";
            }
            else if (!state.lastPartial.empty())
            {
                std::cout << "\n";
            }
        }
        return;
    }

    std::cout << "[partial";
    if (result.isEndpoint)
        std::cout << " endpoint";
    std::cout << "] " << result.text << "\n";
}

int runMicMode(
    Service& service,
    std::string_view locale,
    int32_t device_index,
    int32_t mic_sample_rate,
    int32_t chunk_ms,
    int32_t duration_sec,
    bool interaction)
{
    if (chunk_ms <= 0)
    {
        std::cerr << "chunk-ms must be greater than 0.\n";
        return 1;
    }

    if (!interaction && duration_sec < 0)
    {
        std::cerr << "duration must be 0 or greater.\n";
        return 1;
    }

    if (interaction)
        duration_sec = 0;

    const size_t chunk_samples =
        static_cast<size_t>(mic_sample_rate) * static_cast<size_t>(chunk_ms) / 1000;
    if (chunk_samples == 0)
    {
        std::cerr << "chunk-ms is too small for the microphone sample rate.\n";
        return 1;
    }

    struct SharedAudio
    {
        std::mutex mutex;
        std::vector<float> pending;
    };

    SharedAudio shared_audio;
    g_stopMicCapture.store(false);
    std::signal(SIGINT, handleStopSignal);

    StreamOutputState output_state;
    output_state.interaction = interaction;

    auto onPartial = [&output_state](const RecognizeResult& result) {
        printStreamResult(output_state, result);
        if (output_state.interaction && result.isEndpoint)
        {
            ++output_state.segment;
            output_state.lastPartial.clear();
        }
        else if (!result.text.empty())
        {
            output_state.lastPartial = result.text;
        }
    };

    const Result begin_result = service.beginRecognizeStream(locale, onPartial);
    if (begin_result != Result::SUCCESS)
    {
        std::cerr << "beginRecognizeStream failed: " << resultToString(begin_result) << "\n";
        return 1;
    }

    MicCapture mic;
    const bool use_explicit_device = device_index >= 0;
    const bool opened = mic.open(
        device_index,
        mic_sample_rate,
        1,
        [&shared_audio](const float* samples, size_t count) {
            std::lock_guard<std::mutex> lock(shared_audio.mutex);
            shared_audio.pending.insert(shared_audio.pending.end(), samples, samples + count);
        },
        use_explicit_device);
    if (!opened)
    {
        service.endRecognizeStream(locale);
        return 1;
    }

    std::cout << "Listening";
    if (interaction)
        std::cout << " (interaction mode, Ctrl+C to stop)";
    else if (duration_sec > 0)
        std::cout << " for " << duration_sec << " s";
    else
        std::cout << " (Ctrl+C to stop)";
    std::cout << "...\n";

    const auto started_at = std::chrono::steady_clock::now();
    std::vector<float> chunk;
    chunk.reserve(chunk_samples);

    while (!g_stopMicCapture.load())
    {
        if (duration_sec > 0)
        {
            const auto elapsed = std::chrono::steady_clock::now() - started_at;
            if (elapsed >= std::chrono::seconds(duration_sec))
                break;
        }

        chunk.clear();
        {
            std::lock_guard<std::mutex> lock(shared_audio.mutex);
            const size_t take_count = std::min(chunk_samples, shared_audio.pending.size());
            if (take_count > 0)
            {
                chunk.assign(shared_audio.pending.begin(), shared_audio.pending.begin() + take_count);
                shared_audio.pending.erase(shared_audio.pending.begin(), shared_audio.pending.begin() + take_count);
            }
        }

        if (!chunk.empty())
        {
            AudioInput input;
            input.samples = chunk.data();
            input.sampleCount = chunk.size();
            input.sampleRate = static_cast<uint32_t>(mic_sample_rate);

            const Result push_result = service.pushAudio(locale, input);
            if (push_result != Result::SUCCESS)
            {
                std::cerr << "pushAudio failed: " << resultToString(push_result) << "\n";
                mic.close();
                service.endRecognizeStream(locale);
                return 1;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mic.close();
    service.endRecognizeStream(locale);
    if (interaction && !output_state.lastPartial.empty())
        std::cout << "\n";
    std::cout << "Microphone capture finished.\n";
    return 0;
}

int runFileMode(
    Service& service,
    std::string_view locale,
    const sherpa_onnx::cxx::Wave& wave)
{
    AudioInput input;
    input.samples = wave.samples.data();
    input.sampleCount = wave.samples.size();
    input.sampleRate = static_cast<uint32_t>(wave.sample_rate);

    RecognizeResult result;
    const Result recognize_result = service.recognize(locale, input, result);
    if (recognize_result != Result::SUCCESS)
    {
        std::cerr << "STT recognize failed: " << resultToString(recognize_result) << "\n";
        return 1;
    }

    std::cout << "Text: " << result.text << "\n";
    if (result.isEndpoint)
        std::cout << "(endpoint)\n";

    return 0;
}

int runStreamingFileMode(
    Service& service,
    std::string_view locale,
    const sherpa_onnx::cxx::Wave& wave,
    int32_t chunk_ms)
{
    if (chunk_ms <= 0)
    {
        std::cerr << "chunk-ms must be greater than 0.\n";
        return 1;
    }

    const size_t chunk_samples = static_cast<size_t>(wave.sample_rate) * static_cast<size_t>(chunk_ms) / 1000;
    if (chunk_samples == 0)
    {
        std::cerr << "chunk-ms is too small for the WAV sample rate.\n";
        return 1;
    }

    auto onPartial = [](const RecognizeResult& result) {
        std::cout << "[partial";
        if (result.isEndpoint)
            std::cout << " endpoint";
        std::cout << "] " << result.text << "\n";
    };

    const Result begin_result = service.beginRecognizeStream(locale, onPartial);
    if (begin_result != Result::SUCCESS)
    {
        std::cerr << "beginRecognizeStream failed: " << resultToString(begin_result) << "\n";
        return 1;
    }

    size_t offset = 0;
    while (offset < wave.samples.size())
    {
        const size_t count = std::min(chunk_samples, wave.samples.size() - offset);

        AudioInput chunk;
        chunk.samples = wave.samples.data() + offset;
        chunk.sampleCount = count;
        chunk.sampleRate = static_cast<uint32_t>(wave.sample_rate);

        const Result push_result = service.pushAudio(locale, chunk);
        if (push_result != Result::SUCCESS)
        {
            std::cerr << "pushAudio failed: " << resultToString(push_result) << "\n";
            service.endRecognizeStream(locale);
            return 1;
        }

        offset += count;
    }

    service.endRecognizeStream(locale);
    std::cout << "Streaming file simulation finished.\n";
    return 0;
}
}  // namespace

int main(int argc, const char* argv[])
{
    class TaskQueueHolder
    {
    public:
        TaskQueueHolder()
        {
            m_queue.init();
        }

    private:
        ws::TaskQueue m_queue;
    };

    TaskQueueHolder task_queue_holder;

    const std::filesystem::path default_base_dir = defaultBaseDir();

    ArgParser parser("test-stt", "STT test client using ws::stt::Service.");
    parser.addArgument("--base-dir", "-b")
        .help("Base directory containing models/stt/.")
        .defaultValue(default_base_dir.string());
    parser.addArgument("--config", "-c")
        .help("STT config JSON path relative to --base-dir.")
        .defaultValue("models/stt/stt.json");
    parser.addArgument("--locale", "-l")
        .help("Locale to recognize (e.g. ko-KR).")
        .defaultValue("ko-KR");
    parser.addArgument("--file", "-f")
        .help("Input WAV file path. Defaults to bundled ko-kr test_wavs/0.wav.")
        .defaultValue(defaultWavPath(default_base_dir).string());
    parser.addArgument("--stream")
        .help("Simulate streaming recognition by feeding the WAV in chunks.")
        .actionFlag();
    parser.addArgument("--mic")
        .help("Capture audio from the default or selected microphone.")
        .actionFlag();
    parser.addArgument("--interaction", "-i")
        .help("Continuous microphone interaction: keep streaming and print recognized text.")
        .actionFlag();
    parser.addArgument("--mic-list")
        .help("List CoreAudio input devices and exit.")
        .actionFlag();
    parser.addArgument("--mic-device")
        .help("Input device index for --mic (from --mic-list). -1 uses the default device.")
        .defaultValue("-1");
    parser.addArgument("--mic-rate")
        .help("Microphone capture sample rate in Hz for --mic.")
        .defaultValue("16000");
    parser.addArgument("--duration")
        .help("Capture duration in seconds for --mic. 0 runs until interrupted.")
        .defaultValue("0");
    parser.addArgument("--chunk-ms")
        .help("Chunk duration in milliseconds for --stream and --mic modes.")
        .defaultValue("100");
    parser.addArgument("--list")
        .help("List loaded locales and models, then exit.")
        .actionFlag();

    try
    {
        parser.parseArgs(argc, argv);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    if (parser.has("mic-list"))
    {
        printMicDevices();
        return 0;
    }

    if (parser.has("mic") && parser.has("stream"))
    {
        std::cerr << "--mic and --stream cannot be used together.\n";
        return 1;
    }

    if (parser.has("interaction") && parser.has("stream"))
    {
        std::cerr << "--interaction and --stream cannot be used together.\n";
        return 1;
    }

    const bool interaction_mode = parser.has("interaction");
    const bool mic_mode = parser.has("mic") || interaction_mode;

    const std::filesystem::path base_dir = parser.get<std::string>("base-dir");
    const std::filesystem::path config_path = base_dir / parser.get<std::string>("config");
    if (!std::filesystem::is_regular_file(config_path))
    {
        std::cerr << "Config not found: " << config_path << "\n";
        return 1;
    }

    json config;
    try
    {
        config = loadJsonFile(config_path);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    Service service;
    const Result init_result = service.init(base_dir.string(), config);
    if (init_result != Result::SUCCESS)
    {
        std::cerr << "STT init failed: " << resultToString(init_result) << "\n";
        return 1;
    }

    if (parser.has("list"))
    {
        printCapabilities(service);
        return 0;
    }

    const std::string locale = parser.get<std::string>("locale");
    const Capability* capability = findCapability(service, locale);
    if (capability == nullptr)
    {
        std::cerr << "Locale not loaded: " << locale << "\n";
        return 1;
    }

    if (mic_mode)
    {
        const int32_t mic_device = parser.get<int32_t>("mic-device");
        const int32_t mic_rate = parser.get<int32_t>("mic-rate");
        const int32_t chunk_ms = parser.get<int32_t>("chunk-ms");
        const int32_t duration_sec = interaction_mode ? 0 : parser.get<int32_t>("duration");

        std::cout << "Mode: " << (interaction_mode ? "interaction" : "microphone") << "\n";
        if (mic_rate != static_cast<int32_t>(capability->sampleRate))
        {
            std::cout << "Note: mic rate (" << mic_rate
                      << " Hz) differs from engine rate ("
                      << capability->sampleRate << " Hz); using AudioInput.sampleRate.\n";
        }

        return runMicMode(
            service,
            locale,
            mic_device,
            mic_rate,
            chunk_ms,
            duration_sec,
            interaction_mode);
    }

    const std::filesystem::path wav_path = parser.get<std::string>("file");
    const auto wave = loadWaveFile(wav_path);
    if (!wave.has_value())
        return 1;

    std::cout << "WAV: " << wav_path << " (" << wave->samples.size()
              << " samples, " << wave->sample_rate << " Hz)\n";
    if (wave->sample_rate != static_cast<int32_t>(capability->sampleRate))
    {
        std::cout << "Note: input rate (" << wave->sample_rate
                  << " Hz) differs from engine rate ("
                  << capability->sampleRate << " Hz); using AudioInput.sampleRate.\n";
    }
    std::cout << "Mode: " << (parser.has("stream") ? "streaming (file simulation)" : "file") << "\n";

    if (parser.has("stream"))
    {
        const int32_t chunk_ms = parser.get<int32_t>("chunk-ms");
        return runStreamingFileMode(service, locale, *wave, chunk_ms);
    }

    return runFileMode(service, locale, *wave);
}
