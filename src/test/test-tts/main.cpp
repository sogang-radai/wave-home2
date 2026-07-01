#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

bool playWithAfplay(const std::filesystem::path& path)
{
#if defined(__APPLE__)
    const std::string command = "afplay \"" + path.string() + "\"";
    std::cout << "Playing: " << path << "\n";
    return std::system(command.c_str()) == 0;
#else
    (void)path;
    std::cerr << "Playback is only implemented with afplay on macOS.\n";
    return false;
#endif
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
        .help("Supertonic flow-matching steps (0 = service default).")
        .defaultValue("0");
    parser.addArgument("--output", "-o")
        .help("Output WAV path. Defaults to a temp file.");
    parser.addArgument("--list")
        .help("List loaded locales and speakers, then exit.")
        .actionFlag();
    parser.addArgument("--no-play")
        .help("Generate WAV only; do not play audio.")
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

    const std::string locale = parser.get<std::string>("locale");
    const std::string text = parser.get<std::string>("text");

    Input input;
    input.locale = locale;
    input.text = text;
    input.speakerID = static_cast<uint32_t>(parser.get<int>("speaker"));
    input.speed = parser.get<float>("speed");
    input.numSteps = parser.get<int32_t>("num-steps");

    std::vector<float> audio;
    const Result generate_result = service.generate(input, audio);
    if (generate_result != Result::SUCCESS)
    {
        std::cerr << "TTS generate failed: " << resultToString(generate_result) << "\n";
        return 1;
    }

    const int32_t sample_rate = service.sampleRate(input.locale);
    if (sample_rate <= 0)
    {
        std::cerr << "Invalid sample rate for locale: " << input.locale << "\n";
        return 1;
    }

    std::filesystem::path output_path;
    if (parser.has("output"))
    {
        output_path = parser.get<std::string>("output");
    }
    else
    {
        output_path = std::filesystem::temp_directory_path() / "wave-test-tts.wav";
    }

    if (!writeWaveFile(output_path, audio, sample_rate))
    {
        std::cerr << "Failed to write WAV: " << output_path << "\n";
        return 1;
    }

    std::cout << "Generated " << audio.size() << " samples at "
              << sample_rate << " Hz\n";
    std::cout << "WAV: " << output_path << "\n";

    if (!parser.has("no-play"))
    {
        if (!playWithAfplay(output_path))
        {
            std::cerr << "Playback failed.\n";
            return 1;
        }
    }

    return 0;
}
