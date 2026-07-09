#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <util/arg_parser.h>

#include "app/app_state.h"
#include "app/launch_options.h"
#include "core/logger.h"
#include "core/task_queue.h"

namespace
{
    std::atomic<bool> g_forceExit{false};

    void installShutdownHandlers()
    {
        struct sigaction action {};
        action.sa_handler = [](int signal)
        {
            if (g_forceExit.exchange(true, std::memory_order_acq_rel))
            {
                LOG_WARN("Forced exit (signal {})", signal);
                std::_Exit(128 + signal);
            }

            if (signal == SIGTSTP)
                LOG_INFO("Shutdown requested (Ctrl+Z)");
            else if (signal == SIGINT)
                LOG_INFO("Shutdown requested (Ctrl+C)");
            else
                LOG_INFO("Shutdown requested (signal {})", signal);

            ws::AppState::get().running.store(false, std::memory_order_release);
        };
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;

        sigaction(SIGTSTP, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);
    }

    bool isValidProfile(const std::string& profile)
    {
        return profile == "real" || profile == "demo" || profile == "test";
    }

    ws::LaunchOptions parseLaunchOptions(int argc, const char* argv[])
    {
        ArgParser parser(
            "wave-server",
            "Wave Home server (real :8500, demo :8502, test :8503 — see --help)");
        parser.addArgument("--config", "-c")
            .help("config file path")
            .defaultValue("config.json");
        parser.addArgument("--port", "-p")
            .help("HTTP listen port (overrides config)");
        parser.addArgument("--site", "-s")
            .help("static site directory (overrides config document_root)");
        parser.addArgument("--no-devices", "")
            .help("skip device manager (API+DB stay on; use when appliances are offline)")
            .actionFlag();
        parser.addArgument("--profile", "")
            .help("config profile: real | demo | test (default: real)")
            .defaultValue("real");

        parser.parseArgs(argc, argv);

        ws::LaunchOptions launch;
        launch.config_path = parser.get<std::string>("config");
        launch.profile = parser.get<std::string>("profile");
        if (!isValidProfile(launch.profile))
        {
            throw std::runtime_error(
                "Invalid --profile \"" + launch.profile + "\": use real, demo, or test");
        }
        if (parser.has("port"))
            launch.port = parser.get<uint16_t>("port");
        if (parser.has("site"))
            launch.document_root = parser.get<std::string>("site");
        launch.no_devices = parser.has("no-devices");

        return launch;
    }
}

int main(int argc, const char* argv[])
{
    ws::LaunchOptions launch;
    try
    {
        launch = parseLaunchOptions(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }

    ws::TaskQueue taskQueue;
    if (!taskQueue.init(12))
    {
        LOG_ERROR("Task queue init failed");
        return 1;
    }

    installShutdownHandlers();

    ws::AppState app;
    app.init(launch);
    if (!app.running.load(std::memory_order_acquire))
    {
        LOG_ERROR("Application failed to start");
        return 1;
    }

    LOG_INFO("Main loop started (Ctrl+Z or Ctrl+C to stop)");
    while (app.running.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    LOG_INFO("Main loop stopped");
    app.shutdown();
    taskQueue.shutdown();
    return 0;
}
