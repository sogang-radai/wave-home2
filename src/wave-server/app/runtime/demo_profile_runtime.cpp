#include "demo_profile_runtime.h"

#include "../../core/json.h"
#include "../../core/logger.h"
#include "../../demo/demo_automation_runtime.h"
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
    LOG_INFO("Devices skipped (demo profile)");
    app.startAutomationServices();
}

void DemoProfileRuntime::startPostListen(AppState& app)
{
    (void)app;
    DemoAutomationRuntime::get().start();
    m_startedDemoAutomation = true;
    LOG_INFO("DemoAutomationRuntime started (demo_mode)");
}

void DemoProfileRuntime::onDatabaseReady(AppState& app, const db::DbClientPtr& client)
{
    app.bindAutomationDatabase(client);
    app.markDatabaseReady();
}

void DemoProfileRuntime::shutdown(AppState& app)
{
    if (m_startedDemoAutomation)
        DemoAutomationRuntime::get().stop();

    app.stopAutomationServices();
}

WAVE_NAMESPACE_END
