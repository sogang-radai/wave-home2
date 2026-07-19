#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

#include "core/json.h"
#include "device/platform/reolink_e1pro.h"
#include "service/go2rtc_service.h"

using ws::json;
using namespace ws::dev;
using ws::service::Go2RtcService;

namespace
{
    json loadReolinkConfig(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open())
            throw std::runtime_error("failed to open " + path);

        json root;
        in >> root;

        for (const auto& device : root.at("device_list"))
        {
            if (device.at("class").get<std::string>() == "reolink_e1_pro")
                return device;
        }
        throw std::runtime_error("reolink_e1_pro device not found in " + path);
    }

    bool endsWith(const std::string& s, const std::string& suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    void saveImage(const CameraFrame& frame, const std::string& path)
    {
        if (frame.data.empty())
        {
            std::cout << "  (empty frame)\n";
            return;
        }

        if (endsWith(path, ".png"))
        {
            int w = 0, h = 0, ch = 0;
            unsigned char* pixels = stbi_load_from_memory(
                frame.data.data(), static_cast<int>(frame.data.size()), &w, &h, &ch, 3);
            if (!pixels)
            {
                std::cout << "  decode failed: " << stbi_failure_reason() << '\n';
                return;
            }
            const int rc = stbi_write_png(path.c_str(), w, h, 3, pixels, w * 3);
            stbi_image_free(pixels);
            std::cout << (rc ? "  saved PNG " : "  PNG write failed ") << w << "x" << h << " -> " << path << '\n';
        }
        else
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(frame.data.data()), static_cast<std::streamsize>(frame.data.size()));
            std::cout << "  saved " << frame.data.size() << " bytes -> " << path << '\n';
        }
    }

    void printHelp()
    {
        std::cout <<
            "commands:\n"
            "  stream                                 show RTSP / go2rtc URLs\n"
            "  capture image <path>                   snapshot (.png decodes via stb, else raw jpeg)\n"
            "  capture audio <path> [seconds]         record mic to WAV (default 5s)\n"
            "  play audio <path>                      play a local file on the camera speaker\n"
            "  play stop                              stop speaker playback\n"
            "  ptz move <pan> <tilt> [zoom] [ms]      continuous move (-1..1); ms auto-stops\n"
            "  ptz stop                               stop movement\n"
            "  ptz preset <id>                        go to preset\n"
            "  ptz presets                            list presets\n"
            "  ptz home                               go to home position\n"
            "  ptz caps                               show PTZ capabilities\n"
            "  help                                   this help\n"
            "  quit | exit                            shutdown and exit\n";
    }

    void handlePtz(ReolinkE1Pro& cam, std::istringstream& args)
    {
        std::string sub;
        args >> sub;

        if (sub == "move")
        {
            float pan = 0, tilt = 0, zoom = 0;
            uint32_t ms = 0;
            args >> pan >> tilt;
            args >> zoom;
            args >> ms;
            PtzVector v{pan, tilt, zoom};
            std::cout << "  movePtz(" << pan << "," << tilt << "," << zoom << ", " << ms << "ms) => "
                      << (cam.movePtz(v, ms) ? "ok" : "fail") << '\n';
        }
        else if (sub == "stop")
        {
            std::cout << "  stopPtz => " << (cam.stopPtz() ? "ok" : "fail") << '\n';
        }
        else if (sub == "preset")
        {
            uint32_t id = 0;
            args >> id;
            std::cout << "  gotoPtzPreset(" << id << ") => " << (cam.gotoPtzPreset(id) ? "ok" : "fail") << '\n';
        }
        else if (sub == "presets")
        {
            std::vector<PtzPreset> presets;
            if (cam.enumeratePtzPresets(presets))
            {
                std::cout << "  " << presets.size() << " preset(s):\n";
                for (const auto& p : presets)
                    std::cout << "    [" << p.id << "] " << p.name << '\n';
            }
            else
                std::cout << "  enumeratePtzPresets => fail\n";
        }
        else if (sub == "home")
        {
            std::cout << "  movePtzHome => " << (cam.movePtzHome() ? "ok" : "fail") << '\n';
        }
        else if (sub == "caps")
        {
            const PtzCapabilities c = cam.getPtzCapabilities();
            std::cout << "  pan=" << c.pan << " tilt=" << c.tilt << " zoom=" << c.zoom
                      << " absolute=" << c.absolute << " presets=" << c.presets
                      << " home=" << c.home << " maxPresets=" << c.maxPresets << '\n';
        }
        else
            std::cout << "  unknown ptz subcommand: " << sub << '\n';
    }

    void handleCapture(ReolinkE1Pro& cam, std::istringstream& args)
    {
        std::string kind, path;
        args >> kind >> path;
        if (path.empty())
        {
            std::cout << "  usage: capture <image|audio> <path> [seconds]\n";
            return;
        }

        if (kind == "image")
        {
            CameraFrame frame;
            if (cam.captureFrame(frame))
                saveImage(frame, path);
            else
                std::cout << "  captureFrame => fail\n";
        }
        else if (kind == "audio")
        {
            uint32_t seconds = 5;
            args >> seconds;
            std::cout << "  recording " << seconds << "s ...\n";
            std::cout << "  recordAudioToFile => " << (cam.recordAudioToFile(path, seconds) ? "ok" : "fail") << '\n';
        }
        else
            std::cout << "  unknown capture kind: " << kind << '\n';
    }

    void handlePlay(ReolinkE1Pro& cam, std::istringstream& args)
    {
        std::string kind, path;
        args >> kind;
        if (kind == "stop")
        {
            cam.stopPlayback();
            std::cout << "  playback stopped\n";
            return;
        }
        args >> path;
        if (kind == "audio" && !path.empty())
            std::cout << "  playAudioFile => " << (cam.playAudioFile(path) ? "ok" : "fail") << '\n';
        else
            std::cout << "  usage: play audio <path> | play stop\n";
    }
}

int main(int argc, char* argv[])
{
    const std::string configPath = argc > 1 ? argv[1] : "bin/device/device_list.json";

    try
    {
        // Non-default ports so the test never collides with a running server.
        Go2RtcService::Config cfg;
        cfg.apiPort = 11984;
        cfg.rtspPort = 18554;
        Go2RtcService::get().configure(cfg);

        json config = loadReolinkConfig(configPath);
        if (!config.contains("room_id"))
            config["room_id"] = "1111111111111111";

        ReolinkE1Pro camera;
        const int rc = camera.init(config);
        if (rc != 0)
        {
            std::cerr << "camera.init failed: " << rc << '\n';
            return 1;
        }
        std::cout << "camera ready (" << camera.getConfig().host << ")\n";
        printHelp();

        std::string line;
        std::cout << ">> " << std::flush;
        while (std::getline(std::cin, line))
        {
            std::istringstream in(line);
            std::string cmd;
            in >> cmd;

            if (cmd.empty()) { /* fallthrough to prompt */ }
            else if (cmd == "quit" || cmd == "exit") break;
            else if (cmd == "help") printHelp();
            else if (cmd == "stream") std::cout << "  " << camera.query("stream", json::object()).dump() << '\n';
            else if (cmd == "capture") handleCapture(camera, in);
            else if (cmd == "play") handlePlay(camera, in);
            else if (cmd == "ptz") handlePtz(camera, in);
            else std::cout << "  unknown command: " << cmd << " (type 'help')\n";

            std::cout << ">> " << std::flush;
        }

        std::cout << "\nshutting down...\n";
        camera.shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
