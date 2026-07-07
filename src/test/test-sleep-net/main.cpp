#include <memory>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"
#include "device/platform/srs_r4sn.h"
#include "nn/sleep_pipeline.h"
#include "util/arg_parser.h"

using ws::json;
using namespace ws::dev;
using ws::nn::SleepPipeline;
using ws::nn::SleepResult;
using ws::nn::FrameEncoderWindowMode;

namespace
{
    volatile std::sig_atomic_t g_running = 1;

    void onSignal(int)
    {
        g_running = 0;
    }

    std::vector<json> loadRadarDevices(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open())
            throw std::runtime_error("failed to open " + path);

        json root;
        in >> root;

        std::vector<json> radars;
        for (const auto& device : root.at("device_list"))
        {
            if (device.value("class", std::string()) == "srs_r4sn")
                radars.push_back(device);
        }

        return radars;
    }

    void printRadarList(const std::vector<json>& radars)
    {
        std::cout << "srs_r4sn devices (" << radars.size() << "):\n";
        for (size_t i = 0; i < radars.size(); ++i)
        {
            const json& device = radars[i];
            const json interface = device.value("interface", json::object());
            std::cout << "  [" << i << "] "
                      << device.value("name", std::string("(no name)"))
                      << "  id=" << device.value("id", std::string("?"))
                      << "  host=" << interface.value("host", std::string("?"))
                      << '\n';
        }
    }

    std::string formatScores(const std::vector<std::string>& labels, const std::vector<float>& scores)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2);
        for (size_t i = 0; i < scores.size(); ++i)
        {
            if (i > 0)
                os << ' ';

            if (i < labels.size())
                os << labels[i] << ' ';
            os << scores[i];
        }
        return os.str();
    }

    std::string labelOf(const std::vector<std::string>& labels, int32_t index)
    {
        if (index < 0 || static_cast<size_t>(index) >= labels.size())
            return "?";
        return labels[static_cast<size_t>(index)];
    }

    std::vector<std::filesystem::path> discoverSleepModelDirs(const std::filesystem::path& parent)
    {
        if (!std::filesystem::is_directory(parent))
            throw std::runtime_error("model-dir is not a directory: " + parent.string());

        std::vector<std::filesystem::path> dirs;
        for (const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator(parent))
        {
            if (!entry.is_directory())
                continue;

            const std::string name = entry.path().filename().string();
            if (name.size() < 5 || name.compare(0, 5, "sleep") != 0)
                continue;

            if (!std::filesystem::is_regular_file(entry.path() / "model.json"))
                continue;

            dirs.push_back(entry.path());
        }

        std::sort(dirs.begin(), dirs.end());
        if (dirs.empty())
        {
            throw std::runtime_error(
                "no sleep*/model.json subfolders found under " + parent.string());
        }

        return dirs;
    }

    json loadModelConfig(const std::filesystem::path& model_dir)
    {
        const std::filesystem::path model_json = model_dir / "model.json";
        std::ifstream in(model_json);
        if (!in.is_open())
            throw std::runtime_error("failed to open " + model_json.string());

        json config;
        in >> config;
        return config;
    }

    void nowDateTime(std::string& out_date, std::string& out_time)
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm {};
        localtime_r(&t, &tm);

        char date_buf[16];
        char time_buf[16];
        std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);
        std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);
        out_date = date_buf;
        out_time = time_buf;
    }

    // logFrames 프레임을 한 행으로 집계한 결과.
    struct AggregatedLog
    {
        std::string date;
        std::string time;
        uint64_t frameBegin = 0;
        uint64_t frameEnd = 0;
        size_t sampleCount = 0;

        bool connected = true;
        bool frameGap = false;
        bool warmup = false;

        // 상태: 윈도우에서 가장 오래 유지된 클래스 + 해당 클래스 프레임들의 평균 확률.
        int32_t statusClass = -1;
        std::vector<float> statusScores;

        // 뒤척임: 윈도우 내 tossIndex 최대인 피크 프레임 기준.
        bool tossValid = false;
        int32_t tossClass = -1;
        std::vector<float> tossScores;
        float tossIndex = 0.0f;
    };

    struct TickLog
    {
        SleepResult result;
        bool haveResult = false;
        bool connected = false;
        bool frameGap = false;
    };

    struct PushResult
    {
        size_t pushed = 0;
        bool frameGap = false;
    };

    struct ModelSlot
    {
        std::string tag;
        std::filesystem::path dir;
        SleepPipeline pipeline;
        std::ofstream csv;
        std::vector<TickLog> logWindow;
    };

    // 프레임 단위 결과 윈도우를 하나의 로그 행으로 축약한다.
    // 상태는 최다 프레임(최장 유지) 클래스, 뒤척임은 최대 tossIndex 프레임을 채택.
    AggregatedLog aggregateWindow(const std::vector<TickLog>& window)
    {
        AggregatedLog log;
        nowDateTime(log.date, log.time);
        log.sampleCount = window.size();
        if (window.empty())
            return log;

        log.connected = true;
        log.frameGap = false;
        log.warmup = false;

        std::vector<SleepResult> inferenceRows;
        inferenceRows.reserve(window.size());

        for (const TickLog& tick : window)
        {
            log.connected = log.connected && tick.connected;
            log.frameGap = log.frameGap || tick.frameGap;
            if (!tick.haveResult)
                log.warmup = true;

            if (tick.haveResult)
                inferenceRows.push_back(tick.result);
        }

        if (inferenceRows.empty())
            return log;

        log.frameBegin = inferenceRows.front().frameIndex;
        log.frameEnd = inferenceRows.back().frameIndex;

        std::vector<int32_t> classCounts;
        for (const SleepResult& r : inferenceRows)
        {
            if (r.statusClass < 0)
                continue;
            if (static_cast<size_t>(r.statusClass) >= classCounts.size())
                classCounts.resize(static_cast<size_t>(r.statusClass) + 1, 0);
            ++classCounts[static_cast<size_t>(r.statusClass)];
        }

        for (size_t c = 0; c < classCounts.size(); ++c)
        {
            if (log.statusClass < 0 || classCounts[c] > classCounts[static_cast<size_t>(log.statusClass)])
                log.statusClass = static_cast<int32_t>(c);
        }

        size_t winningFrames = 0;
        for (const SleepResult& r : inferenceRows)
        {
            if (r.statusClass != log.statusClass)
                continue;

            if (log.statusScores.size() < r.statusScores.size())
                log.statusScores.resize(r.statusScores.size(), 0.0f);
            for (size_t i = 0; i < r.statusScores.size(); ++i)
                log.statusScores[i] += r.statusScores[i];
            ++winningFrames;
        }
        if (winningFrames > 0)
        {
            for (float& v : log.statusScores)
                v /= static_cast<float>(winningFrames);
        }

        for (const SleepResult& r : inferenceRows)
        {
            if (!r.tossValid)
                continue;
            if (!log.tossValid || r.tossIndex > log.tossIndex)
            {
                log.tossValid = true;
                log.tossIndex = r.tossIndex;
                log.tossClass = r.tossClass;
                log.tossScores = r.tossScores;
            }
        }

        return log;
    }

    void writeCsvHeader(
        std::ostream& out,
        const std::vector<std::string>& bed_labels,
        const std::vector<std::string>& toss_labels)
    {
        out << "date,time,frame_begin,frame_end,samples,connected,frame_gap,warmup,status_label";
        for (const std::string& label : bed_labels)
            out << ",status_" << label;
        out << ",toss_valid,toss_label,toss_index";
        for (const std::string& label : toss_labels)
            out << ",toss_" << label;
        out << '\n';
    }

    void writeCsvRow(
        std::ostream& out,
        const AggregatedLog& log,
        const std::vector<std::string>& bed_labels,
        const std::vector<std::string>& toss_labels)
    {
        out << log.date << ',' << log.time << ','
            << log.frameBegin << ',' << log.frameEnd << ',' << log.sampleCount << ','
            << (log.connected ? 1 : 0) << ','
            << (log.frameGap ? 1 : 0) << ','
            << (log.warmup ? 1 : 0) << ','
            << labelOf(bed_labels, log.statusClass);

        out << std::fixed << std::setprecision(4);
        for (size_t i = 0; i < bed_labels.size(); ++i)
        {
            out << ',';
            if (i < log.statusScores.size())
                out << log.statusScores[i];
        }

        out << ',' << (log.tossValid ? 1 : 0) << ',';
        if (log.tossValid)
            out << labelOf(toss_labels, log.tossClass) << ',' << log.tossIndex;
        else
            out << ',';

        for (size_t i = 0; i < toss_labels.size(); ++i)
        {
            out << ',';
            if (log.tossValid && i < log.tossScores.size())
                out << log.tossScores[i];
        }
        out << '\n';
        out.flush();
    }

    // 레이더에서 새 프레임을 한 번만 읽어 모든 파이프라인에 동일하게 push.
    PushResult pushNewFrames(
        SRSR4SN& radar,
        std::vector<std::unique_ptr<ModelSlot>>& models,
        bool& has_last,
        uint64_t& last_index,
        size_t& out_last_point_count)
    {
        PushResult out;
        std::vector<uint64_t> indices;
        radar.enumeratePointCloudFrameIndices(indices);
        if (indices.empty())
            return out;

        std::sort(indices.begin(), indices.end());

        for (const uint64_t idx : indices)
        {
            if (has_last && idx <= last_index)
                continue;

            if (has_last && idx > last_index + 1)
                out.frameGap = true;

            RadarPointCloud frame;
            if (!radar.getPointCloudFrame(idx, frame))
                continue;

            out_last_point_count = frame.points.size();

            for (size_t i = 0; i < models.size(); ++i)
            {
                if (i + 1 == models.size())
                    models[i]->pipeline.pushFrame(std::move(frame));
                else
                    models[i]->pipeline.pushFrame(frame);
            }

            has_last = true;
            last_index = idx;
            ++out.pushed;
        }

        if (out.pushed > 0)
            radar.releasePointCloudFramesUpTo(last_index);

        return out;
    }

    void flushCsvWindows(std::vector<std::unique_ptr<ModelSlot>>& models, size_t log_frames, bool force_all)
    {
        for (auto& slot : models)
        {
            if (!slot->csv.is_open() || slot->logWindow.empty())
                continue;

            if (!force_all && slot->logWindow.size() < log_frames)
                continue;

            const AggregatedLog aggregated = aggregateWindow(slot->logWindow);
            writeCsvRow(
                slot->csv,
                aggregated,
                slot->pipeline.getBedLabels(),
                slot->pipeline.getTossLabels());
            slot->logWindow.clear();
        }
    }

    void printStatusBlock(const std::string& block, size_t line_count, bool& first_paint)
    {
        if (!first_paint)
            std::cout << "\033[" << line_count << "F";

        std::istringstream lines(block);
        std::string line;
        while (std::getline(lines, line))
            std::cout << "\033[2K\r" << line << '\n';

        std::cout << std::flush;
        first_paint = false;
    }
}

int main(int argc, const char* argv[])
{
    ArgParser parser("test-sleep-net", "Real-time SleepNet (bed/toss) inference test on SRS R4SN radar.");
    parser.addArgument("--config", "-c")
        .help("device_list.json path.")
        .defaultValue("bin/device/device_list.json");
    parser.addArgument("--model-dir", "-m")
        .help("parent directory; loads every sleep*/model.json subfolder.")
        .defaultValue("bin/models");
    parser.addArgument("--csv-dir")
        .help("directory for per-model CSV logs ({subfolder}.csv).")
        .defaultValue(".");
    parser.addArgument("--fps", "-f")
        .help("output/poll rate in Hz.")
        .defaultValue("20");
    parser.addArgument("--index", "-i")
        .help("srs_r4sn device index to use.")
        .defaultValue("0");
    parser.addArgument("--list", "-l")
        .help("list srs_r4sn devices and exit.")
        .actionFlag();
    parser.addArgument("--log-frames", "-n")
        .help("frames aggregated per CSV row (0 = ~1 second based on fps).")
        .defaultValue("0");
    parser.addArgument("--consecutive")
        .help("require consecutive frame indices for temporal windows (default: relaxed).")
        .actionFlag();

    try
    {
        parser.parseArgs(argc, argv);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    const std::string configPath = parser.get<std::string>("config");
    const std::filesystem::path modelsParent(parser.get<std::string>("model-dir"));
    const std::filesystem::path csvDir(parser.get<std::string>("csv-dir"));
    const double fps = parser.get<double>("fps");
    const int32_t radarIndex = parser.get<int32_t>("index");
    const bool listOnly = parser.has("list");
    const bool consecutiveWindows = parser.has("consecutive");
    const int32_t logFramesArg = parser.get<int32_t>("log-frames");

    if (fps <= 0.0)
    {
        std::cerr << "error: --fps must be greater than 0\n";
        return 1;
    }

    const size_t logFrames = logFramesArg > 0
        ? static_cast<size_t>(logFramesArg)
        : static_cast<size_t>(std::max<long>(1, std::lround(fps)));

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try
    {
        const std::vector<json> radars = loadRadarDevices(configPath);
        if (radars.empty())
            throw std::runtime_error("no srs_r4sn device found in " + configPath);

        printRadarList(radars);
        if (listOnly)
            return 0;

        if (radarIndex < 0 || static_cast<size_t>(radarIndex) >= radars.size())
            throw std::runtime_error("--index out of range (0.." + std::to_string(radars.size() - 1) + ")");

        const json& radarConfig = radars[static_cast<size_t>(radarIndex)];

        const std::vector<std::filesystem::path> modelDirs = discoverSleepModelDirs(modelsParent);
        std::vector<std::unique_ptr<ModelSlot>> models;

        std::cout << "models under " << modelsParent.string() << " (" << modelDirs.size() << "):\n";

        if (!csvDir.empty() && !std::filesystem::exists(csvDir))
            std::filesystem::create_directories(csvDir);

        for (const std::filesystem::path& dir : modelDirs)
        {
            auto slot = std::make_unique<ModelSlot>();
            slot->tag = dir.filename().string();
            slot->dir = dir;

            const json modelConfig = loadModelConfig(dir);
            std::string error;
            if (!slot->pipeline.init(dir.string(), modelConfig, error))
            {
                throw std::runtime_error(
                    "sleep pipeline init failed for " + dir.string() + ": " + error);
            }

            slot->pipeline.setEncoderWindowMode(
                consecutiveWindows
                    ? FrameEncoderWindowMode::Consecutive
                    : FrameEncoderWindowMode::Relaxed);

            std::cout << "  " << slot->tag
                      << "  name=" << slot->pipeline.getModelName()
                      << "  bed_window=" << slot->pipeline.getBedWindow()
                      << "  toss_window=" << slot->pipeline.getTossWindow()
                      << '\n';

            const std::filesystem::path csvPath = csvDir / (slot->tag + ".csv");
            const bool exists = std::filesystem::exists(csvPath)
                && std::filesystem::file_size(csvPath) > 0;
            slot->csv.open(csvPath, std::ios::app);
            if (!slot->csv.is_open())
                throw std::runtime_error("failed to open csv log " + csvPath.string());
            if (!exists)
            {
                writeCsvHeader(
                    slot->csv,
                    slot->pipeline.getBedLabels(),
                    slot->pipeline.getTossLabels());
            }

            std::cout << "    csv: " << csvPath.string() << '\n';
            models.push_back(std::move(slot));
        }

        std::cout << "csv aggregation: every " << logFrames << " frames\n";
        std::cout << "encoder window: "
                  << (consecutiveWindows ? "consecutive" : "relaxed") << '\n';

        SRSR4SN radar;
        const int initCode = radar.init(radarConfig);
        std::cout << "radar init => " << initCode << " (" << radar.getErrorString(initCode) << ")\n";
        if (initCode != 0)
            return 1;

        size_t maxBedWindow = 0;
        for (const auto& slot : models)
            maxBedWindow = std::max(maxBedWindow, static_cast<size_t>(slot->pipeline.getBedWindow()));

        const size_t radarQueueSize = std::max<size_t>(maxBedWindow * 8, 2048);
        radar.setPointCloudQueueSize(radarQueueSize);
        std::cout << "radar queue size: " << radarQueueSize << '\n';

        std::cout << "target " << fps << " Hz  (radar [" << radarIndex << "] "
                  << radarConfig.value("name", std::string("?")) << ")\n";
        std::cout << "Ctrl+C to stop\n";

        const auto period = std::chrono::duration<double>(1.0 / fps);

        bool hasLast = false;
        uint64_t lastIndex = 0;
        size_t lastPointCount = 0;

        auto lastTick = std::chrono::steady_clock::now()
            - std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);

        const bool multiModel = models.size() > 1;
        bool firstPaint = true;

        while (g_running)
        {
            const auto loopStart = std::chrono::steady_clock::now();

            const PushResult push = pushNewFrames(radar, models, hasLast, lastIndex, lastPointCount);
            const bool connected = radar.isPointCloudConnected();

            const auto inferStart = std::chrono::steady_clock::now();
            std::vector<SleepResult> results(models.size());
            std::vector<bool> haveResults(models.size(), false);
            for (size_t i = 0; i < models.size(); ++i)
                haveResults[i] = models[i]->pipeline.evaluate(results[i]);
            const auto inferEnd = std::chrono::steady_clock::now();

            const double inferMs =
                std::chrono::duration<double, std::milli>(inferEnd - inferStart).count();
            const double loopHz =
                1.0 / std::max(1e-6, std::chrono::duration<double>(loopStart - lastTick).count());
            lastTick = loopStart;

            for (size_t i = 0; i < models.size(); ++i)
            {
                TickLog tick;
                tick.haveResult = haveResults[i];
                tick.connected = connected;
                tick.frameGap = push.frameGap;
                if (haveResults[i])
                    tick.result = results[i];

                models[i]->logWindow.push_back(tick);
            }
            flushCsvWindows(models, logFrames, false);

            std::ostringstream block;
            block << std::fixed << std::setprecision(1);
            block << "frame " << std::setw(6) << std::setfill('0')
                  << (hasLast ? lastIndex : 0) << std::setfill(' ')
                  << " | " << std::setw(5) << loopHz << "Hz"
                  << " | new " << push.pushed
                  << " | pts " << std::setw(3) << lastPointCount
                  << " | conn " << (connected ? 1 : 0)
                  << " | infer " << std::setprecision(2) << inferMs << "ms";

            for (size_t i = 0; i < models.size(); ++i)
            {
                const ModelSlot& slot = *models[i];
                const auto& bedLabels = slot.pipeline.getBedLabels();
                const auto& tossLabels = slot.pipeline.getTossLabels();

                block << '\n' << "  " << slot.tag << " | ";
                if (!haveResults[i])
                {
                    block << "warmup " << slot.pipeline.getFrameCount()
                          << '/' << slot.pipeline.getBedWindow();
                }
                else
                {
                    const SleepResult& result = results[i];
                    block << "STATUS " << labelOf(bedLabels, result.statusClass)
                          << " [" << formatScores(bedLabels, result.statusScores) << "]";

                    if (result.tossValid)
                    {
                        block << " | TOSS " << labelOf(tossLabels, result.tossClass)
                              << " idx " << std::setprecision(2) << result.tossIndex
                              << " [" << formatScores(tossLabels, result.tossScores) << "]";
                    }
                    else
                    {
                        block << " | TOSS -";
                    }
                }
            }

            if (multiModel)
            {
                printStatusBlock(block.str(), 1 + models.size(), firstPaint);
            }
            else
            {
                std::string text = block.str();
                static size_t prevLineLength = 0;
                if (text.size() < prevLineLength)
                    text.append(prevLineLength - text.size(), ' ');
                prevLineLength = text.size();
                std::cout << '\r' << text << std::flush;
            }

            const auto elapsed = std::chrono::steady_clock::now() - loopStart;
            const auto sleepFor = std::chrono::duration_cast<std::chrono::nanoseconds>(period) - elapsed;
            if (sleepFor.count() > 0)
                std::this_thread::sleep_for(sleepFor);
        }

        flushCsvWindows(models, logFrames, true);

        std::cout << '\n';
        radar.shutdown();
        for (auto& slot : models)
            slot->pipeline.shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "\nerror: " << ex.what() << '\n';
        return 1;
    }
}
