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
#include "device/platform/radai_ws.h"
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
using namespace ws::dev;

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

void appendInt16AsFloat(std::vector<float>& out, const std::vector<int16_t>& samples, float gain)
{
    out.reserve(out.size() + samples.size());
    for (int16_t sample : samples)
    {
        float value = (static_cast<float>(sample) / 32768.0f) * gain;
        if (value > 1.0f)
            value = 1.0f;
        else if (value < -1.0f)
            value = -1.0f;
        out.push_back(value);
    }
}

void appendFloatWithGain(std::vector<float>& out, const float* samples, size_t count, float gain)
{
    out.reserve(out.size() + count);
    for (size_t i = 0; i < count; ++i)
    {
        float value = samples[i] * gain;
        if (value > 1.0f)
            value = 1.0f;
        else if (value < -1.0f)
            value = -1.0f;
        out.push_back(value);
    }
}

json loadDeviceList(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        throw std::runtime_error("failed to open " + path.string());

    json root;
    in >> root;
    return root;
}

json findWaveStationConfig(const json& root)
{
    for (const auto& device : root.at("device_list"))
    {
        if (device.at("class").get<std::string>() == RadaiWs::kClass)
            return device;
    }
    throw std::runtime_error("wave_station device not found in device list");
}

int runMicMode(
    Service& service,
    std::string_view locale,
    int32_t device_index,
    int32_t mic_sample_rate,
    int32_t chunk_ms,
    int32_t duration_sec,
    bool interaction,
    float gain)
{
    if (chunk_ms <= 0)
    {
        std::cerr << "chunk-ms must be greater than 0.\n";
        return 1;
    }

    if (gain <= 0.0f)
    {
        std::cerr << "gain must be greater than 0.\n";
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
        [&shared_audio, gain](const float* samples, size_t count) {
            std::lock_guard<std::mutex> lock(shared_audio.mutex);
            appendFloatWithGain(shared_audio.pending, samples, count, gain);
        },
        use_explicit_device);
    if (!opened)
    {
        service.endRecognizeStream(locale);
        return 1;
    }

    std::cout << "Listening (gain=" << gain << ")";
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

int runWaveStationMode(
    Service& service,
    std::string_view locale,
    const std::filesystem::path& device_list_path,
    const std::string& host_override,
    int32_t chunk_ms,
    int32_t duration_sec,
    bool interaction,
    float gain)
{
    if (chunk_ms <= 0)
    {
        std::cerr << "chunk-ms must be greater than 0.\n";
        return 1;
    }

    if (gain <= 0.0f)
    {
        std::cerr << "gain must be greater than 0.\n";
        return 1;
    }

    if (duration_sec < 0)
    {
        std::cerr << "duration must be 0 or greater.\n";
        return 1;
    }

    json config;
    try
    {
        config = findWaveStationConfig(loadDeviceList(device_list_path));
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    if (!config.contains("room_id"))
        config["room_id"] = "1111111111111111";
    if (!host_override.empty())
        config["interface"]["host"] = host_override;

    RadaiWs station;
    const int init_rc = station.init(config);
    if (init_rc != 0)
    {
        std::cerr << "Wave Station init failed: " << init_rc
                  << " (" << station.getErrorString(init_rc) << ")\n";
        return 1;
    }

    const AudioFormat fmt = station.getSourceFormat();
    if (fmt.sampleRate == 0 || fmt.channels == 0)
    {
        std::cerr << "Wave Station reported invalid audio format.\n";
        station.shutdown();
        return 1;
    }

    const size_t chunk_samples =
        static_cast<size_t>(fmt.sampleRate) * static_cast<size_t>(chunk_ms) / 1000
        * static_cast<size_t>(fmt.channels);
    if (chunk_samples == 0)
    {
        std::cerr << "chunk-ms is too small for the Wave Station sample rate.\n";
        station.shutdown();
        return 1;
    }

    // Keep enough backlog so STT processing does not drop 20ms frames.
    const size_t prev_queue = station.getAudioQueueSize();
    station.setAudioQueueSize(std::max<size_t>(prev_queue, 64));

    {
        AudioFrame discard;
        while (station.popFrame(discard))
        {
        }
    }

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
        station.setAudioQueueSize(prev_queue);
        station.shutdown();
        return 1;
    }

    const auto& iface = station.getInterfaceConfig();
    std::cout << "Wave Station mic: " << iface.host << ":" << iface.port
              << " @ " << fmt.sampleRate << " Hz, " << fmt.channels << " ch"
              << " (gain=" << gain << ")\n";
    std::cout << "Listening";
    if (interaction)
        std::cout << " (interaction mode, Ctrl+C to stop)";
    else if (duration_sec > 0)
        std::cout << " for " << duration_sec << " s";
    else
        std::cout << " (Ctrl+C to stop)";
    std::cout << "...\n";

    const auto started_at = std::chrono::steady_clock::now();
    std::vector<float> pending;
    pending.reserve(chunk_samples * 4);
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

        if (!station.isLinkConnected())
        {
            std::cerr << "Wave Station link lost.\n";
            break;
        }

        AudioFrame frame;
        bool got = false;
        while (station.popFrame(frame))
        {
            got = true;
            if (!frame.samples.empty())
                appendInt16AsFloat(pending, frame.samples, gain);
        }

        while (pending.size() >= chunk_samples)
        {
            chunk.assign(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(chunk_samples));
            pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(chunk_samples));

            AudioInput input;
            input.samples = chunk.data();
            input.sampleCount = chunk.size();
            input.sampleRate = fmt.sampleRate;

            const Result push_result = service.pushAudio(locale, input);
            if (push_result != Result::SUCCESS)
            {
                std::cerr << "pushAudio failed: " << resultToString(push_result) << "\n";
                service.endRecognizeStream(locale);
                station.setAudioQueueSize(prev_queue);
                station.shutdown();
                return 1;
            }
        }

        if (!got)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!pending.empty())
    {
        AudioInput input;
        input.samples = pending.data();
        input.sampleCount = pending.size();
        input.sampleRate = fmt.sampleRate;
        (void)service.pushAudio(locale, input);
    }

    service.endRecognizeStream(locale);
    station.setAudioQueueSize(prev_queue);
    station.shutdown();

    if (interaction && !output_state.lastPartial.empty())
        std::cout << "\n";
    std::cout << "Wave Station capture finished.\n";
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
    parser.addArgument("--ws")
        .help("Capture audio from Wave Station (device_list.json). Implies live console output.")
        .actionFlag();
    parser.addArgument("--device-list")
        .help("device_list.json path for --ws.")
        .defaultValue("bin/device/device_list.json");
    parser.addArgument("--ws-host")
        .help("Override Wave Station host/IP from --device-list.")
        .defaultValue("");
    parser.addArgument("--interaction", "-i")
        .help("Continuous streaming with live partial text (default for --ws).")
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
        .help("Capture duration in seconds for --mic/--ws. 0 runs until interrupted.")
        .defaultValue("0");
    parser.addArgument("--gain", "-g")
        .help("Microphone input gain for --mic/--ws (e.g. 2.0 amplifies). Clamped to [-1, 1].")
        .defaultValue("1.0");
    parser.addArgument("--chunk-ms")
        .help("Chunk duration in milliseconds for --stream, --mic, and --ws modes.")
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

    if (parser.has("ws") && parser.has("stream"))
    {
        std::cerr << "--ws and --stream cannot be used together.\n";
        return 1;
    }

    if (parser.has("ws") && parser.has("mic"))
    {
        std::cerr << "--ws and --mic cannot be used together.\n";
        return 1;
    }

    if (parser.has("interaction") && parser.has("stream"))
    {
        std::cerr << "--interaction and --stream cannot be used together.\n";
        return 1;
    }

    const bool ws_mode = parser.has("ws");
    // Wave Station mode always prints live partials; --interaction only matters for local mic.
    const bool interaction_mode = parser.has("interaction") || ws_mode;
    const bool mic_mode = parser.has("mic") || (parser.has("interaction") && !ws_mode);

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

    if (ws_mode)
    {
        const int32_t chunk_ms = parser.get<int32_t>("chunk-ms");
        // --ws keeps live console output; --duration still limits the session when set.
        const int32_t duration_sec = parser.get<int32_t>("duration");
        const float gain = parser.get<float>("gain");
        const std::filesystem::path device_list = parser.get<std::string>("device-list");
        const std::string ws_host = parser.get<std::string>("ws-host");

        std::cout << "Mode: wave-station (live)\n";
        if (static_cast<int32_t>(capability->sampleRate) != 16000)
        {
            std::cout << "Note: engine rate is " << capability->sampleRate
                      << " Hz; Wave Station typically streams 16000 Hz.\n";
        }

        return runWaveStationMode(
            service,
            locale,
            device_list,
            ws_host,
            chunk_ms,
            duration_sec,
            /*interaction=*/true,
            gain);
    }

    if (mic_mode)
    {
        const int32_t mic_device = parser.get<int32_t>("mic-device");
        const int32_t mic_rate = parser.get<int32_t>("mic-rate");
        const int32_t chunk_ms = parser.get<int32_t>("chunk-ms");
        const int32_t duration_sec = interaction_mode ? 0 : parser.get<int32_t>("duration");
        const float gain = parser.get<float>("gain");

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
            interaction_mode,
            gain);
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
