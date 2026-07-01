#include <chrono>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "core/json.h"
#include "device/platform/srs_r4sn.h"

using ws::json;
using namespace ws::dev;

namespace
{
    volatile std::sig_atomic_t g_running = 1;

    void onSignal(int)
    {
        g_running = 0;
    }

    json loadSrsR4snConfig(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open())
            throw std::runtime_error("failed to open " + path);

        json root;
        in >> root;

        for (const auto& device : root.at("device_list"))
        {
            if (device.at("class").get<std::string>() == "srs_r4sn")
                return device;
        }

        throw std::runtime_error("srs_r4sn device not found in " + path);
    }

    float degToRad(float degrees)
    {
        return degrees * static_cast<float>(M_PI) / 180.0f;
    }

    float iqMagnitude(const RadarIQ& sample)
    {
        return std::hypot(sample.real, sample.imag);
    }
}

int main(int argc, char* argv[])
{
    const std::string configPath = argc > 1 ? argv[1] : "bin/data/device_list.json";
    const float azimuthDeg = argc > 2 ? std::stof(argv[2]) : 0.0f;
    const float elevationDeg = argc > 3 ? std::stof(argv[3]) : 0.0f;
    const float distanceM = argc > 4 ? std::stof(argv[4]) : 2.0f;
    const int pollMs = argc > 5 ? std::stoi(argv[5]) : 500;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try
    {
        const json config = loadSrsR4snConfig(configPath);
        SRSR4SN radar;

        const int initCode = radar.init(config);
        std::cout << "init => " << initCode << " (" << radar.getErrorString(initCode) << ")\n";
        if (initCode != 0)
            return 1;

        const auto& deviceConfig = radar.getConfig();
        std::cout << "host " << deviceConfig.host
                  << "  pc:" << deviceConfig.pointCloudPort
                  << "  iq:" << deviceConfig.iqPort
                  << "  iq target az=" << azimuthDeg << "deg el=" << elevationDeg
                  << "deg dist=" << distanceM << "m"
                  << "  poll=" << pollMs << "ms\n";
        std::cout << "Ctrl+C to stop\n";

        RadarIQRequest iqRequest{};
        iqRequest.azimuth = degToRad(azimuthDeg);
        iqRequest.elevation = degToRad(elevationDeg);
        iqRequest.distance = distanceM;

        while (g_running)
        {
            RadarPointCloud frame;
            radar.getLatestPointCloudFrameAsync(frame).get();
            if (!frame.points.empty())
            {
                std::cout << "frame " << frame.frameIndex
                          << ": " << frame.points.size() << " points\n";
            }
            else
            {
                std::cout << "frame: (waiting)\n";
            }

            std::vector<RadarIQResponse> iqResponses;
            radar.requestIQAsync({iqRequest}, iqResponses).get();
            if (iqResponses.empty())
            {
                std::cout << "iq: (no response)\n";
            }
            else
            {
                const RadarIQ& sample = iqResponses.front().iq;
                std::cout << std::fixed << std::setprecision(4)
                          << "iq: |z|=" << iqMagnitude(sample)
                          << "  I=" << sample.real
                          << "  Q=" << sample.imag << '\n';
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        }

        radar.shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
