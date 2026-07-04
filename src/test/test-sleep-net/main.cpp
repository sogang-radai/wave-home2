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

        // 상태: 윈도우에서 가장 오래 유지된 클래스 + 해당 클래스 프레임들의 평균 확률.
        int32_t statusClass = -1;
        std::vector<float> statusScores;

        // 뒤척임: 윈도우 내 tossIndex 최대인 피크 프레임 기준.
        bool tossValid = false;
        int32_t tossClass = -1;
        std::vector<float> tossScores;
        float tossIndex = 0.0f;
    };

    // 프레임 단위 결과 윈도우를 하나의 로그 행으로 축약한다.
    // 상태는 최다 프레임(최장 유지) 클래스, 뒤척임은 최대 tossIndex 프레임을 채택.
    AggregatedLog aggregateWindow(const std::vector<SleepResult>& window)
    {
        AggregatedLog log;
        nowDateTime(log.date, log.time);
        log.sampleCount = window.size();
        if (window.empty())
            return log;

        log.frameBegin = window.front().frameIndex;
        log.frameEnd = window.back().frameIndex;

        std::vector<int32_t> classCounts;
        for (const SleepResult& r : window)
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
        for (const SleepResult& r : window)
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

        for (const SleepResult& r : window)
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
        out << "date,time,frame_begin,frame_end,samples,status_label";
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

    // 레이더 링 버퍼에서 아직 밀어 넣지 않은 프레임을 인덱스 오름차순으로 모두 push.
    // 윈도우가 연속 프레임을 요구하므로 최신 프레임만 가져오면 안 되고, 사이 프레임까지 채워야 한다.
    size_t pushNewFrames(
        SRSR4SN& radar,
        SleepPipeline& pipeline,
        bool& has_last,
        uint64_t& last_index,
        size_t& out_last_point_count)
    {
        std::vector<uint64_t> indices;
        radar.enumeratePointCloudFrameIndices(indices);
        if (indices.empty())
            return 0;

        std::sort(indices.begin(), indices.end());

        size_t pushed = 0;
        for (const uint64_t idx : indices)
        {
            if (has_last && idx <= last_index)
                continue;

            RadarPointCloud frame;
            if (!radar.getPointCloudFrame(idx, frame))
                continue;

            out_last_point_count = frame.points.size();
            pipeline.pushFrame(std::move(frame));

            has_last = true;
            last_index = idx;
            ++pushed;
        }

        return pushed;
    }
}

int main(int argc, const char* argv[])
{
    ArgParser parser("test-sleep-net", "Real-time SleepNet (bed/toss) inference test on SRS R4SN radar.");
    parser.addArgument("--config", "-c")
        .help("device_list.json path.")
        .defaultValue("bin/data/device_list.json");
    parser.addArgument("--model", "-m")
        .help("sleep model.json path.")
        .defaultValue("bin/models/sleep/model.json");
    parser.addArgument("--fps", "-f")
        .help("output/poll rate in Hz.")
        .defaultValue("20");
    parser.addArgument("--index", "-i")
        .help("srs_r4sn device index to use.")
        .defaultValue("0");
    parser.addArgument("--list", "-l")
        .help("list srs_r4sn devices and exit.")
        .actionFlag();
    parser.addArgument("--csv")
        .help("append aggregated results to a CSV log file.");
    parser.addArgument("--log-frames", "-n")
        .help("frames aggregated per CSV row (0 = ~1 second based on fps).")
        .defaultValue("0");

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
    const std::string modelFile = parser.get<std::string>("model");
    const double fps = parser.get<double>("fps");
    const int32_t radarIndex = parser.get<int32_t>("index");
    const bool listOnly = parser.has("list");
    const std::string csvPath = parser.has("csv") ? parser.get<std::string>("csv") : std::string();
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

        const std::filesystem::path modelPath(modelFile);
        std::ifstream modelIn(modelPath);
        if (!modelIn.is_open())
            throw std::runtime_error("failed to open model " + modelFile);

        json modelConfig;
        modelIn >> modelConfig;

        const std::string baseDir = modelPath.has_parent_path()
            ? modelPath.parent_path().string()
            : std::string(".");

        SleepPipeline pipeline;
        std::string error;
        if (!pipeline.init(baseDir, modelConfig, error))
            throw std::runtime_error("sleep pipeline init failed: " + error);

        const std::vector<std::string>& bedLabels = pipeline.getBedLabels();
        const std::vector<std::string>& tossLabels = pipeline.getTossLabels();

        std::cout << "model: " << pipeline.getModelName()
                  << "  embedding=" << pipeline.getEmbeddingSize()
                  << "  bed_window=" << pipeline.getBedWindow()
                  << "  toss_window=" << pipeline.getTossWindow()
                  << "  toss_active_status=" << pipeline.getTossActiveStatus() << '\n';

        std::ofstream csvFile;
        if (!csvPath.empty())
        {
            const bool exists = std::filesystem::exists(csvPath)
                && std::filesystem::file_size(csvPath) > 0;
            csvFile.open(csvPath, std::ios::app);
            if (!csvFile.is_open())
                throw std::runtime_error("failed to open csv log " + csvPath);
            if (!exists)
                writeCsvHeader(csvFile, bedLabels, tossLabels);

            std::cout << "csv log: " << csvPath
                      << "  (every " << logFrames << " frames)\n";
        }

        SRSR4SN radar;
        const int initCode = radar.init(radarConfig);
        std::cout << "radar init => " << initCode << " (" << radar.getErrorString(initCode) << ")\n";
        if (initCode != 0)
            return 1;

        std::cout << "target " << fps << " Hz  (radar [" << radarIndex << "] "
                  << radarConfig.value("name", std::string("?")) << ")\n";
        std::cout << "Ctrl+C to stop\n";

        const auto period = std::chrono::duration<double>(1.0 / fps);

        bool hasLast = false;
        uint64_t lastIndex = 0;
        size_t lastPointCount = 0;
        size_t prevLineLength = 0;

        std::vector<SleepResult> logWindow;
        bool haveLoggedFrame = false;
        uint64_t lastLoggedFrame = 0;

        auto lastTick = std::chrono::steady_clock::now()
            - std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);

        while (g_running)
        {
            const auto loopStart = std::chrono::steady_clock::now();

            const size_t pushed = pushNewFrames(radar, pipeline, hasLast, lastIndex, lastPointCount);

            SleepResult result;
            const auto inferStart = std::chrono::steady_clock::now();
            const bool haveResult = pipeline.evaluate(result);
            const auto inferEnd = std::chrono::steady_clock::now();

            const double inferMs =
                std::chrono::duration<double, std::milli>(inferEnd - inferStart).count();
            const double loopHz =
                1.0 / std::max(1e-6, std::chrono::duration<double>(loopStart - lastTick).count());
            lastTick = loopStart;

            // CSV: 프레임 인덱스가 실제로 진행됐을 때만 한 프레임으로 집계(폴링 중복 방지).
            if (csvFile.is_open() && haveResult
                && (!haveLoggedFrame || result.frameIndex != lastLoggedFrame))
            {
                logWindow.push_back(result);
                haveLoggedFrame = true;
                lastLoggedFrame = result.frameIndex;

                if (logWindow.size() >= logFrames)
                {
                    const AggregatedLog aggregated = aggregateWindow(logWindow);
                    writeCsvRow(csvFile, aggregated, bedLabels, tossLabels);
                    logWindow.clear();
                }
            }

            std::ostringstream line;
            line << std::fixed << std::setprecision(1);
            line << "frame " << std::setw(6) << std::setfill('0')
                 << (hasLast ? lastIndex : 0) << std::setfill(' ')
                 << " | " << std::setw(5) << loopHz << "Hz"
                 << " | new " << pushed
                 << " | pts " << std::setw(3) << lastPointCount;

            if (!haveResult)
            {
                line << " | warmup " << pipeline.getFrameCount() << "/" << pipeline.getBedWindow();
            }
            else
            {
                line << " | infer " << std::setprecision(2) << inferMs << "ms"
                     << " | STATUS " << labelOf(bedLabels, result.statusClass)
                     << " [" << formatScores(bedLabels, result.statusScores) << "]";

                if (result.tossValid)
                {
                    line << " | TOSS " << labelOf(tossLabels, result.tossClass)
                         << " idx " << std::setprecision(2) << result.tossIndex
                         << " [" << formatScores(tossLabels, result.tossScores) << "]";
                }
                else
                {
                    line << " | TOSS -";
                }
            }

            std::string text = line.str();
            if (text.size() < prevLineLength)
                text.append(prevLineLength - text.size(), ' ');
            prevLineLength = line.str().size();

            std::cout << '\r' << text << std::flush;

            const auto elapsed = std::chrono::steady_clock::now() - loopStart;
            const auto sleepFor = std::chrono::duration_cast<std::chrono::nanoseconds>(period) - elapsed;
            if (sleepFor.count() > 0)
                std::this_thread::sleep_for(sleepFor);
        }

        if (csvFile.is_open() && !logWindow.empty())
        {
            const AggregatedLog aggregated = aggregateWindow(logWindow);
            writeCsvRow(csvFile, aggregated, bedLabels, tossLabels);
        }

        std::cout << '\n';
        radar.shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "\nerror: " << ex.what() << '\n';
        return 1;
    }
}
