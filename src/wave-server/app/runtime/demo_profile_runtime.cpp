#include "demo_profile_runtime.h"

#include "../../core/json.h"
#include "../../core/logger.h"
#include "../../demo/demo_policy.h"
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

void DemoProfileRuntime::applyConfigDefaults(AppConfig& config) const
{
    ensure_server_field(config.server, "port", 8510);
    ensure_server_field(config.server, "agent_api_port", 8511);
    if (!config.server.contains("agent_api_bind"))
        config.server["agent_api_bind"] = "127.0.0.1";
    if (config.agent.base_url.empty())
        config.agent.base_url = "http://127.0.0.1:8512";
}

void DemoProfileRuntime::startServices(AppState& app)
{
    WLOG_INFO("Devices skipped (demo profile)");
    web::registerDemoPolicy();
    app.startAutomationServices();
}

void DemoProfileRuntime::startPostListen(AppState& app)
{
    std::string stt_error;
    if (!app.stt.warmUp(stt_error))
        WLOG_WARN("STT warmup failed: {}", stt_error);

    m_automation.start();
    m_startedDemoAutomation = true;
    WLOG_INFO("DemoAutomationRuntime started (demo profile)");
}

void DemoProfileRuntime::onDatabaseReady(AppState& app, const db::DbClientPtr& client)
{
    app.bindAutomationDatabase(client);

    // 데모 모드는 실제 레이더 세션이 없어 DemoAutomationRuntime 이 알람·일정·룰을 대신
    // 처리하지만, sleep_plan 생성만은 SleepManager 의 job worker 큐를 그대로 쓴다 -
    // SleepStore::getTodayPlan() 의 캐시 미스 폴백이 데모 모드에서도 이 큐에 생성 요청을
    // 넣기 때문에(sleep_store.cpp), job worker 스레드가 떠 있어야 처리된다. tickRuntime()
    // (레이더 폴링)은 start() 내부에서 AppState::get().no_devices 를 자체 체크해 데모
    // 모드에서는 그냥 아무것도 안 하므로 무조건 start() 해도 안전하다.
    service::SleepManager::get().reconcile();
    service::SleepManager::get().start();
    WLOG_INFO("SleepManager started (demo profile)");
    m_startedSleepManager = true;

    app.markDatabaseReady();
}

void DemoProfileRuntime::shutdown(AppState& app)
{
    if (m_startedSleepManager)
        service::SleepManager::get().stop();

    if (m_startedDemoAutomation)
        m_automation.stop();

    app.stopAutomationServices();
}

WAVE_NAMESPACE_END
