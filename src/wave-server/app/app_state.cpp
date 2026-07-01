#include "app_state.h"

#include <fstream>
#include <filesystem>

#include "../core/json.h"
#include "../core/logger.h"
#include "util/exe_path.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    static AppState* s_instance = nullptr;

    std::filesystem::path resolveConfigPath(std::string_view config_path)
    {
        std::filesystem::path path(config_path);
        if (path.is_absolute())
            return path;

        const auto base_dir = getExecutableDir();
        if (!base_dir.empty())
            return base_dir / path;

        return path;
    }
}

AppState& AppState::get()
{
    assert(s_instance != nullptr);
    return *s_instance;
}

AppState::AppState()
{
    assert(s_instance == nullptr);
    s_instance = this;
}

AppState::~AppState()
{
    shutdown();
    s_instance = nullptr;
}

void AppState::init(std::string_view config_path)
{
    if (m_initialized)
        return;

    const auto resolved_path = resolveConfigPath(config_path);
    std::ifstream in(resolved_path);
    if (!in.is_open())
    {
        LOG_ERROR("Failed to open config file: {}", resolved_path.string());
        return;
    }

    json root;
    try
    {
        in >> root;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to parse config file {}: {}", resolved_path.string(), e.what());
        return;
    }

    if (!root.contains("server") || !root["server"].is_object())
    {
        LOG_ERROR("Config file is missing \"server\" object: {}", resolved_path.string());
        return;
    }

    if (!server.init(root["server"]))
    {
        LOG_ERROR("Web server init failed");
        return;
    }

    server.run();
    running.store(true, std::memory_order_release);

    LOG_INFO("App initialized (config: {})", resolved_path.string());
    m_initialized = true;
}

void AppState::shutdown()
{
    if (!m_initialized)
        return;

    LOG_INFO("Shutting down app...");
    running.store(false, std::memory_order_release);

    server.shutdown();

    LOG_INFO("App shutdown complete");
    m_initialized = false;
}

WAVE_NAMESPACE_END
