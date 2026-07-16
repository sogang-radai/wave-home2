#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"
#include "device/interface/audio.h"
#include "device/interface/infrared.h"
#include "device/platform/radai_ws.h"
#include "util/arg_parser.h"

using ws::json;
using namespace ws::dev;

namespace
{
    json loadDeviceList(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open())
            throw std::runtime_error("failed to open " + path);

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
        throw std::runtime_error("wave_station device not found");
    }

    void printResult(const json& result)
    {
        if (result.contains("code"))
        {
            std::cout << "  code " << result["code"].get<int>();
            if (result.contains("message"))
                std::cout << " (" << result["message"].get<std::string>() << ")";
            std::cout << '\n';
        }
        else
        {
            std::cout << "  " << result.dump() << '\n';
        }
    }

    void printInvoke(const std::string& name, int code)
    {
        std::cout << "  " << name << " => " << (code == 0 ? "ok" : "fail") << " (" << code << ")\n";
    }

    void printIrFrame(const IrTimingFrame& frame)
    {
        std::cout << "  pulses=" << frame.timingsUs.size()
                  << " overflow=" << (frame.overflow ? "yes" : "no");
        if (!frame.matchedCommandId.empty())
            std::cout << " commandId=" << frame.matchedCommandId;
        std::cout << '\n';
        std::cout << "  timings:";
        for (uint16_t us : frame.timingsUs)
            std::cout << ' ' << us;
        std::cout << '\n';
    }

    bool isSensorTarget(const std::string& target)
    {
        return target == "ambient_light" || target == "lux"
            || target == "temperature" || target == "temp"
            || target == "humidity";
    }

    std::string normalizeTarget(const std::string& target)
    {
        if (target == "lux")
            return "ambient_light";
        if (target == "temp")
            return "temperature";
        if (target == "ir")
            return "ir_receive";
        return target;
    }

    void printHelp()
    {
        std::cout <<
            "commands:\n"
            "  gc gs gst gml genv gir     get caps / session / status / mic_level / env / last_ir\n"
            "  sub <target>               subscribe (mic_opus|mic_pcm|ir_receive|\n"
            "                             ambient_light|temperature|humidity)\n"
            "  unsub <target>             unsubscribe\n"
            "  ir <commandId> [repeat]    send_ir from ir_list.json\n"
            "  irtx <us...> [--carrier N] [--repeat N]\n"
            "                             send raw IR timings (microseconds)\n"
            "  irrx [seconds]             wait for raw IR receive (default 30)\n"
            "  env [seconds]              subscribe sensors + poll until data (default 10)\n"
            "  rec wav <path> [seconds]   record mic to WAV (default 5)\n"
            "  play wav <path> [volume]   play WAV on speaker (volume 0.0~2.0, default 1.0)\n"
            "  mic                        fetch latest mic frame (subscribes if needed)\n"
            "  wait [seconds]             poll status until IR or timeout (default 30)\n"
            "  h q                        help / quit\n";
    }

    class ScopedMicSubscription
    {
    public:
        explicit ScopedMicSubscription(RadaiWs& ws) :
            m_ws(ws),
            m_hadMic(ws.getSubscriptionState().micPcm || ws.getSubscriptionState().micOpus)
        {
            if (!m_hadMic)
            {
                const auto caps = ws.query("capabilities", json::object());
                const std::string target =
                    caps.value("mic_opus", false) ? "mic_opus" : "mic_pcm";
                m_subscribed = ws.invoke("subscribe", {{"target", target}}) == 0;
            }
        }

        ~ScopedMicSubscription()
        {
            if (m_subscribed)
            {
                const auto caps = m_ws.query("capabilities", json::object());
                const std::string target =
                    caps.value("mic_opus", false) ? "mic_opus" : "mic_pcm";
                (void)m_ws.invoke("unsubscribe", {{"target", target}});
            }
        }

    private:
        RadaiWs& m_ws;
        bool m_hadMic = false;
        bool m_subscribed = false;
    };

    class ScopedIrReceiveSubscription
    {
    public:
        explicit ScopedIrReceiveSubscription(RadaiWs& ws) :
            m_ws(ws),
            m_hadIr(ws.getSubscriptionState().irReceive)
        {
            if (!m_hadIr)
                m_subscribed = m_ws.invoke("subscribe", {{"target", "ir_receive"}}) == 0;
        }

        ~ScopedIrReceiveSubscription()
        {
            if (m_subscribed)
                (void)m_ws.invoke("unsubscribe", {{"target", "ir_receive"}});
        }

    private:
        RadaiWs& m_ws;
        bool m_hadIr = false;
        bool m_subscribed = false;
    };

    class ScopedEnvSubscription
    {
    public:
        explicit ScopedEnvSubscription(RadaiWs& ws) :
            m_ws(ws)
        {
            const auto state = ws.getSubscriptionState();
            maybeSubscribe("ambient_light", state.ambientLight);
            maybeSubscribe("temperature", state.temperature);
            maybeSubscribe("humidity", state.humidity);
        }

        ~ScopedEnvSubscription()
        {
            for (const auto& target : m_subscribed)
                (void)m_ws.invoke("unsubscribe", {{"target", target}});
        }

    private:
        void maybeSubscribe(const std::string& target, bool already)
        {
            if (already)
                return;
            if (m_ws.invoke("subscribe", {{"target", target}, {"intervalMs", 1000}}) == 0)
                m_subscribed.push_back(target);
        }

        RadaiWs& m_ws;
        std::vector<std::string> m_subscribed;
    };

    struct WavPcm
    {
        uint32_t sampleRate = 0;
        uint16_t channels = 0;
        uint16_t bitsPerSample = 16;
        std::vector<int16_t> samples;
    };

    bool readWavFile(const std::string& path, WavPcm& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;

        auto read_u32 = [&](uint32_t& v)
        {
            unsigned char b[4] {};
            in.read(reinterpret_cast<char*>(b), 4);
            if (!in)
                return false;
            v = static_cast<uint32_t>(b[0])
                | (static_cast<uint32_t>(b[1]) << 8)
                | (static_cast<uint32_t>(b[2]) << 16)
                | (static_cast<uint32_t>(b[3]) << 24);
            return true;
        };

        auto read_u16 = [&](uint16_t& v)
        {
            unsigned char b[2] {};
            in.read(reinterpret_cast<char*>(b), 2);
            if (!in)
                return false;
            v = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
            return true;
        };

        char riff[4] {};
        in.read(riff, 4);
        if (std::string(riff, 4) != "RIFF")
            return false;

        uint32_t riffSize = 0;
        if (!read_u32(riffSize))
            return false;
        (void)riffSize;

        char wave[4] {};
        in.read(wave, 4);
        if (std::string(wave, 4) != "WAVE")
            return false;

        uint16_t audioFormat = 0;
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
        uint32_t dataSize = 0;
        std::streampos dataPos {};

        while (in)
        {
            char chunkId[4] {};
            in.read(chunkId, 4);
            if (!in)
                break;

            uint32_t chunkSize = 0;
            if (!read_u32(chunkSize))
                return false;

            const std::string id(chunkId, 4);
            if (id == "fmt ")
            {
                if (!read_u16(audioFormat) || !read_u16(channels) || !read_u32(sampleRate))
                    return false;

                uint32_t byteRate = 0;
                uint16_t blockAlign = 0;
                if (!read_u32(byteRate) || !read_u16(blockAlign) || !read_u16(bitsPerSample))
                    return false;

                if (chunkSize > 16)
                    in.seekg(static_cast<std::streamoff>(chunkSize - 16), std::ios::cur);
            }
            else if (id == "data")
            {
                dataPos = in.tellg();
                dataSize = chunkSize;
                in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
            }
            else
            {
                in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
            }

            if (chunkSize % 2 == 1)
                in.seekg(1, std::ios::cur);
        }

        if (audioFormat != 1 || bitsPerSample != 16 || dataSize == 0 || dataPos == std::streampos(-1))
            return false;

        in.clear();
        in.seekg(dataPos);
        out.samples.resize(dataSize / sizeof(int16_t));
        in.read(reinterpret_cast<char*>(out.samples.data()), static_cast<std::streamsize>(dataSize));
        if (!in)
            return false;

        out.sampleRate = sampleRate;
        out.channels = channels;
        out.bitsPerSample = bitsPerSample;
        return true;
    }

    bool writeWavFile(const std::string& path, const WavPcm& pcm)
    {
        if (pcm.samples.empty() || pcm.sampleRate == 0 || pcm.channels == 0)
            return false;

        std::ofstream out(path, std::ios::binary);
        if (!out)
            return false;

        const uint16_t bitsPerSample = pcm.bitsPerSample ? pcm.bitsPerSample : 16;
        const uint32_t byteRate = pcm.sampleRate * pcm.channels * bitsPerSample / 8;
        const uint16_t blockAlign = static_cast<uint16_t>(pcm.channels * bitsPerSample / 8);
        const uint32_t dataSize = static_cast<uint32_t>(pcm.samples.size() * sizeof(int16_t));
        const uint32_t riffSize = 36 + dataSize;

        auto write_u32 = [&](uint32_t v)
        {
            const unsigned char b[4] = {
                static_cast<unsigned char>(v & 0xff),
                static_cast<unsigned char>((v >> 8) & 0xff),
                static_cast<unsigned char>((v >> 16) & 0xff),
                static_cast<unsigned char>((v >> 24) & 0xff),
            };
            out.write(reinterpret_cast<const char*>(b), 4);
        };

        auto write_u16 = [&](uint16_t v)
        {
            const unsigned char b[2] = {
                static_cast<unsigned char>(v & 0xff),
                static_cast<unsigned char>((v >> 8) & 0xff),
            };
            out.write(reinterpret_cast<const char*>(b), 2);
        };

        out.write("RIFF", 4);
        write_u32(riffSize);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        write_u32(16);
        write_u16(1);
        write_u16(pcm.channels);
        write_u32(pcm.sampleRate);
        write_u32(byteRate);
        write_u16(blockAlign);
        write_u16(bitsPerSample);
        out.write("data", 4);
        write_u32(dataSize);
        out.write(reinterpret_cast<const char*>(pcm.samples.data()), static_cast<std::streamsize>(dataSize));
        return static_cast<bool>(out);
    }

    bool parseIrTransmitArgs(std::istringstream& in, std::vector<uint16_t>& timings, uint32_t& carrierHz, uint16_t& repeat)
    {
        timings.clear();
        carrierHz = 38000;
        repeat = 0;

        std::string token;
        while (in >> token)
        {
            if (token == "--carrier")
            {
                unsigned long value = 0;
                if (!(in >> value))
                    return false;
                carrierHz = static_cast<uint32_t>(value);
            }
            else if (token == "--repeat")
            {
                unsigned long value = 0;
                if (!(in >> value))
                    return false;
                repeat = static_cast<uint16_t>(value);
            }
            else
            {
                try
                {
                    const unsigned long us = std::stoul(token);
                    timings.push_back(static_cast<uint16_t>(us));
                }
                catch (...)
                {
                    return false;
                }
            }
        }

        return !timings.empty();
    }

    bool recordWav(RadaiWs& ws, IAudioSource& source, const std::string& path, uint32_t seconds)
    {
        ScopedMicSubscription guard(ws);
        const AudioFormat fmt = source.getSourceFormat();
        if (fmt.sampleRate == 0 || fmt.channels == 0)
        {
            std::cout << "  invalid source format\n";
            return false;
        }

        // Keep enough backlog so a slow drain loop does not drop 20ms frames.
        const size_t prevQueue = source.getAudioQueueSize();
        source.setAudioQueueSize(std::max<size_t>(prevQueue, 64));

        // Drop stale frames already sitting in the queue.
        {
            AudioFrame discard;
            while (source.popFrame(discard))
            {
            }
        }

        std::vector<int16_t> captured;
        captured.reserve(static_cast<size_t>(fmt.sampleRate) * fmt.channels * seconds);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::cout << "  recording " << seconds << "s ...\n";

        while (std::chrono::steady_clock::now() < deadline)
        {
            AudioFrame frame;
            bool got = false;
            while (source.popFrame(frame))
            {
                got = true;
                if (!frame.samples.empty())
                    captured.insert(captured.end(), frame.samples.begin(), frame.samples.end());
            }
            if (!got)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // Drain remaining queued frames that arrived near the deadline.
        {
            AudioFrame frame;
            while (source.popFrame(frame))
            {
                if (!frame.samples.empty())
                    captured.insert(captured.end(), frame.samples.begin(), frame.samples.end());
            }
        }

        source.setAudioQueueSize(prevQueue);

        if (captured.empty())
        {
            std::cout << "  no audio captured\n";
            return false;
        }

        WavPcm wav;
        wav.sampleRate = fmt.sampleRate;
        wav.channels = static_cast<uint16_t>(fmt.channels);
        wav.bitsPerSample = static_cast<uint16_t>(fmt.sampleSize ? fmt.sampleSize : 16);
        wav.samples = std::move(captured);

        if (!writeWavFile(path, wav))
        {
            std::cout << "  failed to write " << path << '\n';
            return false;
        }

        const double durationSec =
            static_cast<double>(wav.samples.size()) / static_cast<double>(wav.sampleRate * wav.channels);
        std::cout << "  saved " << wav.samples.size() << " samples ("
                  << durationSec << "s) -> " << path << '\n';
        return true;
    }

    void apply_volume(std::vector<int16_t>& samples, float volume)
    {
        if (std::fabs(volume - 1.0f) < 1e-6f)
            return;

        for (int16_t& sample : samples)
        {
            const float scaled = static_cast<float>(sample) * volume;
            if (scaled > 32767.0f)
                sample = 32767;
            else if (scaled < -32768.0f)
                sample = -32768;
            else
                sample = static_cast<int16_t>(scaled);
        }
    }

    bool playWav(RadaiWs& ws, IAudioSink& sink, const std::string& path, float volume)
    {
        WavPcm wav;
        if (!readWavFile(path, wav))
        {
            std::cout << "  failed to read " << path << '\n';
            return false;
        }

        const AudioFormat sinkFmt = sink.getSinkFormat();
        if (sinkFmt.sampleRate != wav.sampleRate || sinkFmt.channels != wav.channels)
        {
            std::cout << "  WAV format " << wav.sampleRate << "Hz x" << wav.channels
                      << " does not match sink " << sinkFmt.sampleRate << "Hz x" << sinkFmt.channels << '\n';
            return false;
        }

        apply_volume(wav.samples, volume);

        const uint32_t frameMs = ws.getAudioConfig().frameDurationMs;
        const size_t frameSamples = std::max<size_t>(
            1,
            static_cast<size_t>(sinkFmt.sampleRate) * frameMs / 1000 * sinkFmt.channels);

        std::cout << "  playing " << path << " (" << wav.samples.size() << " samples"
                  << ", volume=" << volume << ")\n";

        const auto start = std::chrono::steady_clock::now();
        size_t samplesSent = 0;

        for (size_t offset = 0; offset < wav.samples.size(); offset += frameSamples)
        {
            const size_t count = std::min(frameSamples, wav.samples.size() - offset);
            AudioFrame frame;
            frame.samples.assign(wav.samples.begin() + static_cast<ptrdiff_t>(offset),
                wav.samples.begin() + static_cast<ptrdiff_t>(offset + count));

            if (!sink.playFrame(frame))
            {
                std::cout << "  playFrame failed at offset " << offset << '\n';
                return false;
            }

            samplesSent += count;
            const auto target = start + std::chrono::microseconds(
                static_cast<int64_t>(samplesSent) * 1000000
                / static_cast<int64_t>(sinkFmt.sampleRate * sinkFmt.channels));
            std::this_thread::sleep_until(target);
        }

        sink.stopPlayback();
        std::cout << "  done\n";
        return true;
    }

    bool runQuery(RadaiWs& ws, std::string_view name)
    {
        printResult(ws.query(name, json::object()));
        return true;
    }

    bool runCommand(RadaiWs& ws, const std::string& cmd, std::istringstream& in)
    {
        if (cmd == "q" || cmd == "quit" || cmd == "exit")
            return false;
        if (cmd == "h" || cmd == "help" || cmd == "?")
        {
            printHelp();
            return true;
        }
        if (cmd == "gc")
            return runQuery(ws, "capabilities");
        if (cmd == "gs")
            return runQuery(ws, "session");
        if (cmd == "gst")
            return runQuery(ws, "status");
        if (cmd == "gml")
            return runQuery(ws, "mic_level");
        if (cmd == "genv")
            return runQuery(ws, "env");
        if (cmd == "gir")
            return runQuery(ws, "last_ir");

        if (cmd == "sub" || cmd == "subscribe")
        {
            std::string target;
            in >> target;
            if (target.empty())
            {
                std::cout << "  usage: sub <mic_opus|mic_pcm|ir_receive|ambient_light|temperature|humidity>\n";
                return true;
            }
            target = normalizeTarget(target);
            json params = {{"target", target}};
            if (isSensorTarget(target))
                params["intervalMs"] = 1000;
            printInvoke("subscribe", ws.invoke("subscribe", params));
            if (isSensorTarget(target))
                std::cout << "  tip: use 'genv' or 'env' to read sensor values\n";
            return true;
        }

        if (cmd == "unsub" || cmd == "unsubscribe")
        {
            std::string target;
            in >> target;
            if (target.empty())
            {
                std::cout << "  usage: unsub <target>\n";
                return true;
            }
            target = normalizeTarget(target);
            printInvoke("unsubscribe", ws.invoke("unsubscribe", {{"target", target}}));
            return true;
        }

        if (cmd == "env")
        {
            int seconds = 10;
            in >> seconds;
            if (seconds <= 0)
                seconds = 10;

            ScopedEnvSubscription guard(ws);
            json before = ws.query("env", json::object());
            std::cout << "  waiting for env updates (" << seconds << "s)...\n";
            if (!before.empty())
            {
                std::cout << "  current:\n";
                printResult(before);
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                json current = ws.query("env", json::object());
                if (!current.empty() && current != before)
                {
                    std::cout << "  env update:\n";
                    printResult(current);
                    before = current;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            json finalEnv = ws.query("env", json::object());
            if (finalEnv.empty())
                std::cout << "  no env data received (check ESP SensorBody quality!=0)\n";
            else
            {
                std::cout << "  final:\n";
                printResult(finalEnv);
            }
            return true;
        }

        if (cmd == "ir" || cmd == "send_ir")
        {
            std::string commandId;
            int repeat = 0;
            in >> commandId >> repeat;
            if (commandId.empty())
            {
                std::cout << "  usage: ir <commandId> [repeat]\n";
                return true;
            }
            printInvoke("send_ir", ws.invoke("send_ir", {{"commandId", commandId}, {"repeat", repeat}}));
            return true;
        }

        if (cmd == "irtx")
        {
            std::vector<uint16_t> timings;
            uint32_t carrierHz = 38000;
            uint16_t repeat = 0;
            if (!parseIrTransmitArgs(in, timings, carrierHz, repeat))
            {
                std::cout << "  usage: irtx <us...> [--carrier N] [--repeat N]\n";
                return true;
            }

            auto* tx = dynamic_cast<IIrTransmitter*>(&ws);
            if (!tx)
            {
                std::cout << "  device does not support IIrTransmitter\n";
                return true;
            }

            printInvoke("transmitTimings", tx->transmitTimings(timings, carrierHz, repeat));
            return true;
        }

        if (cmd == "irrx")
        {
            int seconds = 30;
            in >> seconds;
            if (seconds <= 0)
                seconds = 30;

            auto* rx = dynamic_cast<IIrReceiver*>(&ws);
            if (!rx)
            {
                std::cout << "  device does not support IIrReceiver\n";
                return true;
            }

            ScopedIrReceiveSubscription guard(ws);
            IrTimingFrame frame;
            if (rx->waitForIr(frame, static_cast<uint32_t>(seconds) * 1000u))
            {
                std::cout << "  IR received:\n";
                printIrFrame(frame);
            }
            else
            {
                std::cout << "  timeout (" << seconds << "s)\n";
            }
            return true;
        }

        if (cmd == "rec")
        {
            std::string kind, path;
            in >> kind >> path;
            if (kind != "wav" || path.empty())
            {
                std::cout << "  usage: rec wav <path> [seconds]\n";
                return true;
            }

            uint32_t seconds = 5;
            in >> seconds;
            if (seconds == 0)
                seconds = 5;

            auto* source = dynamic_cast<IAudioSource*>(&ws);
            if (!source)
            {
                std::cout << "  device does not support IAudioSource\n";
                return true;
            }

            (void)recordWav(ws, *source, path, seconds);
            return true;
        }

        if (cmd == "play")
        {
            std::string kind, path;
            in >> kind >> path;
            if (kind != "wav" || path.empty())
            {
                std::cout << "  usage: play wav <path> [volume]\n";
                return true;
            }

            float volume = 1.0f;
            if (in >> volume)
            {
                if (!(volume >= 0.0f && volume <= 2.0f))
                {
                    std::cout << "  volume must be between 0.0 and 2.0\n";
                    return true;
                }
            }
            else
            {
                in.clear();
            }

            auto* sink = dynamic_cast<IAudioSink*>(&ws);
            if (!sink)
            {
                std::cout << "  device does not support IAudioSink\n";
                return true;
            }

            (void)playWav(ws, *sink, path, volume);
            return true;
        }

        if (cmd == "mic")
        {
            ScopedMicSubscription guard(ws);
            AudioFrame frame;
            if (ws.getLatestFrame(frame))
            {
                std::cout << "  samples=" << frame.samples.size()
                          << " ts=" << frame.timestamp << '\n';
            }
            else
            {
                std::cout << "  no mic frame (link connected="
                          << (ws.isLinkConnected() ? "yes" : "no") << ")\n";
            }
            return true;
        }

        if (cmd == "wait")
        {
            int seconds = 30;
            in >> seconds;
            if (seconds <= 0)
                seconds = 30;

            ScopedIrReceiveSubscription guard(ws);
            json before = ws.query("last_ir", json::object());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                json current = ws.query("last_ir", json::object());
                if (!current.empty() && current != before)
                {
                    std::cout << "  IR event:\n";
                    printResult(current);
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            std::cout << "  timeout (" << seconds << "s)\n";
            return true;
        }

        std::cout << "  unknown: " << cmd << " (h for help)\n";
        return true;
    }
}

int main(int argc, const char* argv[])
{
    ArgParser parser("test-ws", "Interactive test for WaveStation (WSP1 / RadaiWs).");
    parser.addArgument("--config", "-c")
        .help("device_list.json path.")
        .defaultValue("bin/device/device_list.json");

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

    try
    {
        json config = findWaveStationConfig(loadDeviceList(configPath));
        if (!config.contains("room_id"))
            config["room_id"] = "1111111111111111";

        RadaiWs station;
        const int rc = station.init(config);
        if (rc != 0)
        {
            std::cerr << "init failed: " << rc << " (" << station.getErrorString(rc) << ")\n";
            return 1;
        }

        const auto& iface = station.getInterfaceConfig();
        std::cout << "ready (" << iface.host << ":" << iface.port << ") — connected\n";
        printHelp();

        std::string line;
        std::cout << "> " << std::flush;
        while (std::getline(std::cin, line))
        {
            std::istringstream in(line);
            std::string cmd;
            in >> cmd;

            if (!cmd.empty() && !runCommand(station, cmd, in))
                break;

            std::cout << "> " << std::flush;
        }

        std::cout << "\nbye\n";
        station.shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
