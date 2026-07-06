#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include <drogon/orm/DbClient.h>

#include "../device/device_manager.h"
#include "../web/server.h"
#include "app_config.h"
#include "launch_options.h"
#include "app_setting.h"
#include "../nn/gesture_pipeline.h"
#include "../nn/sleep_pipeline.h"

WAVE_NAMESPACE_BEGIN

class AppState
{
public:
    static AppState& get();

    AppState();
    ~AppState();

    void init(const LaunchOptions& launch);
    void shutdown();

    drogon::orm::DbClientPtr db() const;

    // App
    std::atomic<bool> running{false};
    bool test_mode = false;
    bool no_devices = false;
    AppConfig config;
    AppSetting settings;
    std::filesystem::path config_dir;

    // Network
    web::Server server;

    // Devices
    dev::DeviceManager deviceManager;

    // NN Features (wired in later phases)
    using GesturePipelinePtr = std::shared_ptr<nn::GesturePipeline>;
    using SleepPipelinePtr = std::shared_ptr<nn::SleepPipeline>;
    using GesturePipelineList = std::vector<GesturePipelinePtr>;
    using SleepPipelineList = std::vector<SleepPipelinePtr>;

    GesturePipelineList gesturePipelines;
    SleepPipelineList sleepPipelines;

private:
    bool loadDeviceManifests();
    std::filesystem::path resolvePath(const std::string& relative) const;

    bool m_initialized = false;
};

WAVE_NAMESPACE_END
