#pragma once

#include "profile_runtime.h"

WAVE_NAMESPACE_BEGIN

class DemoProfileRuntime :
    public IProfileRuntime
{
public:
    ProfileKind kind() const override { return ProfileKind::Demo; }
    const char* name() const override { return "demo"; }

    void applyConfigDefaults(AppConfig& config) const override;
    void startServices(AppState& app) override;
    void startPostListen(AppState& app) override;
    void onDatabaseReady(AppState& app, const db::DbClientPtr& client) override;
    void shutdown(AppState& app) override;

private:
    bool m_startedDemoAutomation = false;
};

WAVE_NAMESPACE_END
