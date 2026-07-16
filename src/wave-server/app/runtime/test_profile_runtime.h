#pragma once

#include "profile_runtime.h"

WAVE_NAMESPACE_BEGIN

class TestProfileRuntime :
    public IProfileRuntime
{
public:
    ProfileKind kind() const override { return ProfileKind::Test; }
    const char* name() const override { return "test"; }

    void applyConfigDefaults(AppConfig& config) const override;
    void startServices(AppState& app) override;
    void startPostListen(AppState& app) override;
    void onDatabaseReady(AppState& app, const db::DbClientPtr& client) override;
    void shutdown(AppState& app) override;
};

WAVE_NAMESPACE_END
