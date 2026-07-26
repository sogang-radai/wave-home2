#include "production_profile_runtime.h"

#include "../../core/json.h"
#include "../../core/logger.h"
#include "../../service/alarm_manager.h"
#include "../../service/agent/agent_job_queue.h"
#include "../../service/companion/companion_manager.h"
#include "../../service/power_manager.h"
#include "../../service/sleep/sleep_manager.h"
#include "../../service/user_model_manager.h"
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
    service::AgentJobQueue::get().start();
    service::PowerManager::get().start();
    service::UserModelManager::get().start();

    std::string tts_error;
    if (!app.tts.warmUp(tts_error))
        WLOG_WARN("TTS warmup failed: {}", tts_error);
}

void ProductionProfileRuntime::onDatabaseReady(AppState& app, const db::DbClientPtr& client)
{
    app.bindAutomationDatabase(client);

    // SleepManager 의 job worker 스레드(수면 요약/일간·주간 리포트/오늘 밤 계획 생성)는
    // 실제 레이더 유무와 무관하게 항상 필요하다 - --no-devices 로 기기 스택을 건너뛰어도
    // SleepStore::getTodayPlan()(sleep_store.cpp)의 캐시 미스 폴백이 이 큐에 비동기 생성
    // 요청을 넣는다. tickRuntime()(레이더 폴링)은 start() 내부에서 이미
    // AppState::get().no_devices 를 자체 체크하므로 무조건 start() 해도 안전하다.
    service::SleepManager::get().reconcile();
    service::SleepManager::get().start();
    WLOG_INFO("SleepManager started");
    m_startedSleepManager = true;

    if (app.no_devices)
    {
        app.markDatabaseReady();
        return;
    }

    m_startedDeviceStack = true;

    app.deviceManager.setOnStartupComplete([&app]()
    {
        service::CompanionManager::get().onDevicesReady();
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

    service::CompanionManager::get().reconcile();
    service::CompanionManager::get().start();

    service::AlarmManager::get().start();
    WLOG_INFO("AlarmManager started");

    app.markDatabaseReady();
}

void ProductionProfileRuntime::shutdown(AppState& app)
{
    if (m_startedSleepManager)
        service::SleepManager::get().stop();

    if (m_startedDeviceStack)
    {
        service::PowerManager::get().stop();
        service::UserModelManager::get().stop();
        service::CompanionManager::get().stop();
        service::AlarmManager::get().stop();
        service::AgentJobQueue::get().stop();
    }

    app.stopAutomationServices();
}

WAVE_NAMESPACE_END
