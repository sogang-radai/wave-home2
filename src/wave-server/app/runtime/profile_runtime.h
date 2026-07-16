#pragma once

#include <memory>

#include "../../db/database.h"
#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN

class AppState;
struct AppConfig;

enum class ProfileKind
{
    Production,
    Demo,
    Test,
};

/** Profile-specific startup / shutdown. AppState owns shared infra; runtime owns the rest. */
class IProfileRuntime
{
public:
    virtual ~IProfileRuntime() = default;

    virtual ProfileKind kind() const = 0;
    virtual const char* name() const = 0;

    /** Fill missing server/agent listen defaults from docs/ports.txt. */
    virtual void applyConfigDefaults(AppConfig& config) const = 0;

    /** After web::Server::init, before Server::run (automation stores, etc.). */
    virtual void startServices(AppState& app) = 0;

    /** After Server::run has been kicked off (demo automation, power, TTS, …). */
    virtual void startPostListen(AppState& app) = 0;

    /** Drogon DB client ready. */
    virtual void onDatabaseReady(AppState& app, const db::DbClientPtr& client) = 0;

    /** Tear down only what this profile started. */
    virtual void shutdown(AppState& app) = 0;
};

std::unique_ptr<IProfileRuntime> createProfileRuntime(ProfileKind kind);

WAVE_NAMESPACE_END
