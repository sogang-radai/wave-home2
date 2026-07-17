#include "production_profile_runtime.h"

#include "../../core/json.h"
#include "../../core/logger.h"
#include "../../service/alarm_manager.h"
#include "../../service/power_manager.h"
#include "../../service/sleep/sleep_manager.h"
#include "../app_config.h"
#include "../app_state.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    void ensure_server_field(json& server, const char* key, int value)
    {
        if (!server.contains(key))
            server[key] = value;
    }
}

void ProductionProfileRuntime::applyConfigDefaults(AppConfig& config) const
{
    ensure_server_field(config.server, "port", 8500);
    ensure_server_field(config.server, "agent_api_port", 8501);
    if (!config.server.contains("agent_api_bind"))
        config.server["agent_api_bind"] = "127.0.0.1";
    if (config.agent.base_url.empty())
        config.agent.base_url = "http://127.0.0.1:8502";
}

void ProductionProfileRuntime::startServices(AppState& app)
{
    if (app.no_devices)
        WLOG_INFO("Devices skipped (--no-devices or devices_enabled=false)");

    app.startAutomationServices();
}

void ProductionProfileRuntime::startPostListen(AppState& app)
{
    std::string stt_error;
    if (!app.stt.warmUp(stt_error))
        WLOG_WARN("STT warmup failed: {}", stt_error);

    if (app.no_devices)
        return;

    m_startedDeviceStack = true;
    service::PowerManager::get().start();

    std::string tts_error;
    if (!app.tts.warmUp(tts_error))
        WLOG_WARN("TTS warmup failed: {}", tts_error);
}

void ProductionProfileRuntime::onDatabaseReady(AppState& app, const db::DbClientPtr& client)
{
    app.bindAutomationDatabase(client);

    if (app.no_devices)
    {
        app.markDatabaseReady();
        return;
    }

    m_startedDeviceStack = true;

    app.deviceManager.setOnStartupComplete([&app]()
    {
        app.startTriggerRuntime();
    });

    if (!app.deviceManager.manifestLoaded())
    {
        if (!app.loadDeviceManifests(client))
            WLOG_WARN("Device manager load failed");
        else
            app.deviceManager.startDevicesAsync();
    }

    if (app.deviceManager.startupComplete())
        app.startTriggerRuntime();

    service::SleepManager::get().reconcile();
    service::SleepManager::get().start();
    WLOG_INFO("SleepManager started");

    service::AlarmManager::get().start();
    WLOG_INFO("AlarmManager started");

    app.markDatabaseReady();
}

void ProductionProfileRuntime::shutdown(AppState& app)
{
    if (m_startedDeviceStack)
    {
        service::PowerManager::get().stop();
        service::SleepManager::get().stop();
        service::AlarmManager::get().stop();
    }

    app.stopAutomationServices();
}

WAVE_NAMESPACE_END
