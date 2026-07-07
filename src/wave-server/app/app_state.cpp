#include "app_state.h"

#include <fstream>

#include <drogon/drogon.h>

#include "../core/logger.h"
#include "../service/power_manager.h"
#include "../web/http/v1/iot_store.h"
#include "util/exe_path.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    static AppState* s_instance = nullptr;
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

std::filesystem::path AppState::resolvePath(const std::string& relative) const
{
    std::filesystem::path path(relative);
    if (path.is_absolute())
        return path;
    if (!config_dir.empty())
        return config_dir / path;
    return std::filesystem::weakly_canonical(std::filesystem::current_path() / path);
}

drogon::orm::DbClientPtr AppState::db() const
{
    if (!m_initialized)
        return nullptr;
    return drogon::app().getDbClient();
}

bool AppState::loadDeviceManifests()
{
    const auto rooms_path = resolvePath(config.rooms_path);
    const auto devices_path = resolvePath(config.device_list_path);

    if (!std::filesystem::exists(rooms_path))
    {
        LOG_WARN("Rooms manifest not found ({}); device manager not loaded", rooms_path.string());
        return false;
    }

    if (!std::filesystem::exists(devices_path))
    {
        LOG_WARN("Device list not found ({}); device manager not loaded", devices_path.string());
        return false;
    }

    try
    {
        json rooms_json;
        {
            std::ifstream in(rooms_path);
            in >> rooms_json;
        }
        json devices_json;
        {
            std::ifstream in(devices_path);
            in >> devices_json;
        }

        if (!deviceManager.load(rooms_json, devices_json))
        {
            LOG_WARN("Device manager load returned false");
            return false;
        }

        LOG_INFO(
            "Device manager loaded ({} rooms, {} devices)",
            deviceManager.enumerateRooms().size(),
            deviceManager.enumerateDevices().size());
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_WARN("Device manager load failed: {}", e.what());
        return false;
    }
}

void AppState::init(const LaunchOptions& launch)
{
    if (m_initialized)
        return;

    test_mode = launch.test_mode;
    no_devices = launch.no_devices || !config.devices_enabled;

    std::filesystem::path resolved_config(launch.config_path);
    if (!resolved_config.is_absolute())
    {
        const auto base_dir = getExecutableDir();
        resolved_config = base_dir.empty()
            ? std::filesystem::weakly_canonical(std::filesystem::current_path() / resolved_config)
            : base_dir / resolved_config;
    }
    config_dir = resolved_config.parent_path();

    if (!AppConfig::loadFromFile(resolved_config, config))
    {
        LOG_ERROR("Failed to load app config: {}", resolved_config.string());
        return;
    }

    if (launch.port)
        config.server["port"] = *launch.port;
    if (launch.document_root)
        config.server["document_root"] = *launch.document_root;
    if (launch.test_mode)
        config.server["test_mode"] = true;

    if (!server.init(config.server, launch.test_mode))
    {
        LOG_ERROR("Web server init failed");
        return;
    }

    if (!launch.test_mode)
    {
        settings.load(resolvePath(config.setting_path).string());
        if (!no_devices)
            loadDeviceManifests();
        else
            LOG_INFO("Devices skipped (--no-devices or devices_enabled=false)");
    }
    else
    {
        LOG_INFO("Test mode: skipping settings, devices, and database");
    }

    server.run();
    running.store(true, std::memory_order_release);

    if (!launch.test_mode && !no_devices)
    {
        deviceManager.startDevicesAsync();
        ws::service::PowerManager::get().start();

        std::string tts_error;
        if (!web::v1::warmUpTtsService(tts_error))
            LOG_WARN("TTS warmup failed: {}", tts_error);
    }

    LOG_INFO(
        "App initialized (config: {}, test_mode: {}, no_devices: {})",
        resolved_config.string(),
        launch.test_mode,
        no_devices);
    m_initialized = true;
}

void AppState::shutdown()
{
    if (!m_initialized)
        return;

    LOG_INFO("Shutting down app...");
    running.store(false, std::memory_order_release);

    deviceManager.shutdown();
    ws::service::PowerManager::get().stop();

    gesturePipelines.clear();
    sleepPipelines.clear();

    web::v1::shutdownBackgroundServices();

    server.shutdown();

    LOG_INFO("App shutdown complete");
    m_initialized = false;
}

WAVE_NAMESPACE_END
