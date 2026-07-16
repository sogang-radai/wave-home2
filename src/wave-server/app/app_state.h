#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <json/json.h>

#include "../device/device_manager.h"
#include "../web/server.h"
#include "app_config.h"
#include "app_setting.h"
#include "launch_options.h"
#include "../db/database.h"
#include "../service/action_queue.h"
#include "../service/rule_store.h"
#include "../service/trigger_manager.h"
#include "../web/http/v1/gesture_store.h"
#include "../web/http/v1/ir_store.h"


#ifdef WAVE_BUILD_TTS
#include "../service/tts_service.h"
#endif

WAVE_NAMESPACE_BEGIN

class AppState;

class TTSState
{
public:
    explicit TTSState(AppState& app);

    bool warmUp(std::string& error);
    bool isReady() const;
#ifdef WAVE_BUILD_TTS
    tts::Service* service(std::string& code);
    std::mutex& generateMutex();
#endif
    void shutdown();

private:
    AppState& m_app;

#ifdef WAVE_BUILD_TTS
    std::mutex m_mutex;
    std::mutex m_generateMutex;
    std::unique_ptr<tts::Service> m_service;
    std::atomic<bool> m_ready{false};
    bool m_taskQueueReady = false;
#endif
};

class IotRuntime
{
public:
    explicit IotRuntime(AppState& app);

    void logEvent(
        const std::string& type,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& message,
        const std::string& triggered_by = "manual",
        const Json::Value& detail = Json::Value(Json::objectValue));
    Json::Value listEvents(const std::string& device_id = "") const;
    int eventCount() const;

    int streamViewers(const std::string& external_id) const;
    bool changeStreamViewers(
        const std::string& external_id,
        bool streaming,
        const std::function<bool()>& on_first_viewer,
        const std::function<void()>& on_last_viewer);
    void resetCameraStreamSession(const std::string& external_id);
    int adjustZoom(const std::string& external_id, int delta);

    std::shared_ptr<std::atomic<bool>> beginDroidMjpegProxy(const std::string& external_id);
    void stopDroidMjpegProxy(const std::string& external_id);
    void finishDroidMjpegProxy(const std::string& external_id, bool phone_lost);

    void shutdown();

private:
    static std::string isoNowKst();

    AppState& m_app;

    mutable std::mutex m_eventMutex;
    std::vector<Json::Value> m_events;
    int64_t m_nextEventId = 1;

    mutable std::mutex m_cameraStreamMutex;
    std::unordered_map<std::string, int> m_cameraStreamViewers;
    std::unordered_map<std::string, int> m_cameraZoomLevels;

    mutable std::mutex m_droidMjpegMutex;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> m_droidMjpegStop;
};

class AppState
{
public:
    static AppState& get();

    AppState();
    ~AppState();

    void init(const LaunchOptions& launch);
    void shutdown();

    std::filesystem::path resolvePath(const std::string& relative) const;

    db::DbClientPtr db() const;

    service::ActionQueue& actionQueue();
    service::RuleStore& ruleStore();
    service::TriggerManager& triggerManager();
    // Alarm scheduling runtime: service::AlarmManager::get()
    web::v1::GestureStore& gestureStore();
    web::v1::IrStore& irStore();

    bool automationReady() const;
    bool hasRuleStore() const;
    bool hasGestureStore() const;
    bool hasIrStore() const;

    void onDatabaseReady(const db::DbClientPtr& client);

    // App
    std::atomic<bool> running{false};
    bool test_mode = false;
    bool demo_mode = false;
    bool no_devices = false;
    std::string anchor_date;
    AppConfig config;
    AppSetting settings;
    std::filesystem::path config_dir;

    // Network
    web::Server server;

    // Devices
    dev::DeviceManager deviceManager;

    // TTS
    TTSState tts;

    // IoT runtime (event log, camera stream sessions, DroidCam MJPEG proxies)
    IotRuntime iot;

private:
    bool loadDeviceManifests(const db::DbClientPtr& client);
    void startAutomationServices();
    void stopAutomationServices();
    void startTriggerRuntime();

    std::unique_ptr<service::ActionQueue> m_actionQueue;
    std::unique_ptr<service::RuleStore> m_ruleStore;
    std::unique_ptr<service::TriggerManager> m_triggerManager;
    std::unique_ptr<web::v1::GestureStore> m_gestureStore;
    std::unique_ptr<web::v1::IrStore> m_irStore;

    bool m_triggerRuntimeStarted = false;
    bool m_initialized = false;
    std::atomic<bool> m_dbReady{false};
};

WAVE_NAMESPACE_END
