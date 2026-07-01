#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <util/arg_parser.h>
#include <util/exe_path.h>

#include <sherpa-onnx/c-api/cxx-api.h>

#include "core/json.h"
#include "core/task_queue.h"
#include "service/tts_service.h"

namespace
{
using ws::json;
using ws::tts::Capability;
using ws::tts::Input;
using ws::tts::Result;
using ws::tts::Service;

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
    case Result::ERROR_INVALID_SPEAKER:
        return "ERROR_INVALID_SPEAKER";
    case Result::ERROR_INVALID_INPUT:
        return "ERROR_INVALID_INPUT";
    case Result::ERROR_MODEL_LOAD:
        return "ERROR_MODEL_LOAD";
    case Result::ERROR_GENERATION:
        return "ERROR_GENERATION";
    }
    return "UNKNOWN";
}

std::filesystem::path defaultBaseDir()
{
    const std::filesystem::path exe_dir = getExecutableDir();
    if (std::filesystem::is_regular_file(exe_dir / "models/tts/tts.json"))
        return exe_dir;

    const std::filesystem::path parent = exe_dir.parent_path();
    if (std::filesystem::is_regular_file(parent / "models/tts/tts.json"))
        return parent;

    return std::filesystem::current_path();
}

std::filesystem::path defaultOutputDir()
{
    return std::filesystem::temp_directory_path();
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
                  << capability.country << "\n";
        for (const auto& speaker : capability.speakers)
        {
            std::cout << "  sid=" << speaker.speakerID
                      << " name=" << speaker.name
                      << " gender=" << speaker.gender << "\n";
        }
    }
}

bool writeWaveFile(
    const std::filesystem::path& path,
    const std::vector<float>& samples,
    int32_t sample_rate)
{
    sherpa_onnx::cxx::Wave wave;
    wave.samples = samples;
    wave.sample_rate = sample_rate;
    return sherpa_onnx::cxx::WriteWave(path.string(), wave);
}

std::string shellQuote(const std::filesystem::path& path)
{
    const std::string raw = path.string();
    std::string quoted;
    quoted.reserve(raw.size() + 2);
    quoted.push_back('"');
    for (char ch : raw)
    {
        if (ch == '"')
            quoted += "\\\"";
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

std::string escapeForPowerShellSingleQuoted(std::string_view path)
{
    std::string escaped;
    escaped.reserve(path.size());
    for (char ch : path)
    {
        if (ch == '\'')
            escaped += "''";
        else
            escaped += ch;
    }
    return escaped;
}

bool runShellCommand(const std::string& command)
{
    return std::system(command.c_str()) == 0;
}

std::optional<std::string> detectUsbAlsaPlaybackDevice()
{
#if defined(__linux__)
    std::ifstream cards("/proc/asound/cards");
    if (!cards.is_open())
        return std::nullopt;

    std::string line;
    while (std::getline(cards, line))
    {
        if (line.find("USB-Audio") == std::string::npos
            && line.find("USB Audio") == std::string::npos)
        {
            continue;
        }

        const auto lb = line.find('[');
        const auto rb = line.find(']');
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1)
            continue;

        std::string card = line.substr(lb + 1, rb - lb - 1);
        while (!card.empty() && card.back() == ' ')
            card.pop_back();
        if (card.empty())
            continue;

        return "plughw:CARD=" + card + ",DEV=0";
    }
#endif
    return std::nullopt;
}

bool playAudioFile(const std::filesystem::path& path, const std::string& alsa_device = {})
{
    const std::string quoted = shellQuote(path);

#if defined(__APPLE__)
    if (runShellCommand("afplay " + quoted))
        return true;
#elif defined(_WIN32)
    const std::string ps_path = escapeForPowerShellSingleQuoted(path.string());
    const std::string command =
        "powershell -NoProfile -Command \"(New-Object System.Media.SoundPlayer('"
        + ps_path + "')).PlaySync()\"";
    if (runShellCommand(command))
        return true;
#elif defined(__linux__)
    std::string device = alsa_device;
    if (device.empty())
    {
        if (const auto detected = detectUsbAlsaPlaybackDevice())
            device = *detected;
    }

    const std::string device_arg = device.empty() ? "" : (" -D " + device);
    if (runShellCommand("aplay -q" + device_arg + " " + quoted))
        return true;
    if (device_arg.empty() && runShellCommand("paplay " + quoted))
        return true;
#endif

    if (runShellCommand("ffplay -nodisp -autoexit -loglevel quiet " + quoted))
        return true;

    std::cerr << "No audio player found.";
#if defined(__APPLE__)
    std::cerr << " Install ffmpeg (ffplay) if afplay is unavailable.";
#elif defined(_WIN32)
    std::cerr << " Install ffmpeg (ffplay) or ensure PowerShell is available.";
#elif defined(__linux__)
    std::cerr << " Install alsa-utils (aplay), pulseaudio-utils (paplay), or ffmpeg (ffplay).";
    std::cerr << " Note: the command is aplay, but the Debian package is alsa-utils.";
#endif
    std::cerr << "\n";
    return false;
}

bool ensureParentDir(const std::filesystem::path& file_path)
{
    const auto parent = file_path.parent_path();
    if (parent.empty())
        return true;

    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    return !ec;
}

struct SessionInput
{
    std::string locale;
    std::string text;
    uint32_t speakerID = 0;
    float speed = 1.0f;
    int32_t numSteps = 0;

    static SessionInput fromParser(const ArgParser& parser)
    {
        SessionInput session;
        session.locale = parser.get<std::string>("locale");
        session.speakerID = static_cast<uint32_t>(parser.get<int>("speaker"));
        session.speed = parser.get<float>("speed");
        session.numSteps = parser.get<int32_t>("num-steps");
        return session;
    }

    Input view() const
    {
        Input input;
        input.locale = locale;
        input.text = text;
        input.speakerID = speakerID;
        input.speed = speed;
        input.numSteps = numSteps;
        return input;
    }
};

std::filesystem::path resolveOutputPath(const ArgParser& parser)
{
    if (parser.has("output"))
        return parser.get<std::string>("output");

    const std::filesystem::path output_dir = parser.get<std::string>("output-dir");
    return output_dir / "wave-test-tts.wav";
}

bool synthesizeAndOutput(
    Service& service,
    const SessionInput& session,
    const std::filesystem::path& output_path,
    bool play,
    const std::string& alsa_device)
{
    if (session.text.empty())
        return true;

    const Input input = session.view();

    std::vector<float> audio;
    const Result generate_result = service.generate(input, audio);
    if (generate_result != Result::SUCCESS)
    {
        std::cerr << "TTS generate failed: " << resultToString(generate_result) << "\n";
        return false;
    }

    const int32_t sample_rate = service.sampleRate(session.locale);
    if (sample_rate <= 0)
    {
        std::cerr << "Invalid sample rate for locale: " << session.locale << "\n";
        return false;
    }

    if (!ensureParentDir(output_path))
    {
        std::cerr << "Failed to create output directory: " << output_path.parent_path() << "\n";
        return false;
    }

    if (!writeWaveFile(output_path, audio, sample_rate))
    {
        std::cerr << "Failed to write WAV: " << output_path << "\n";
        return false;
    }

    std::cout << "Generated " << audio.size() << " samples at "
              << sample_rate << " Hz\n";

    if (!play)
        return true;

    if (!alsa_device.empty())
        std::cout << "Audio device: " << alsa_device << "\n";
    else if (const auto detected = detectUsbAlsaPlaybackDevice())
        std::cout << "Audio device: " << *detected << " (auto)\n";

    if (!playAudioFile(output_path, alsa_device))
    {
        std::cerr << "Playback failed.\n";
        return false;
    }

    return true;
}

int runInteractiveLoop(
    Service& service,
    const ArgParser& parser,
    const std::filesystem::path& output_path)
{
    SessionInput session = SessionInput::fromParser(parser);
    const std::string alsa_device = parser.has("audio-device")
        ? parser.get<std::string>("audio-device")
        : std::string{};

    std::cout << "Interactive TTS (Ctrl+C to exit).\n";

    std::string line;
    while (true)
    {
        std::cout << ">> " << std::flush;
        if (!std::getline(std::cin, line))
            break;

        if (line.empty())
            continue;

        session.text = line;
        if (!synthesizeAndOutput(service, session, output_path, true, alsa_device))
            return 1;
    }

    std::cout << "\n";
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

    ArgParser parser("test-tts", "Offline TTS test client using ws::tts::Service.");
    parser.addArgument("--base-dir", "-b")
        .help("Base directory containing models/tts/.")
        .defaultValue(defaultBaseDir().string());
    parser.addArgument("--config", "-c")
        .help("TTS config JSON path relative to --base-dir.")
        .defaultValue("models/tts/tts.json");
    parser.addArgument("--locale", "-l")
        .help("Locale to synthesize (e.g. ko-KR).")
        .defaultValue("ko-KR");
    parser.addArgument("--speaker", "-s")
        .help("Speaker ID (sid).")
        .defaultValue("0");
    parser.addArgument("--text", "-t")
        .help("Text to synthesize.")
        .defaultValue("안녕하세요. Wave Home TTS 테스트입니다.");
    parser.addArgument("--speed")
        .help("Speech speed.")
        .defaultValue("1.0");
    parser.addArgument("--num-steps")
        .help("Supertonic flow-matching steps (0 = service default, 6).")
        .defaultValue("0");
    parser.addArgument("--output-dir")
        .help("Directory for the generated WAV when --output is not set.")
        .defaultValue(defaultOutputDir().string());
    parser.addArgument("--output", "-o")
        .help("Output WAV file path (overrides --output-dir).");
    parser.addArgument("--audio-device")
        .help("ALSA playback device for --play (e.g. plughw:CARD=K17,DEV=0). "
               "Defaults to the first USB audio device on Linux.");
    parser.addArgument("--list")
        .help("List loaded locales and speakers, then exit.")
        .actionFlag();
    parser.addArgument("--play")
        .help("Play the generated WAV through the system audio output.")
        .actionFlag();
    parser.addArgument("--interactive", "-i")
        .help("Keep the service loaded; synthesize and play each line typed after >> until Ctrl+C.")
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
        std::cerr << "TTS init failed: " << resultToString(init_result) << "\n";
        return 1;
    }

    if (parser.has("list"))
    {
        printCapabilities(service);
        return 0;
    }

    const std::filesystem::path output_path = resolveOutputPath(parser);

    if (parser.has("interactive"))
        return runInteractiveLoop(service, parser, output_path);

    SessionInput session = SessionInput::fromParser(parser);
    session.text = parser.get<std::string>("text");

    const std::string alsa_device = parser.has("audio-device")
        ? parser.get<std::string>("audio-device")
        : std::string{};
    const bool play = parser.has("play");

    if (!synthesizeAndOutput(service, session, output_path, play, alsa_device))
        return 1;

    std::cout << "WAV: " << output_path << "\n";
    if (play)
        std::cout << "Playing: " << output_path << "\n";

    return 0;
}
