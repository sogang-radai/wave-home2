#include "test_profile_runtime.h"

#include "../../core/logger.h"
#include "../app_config.h"
#include "../app_state.h"

WAVE_NAMESPACE_BEGIN

void TestProfileRuntime::applyConfigDefaults(AppConfig& config) const
{
    if (!config.server.contains("port"))
        config.server["port"] = 8520;
}

void TestProfileRuntime::startServices(AppState& app)
{
    (void)app;
    LOG_INFO("Test profile: skipping settings, devices, and database");
}

void TestProfileRuntime::startPostListen(AppState& app)
{
    (void)app;
}

void TestProfileRuntime::onDatabaseReady(AppState& app, const db::DbClientPtr& client)
{
    (void)client;
    app.markDatabaseReady();
}

void TestProfileRuntime::shutdown(AppState& app)
{
    (void)app;
}

WAVE_NAMESPACE_END
