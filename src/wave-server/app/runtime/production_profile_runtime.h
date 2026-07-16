#pragma once

#include "profile_runtime.h"

WAVE_NAMESPACE_BEGIN

class ProductionProfileRuntime :
    public IProfileRuntime
{
public:
    ProfileKind kind() const override { return ProfileKind::Production; }
    const char* name() const override { return "real"; }

    void applyConfigDefaults(AppConfig& config) const override;
    void startServices(AppState& app) override;
    void startPostListen(AppState& app) override;
    void onDatabaseReady(AppState& app, const db::DbClientPtr& client) override;
    void shutdown(AppState& app) override;

private:
    bool m_startedDeviceStack = false;
};

WAVE_NAMESPACE_END
