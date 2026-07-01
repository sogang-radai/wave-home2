#pragma once

#include <atomic>
#include <string>

#include "../device/device_manager.h"
#include "../web/server.h"
#include "app_setting.h"
#include "../service/llm_client.h"
#include "../nn/gesture_pipeline.h"
#include "../nn/sleep_pipeline.h"

WAVE_NAMESPACE_BEGIN

class AppState
{
public:
    static AppState& get();
    
    AppState();
    ~AppState();

    void init(std::string_view config_path);
    void shutdown();

    // App
    std::atomic<bool> running = false;
    AppSetting settings;
    
    // Network
    web::Server server;

    // Devices
    dev::DeviceManager deviceManager;

    // NN Features
    using GesturePipelinePtr = std::shared_ptr<nn::GesturePipeline>;
    using SleepPipelinePtr = std::shared_ptr<nn::SleepPipeline>;
    using GesturePipelineList = std::vector<GesturePipelinePtr>;
    using SleepPipelineList = std::vector<SleepPipelinePtr>;
    
    GesturePipelineList gesturePipelines;
    SleepPipelineList sleepPipelines;

    // Agentic Features
    llm::Client parentLLMClient;
    llm::Client childLLMClient;

private:
    bool m_initialized = false;
};

WAVE_NAMESPACE_END