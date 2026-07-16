#include "app_state.h"

#include <fstream>

#include <drogon/drogon.h>

#include "../core/logger.h"
#include "../core/task_queue.h"
#include "../service/power_manager.h"
#include "../service/sleep/sleep_manager.h"
#include "../service/alarm_manager.h"
#include "../service/go2rtc_service.h"
#include "../core/time_util.h"
#include "../demo/demo_automation_runtime.h"
#include "../device/device.h"
#include "../device/platform/droid_cam.h"
#include "util/exe_path.h"

#ifdef WAVE_BUILD_TTS
#include "../core/json.h"
#endif

WAVE_NAMESPACE_BEGIN

namespace
{
    static AppState* s_instance = nullptr;
}

TTSState::TTSState(AppState& app) :
    m_app(app)
{
}

bool TTSState::warmUp(std::string& error)
{
#ifdef WAVE_BUILD_TTS
    std::string code;
    if (!service(code))
    {
        error = code.empty() ? "TTS_UNAVAILABLE" : code;
        return false;
    }
    error.clear();
    return true;
#else
    error = "TTS_UNAVAILABLE";
    return false;
#endif
}

bool TTSState::isReady() const
{
#ifdef WAVE_BUILD_TTS
    return m_ready.load(std::memory_order_acquire);
#else
    return false;
#endif
}

#ifdef WAVE_BUILD_TTS
tts::Service* TTSState::service(std::string& code)
{
    code.clear();
    if (!m_taskQueueReady)
    {
        if (!TaskQueue::get().init())
        {
            code = "TTS_UNAVAILABLE";
            LOG_ERROR("TTS: TaskQueue init failed");
            return nullptr;
        }
        m_taskQueueReady = true;
    }

    std::lock_guard lock(m_mutex);
    if (!m_service)
    {
        m_service = std::make_unique<tts::Service>();
        const auto config_path = m_app.resolvePath(m_app.config.tts_model_path);
        const auto base_dir = m_app.config_dir.string();
        std::ifstream in(config_path);
        if (!in)
        {
            LOG_ERROR("TTS: config not found at {}", config_path.string());
            code = "TTS_UNAVAILABLE";
            return nullptr;
        }

        json config_json;
        try
        {
            in >> config_json;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("TTS: invalid config {} ({})", config_path.string(), e.what());
            code = "TTS_UNAVAILABLE";
            return nullptr;
        }

        const auto init_rc = m_service->init(base_dir, config_json);
        if (init_rc != tts::SUCCESS)
        {
            LOG_ERROR(
                "TTS: model init failed (rc={}, base_dir={}, config={})",
                static_cast<int>(init_rc),
                base_dir,
                config_path.string());
            m_service.reset();
            code = "TTS_UNAVAILABLE";
            return nullptr;
        }
        LOG_INFO("TTS: service ready (base_dir={})", base_dir);
        m_ready.store(true, std::memory_order_release);
    }

    return m_service.get();
}

std::mutex& TTSState::generateMutex()
{
    return m_generateMutex;
}
#endif

void TTSState::shutdown()
{
#ifdef WAVE_BUILD_TTS
    m_ready.store(false, std::memory_order_release);
    std::lock_guard lock(m_mutex);
    m_service.reset();
#endif
}

IotRuntime::IotRuntime(AppState& app) :
    m_app(app)
{
}

std::string IotRuntime::isoNowKst()
{
    const auto now = formatTimestamp();
    if (now.size() >= 19)
        return now.substr(0, 10) + "T" + now.substr(11, 8) + "+09:00";
    return now;
}

void IotRuntime::logEvent(
    const std::string& type,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& message,
    const std::string& triggered_by,
    const Json::Value& detail)
{
    std::lock_guard lock(m_eventMutex);
    Json::Value event;
    event["id"] = static_cast<Json::Int64>(m_nextEventId++);
    event["type"] = type;
    event["occurredAt"] = isoNowKst();
    if (!device_id.empty())
        event["deviceId"] = device_id;
    event["deviceName"] = device_name;
    event["message"] = message;
    if (!triggered_by.empty())
        event["triggeredBy"] = triggered_by;
    event["detail"] = detail;
    m_events.insert(m_events.begin(), event);
    if (m_events.size() > 300)
        m_events.resize(300);
}

Json::Value IotRuntime::listEvents(const std::string& device_id) const
{
    Json::Value body(Json::arrayValue);
    std::lock_guard lock(m_eventMutex);
    for (const auto& event : m_events)
    {
        if (!device_id.empty() && event.isMember("deviceId") && event["deviceId"].asString() != device_id)
            continue;
        body.append(event);
    }
    return body;
}

int IotRuntime::eventCount() const
{
    std::lock_guard lock(m_eventMutex);
    return static_cast<int>(m_events.size());
}

int IotRuntime::streamViewers(const std::string& external_id) const
{
    std::lock_guard lock(m_cameraStreamMutex);
    const auto it = m_cameraStreamViewers.find(external_id);
    return it != m_cameraStreamViewers.end() ? it->second : 0;
}

bool IotRuntime::changeStreamViewers(
    const std::string& external_id,
    bool streaming,
    const std::function<bool()>& on_first_viewer,
    const std::function<void()>& on_last_viewer)
{
    std::lock_guard lock(m_cameraStreamMutex);
    int& viewers = m_cameraStreamViewers[external_id];
    if (streaming)
    {
        ++viewers;
        if (viewers == 1 && on_first_viewer && !on_first_viewer())
        {
            --viewers;
            if (viewers <= 0)
                m_cameraStreamViewers.erase(external_id);
            return false;
        }
        return true;
    }

    if (viewers > 0)
    {
        --viewers;
        if (viewers <= 0)
        {
            m_cameraStreamViewers.erase(external_id);
            if (on_last_viewer)
                on_last_viewer();
        }
    }
    return true;
}

void IotRuntime::resetCameraStreamSession(const std::string& external_id)
{
    {
        std::lock_guard lock(m_cameraStreamMutex);
        m_cameraStreamViewers.erase(external_id);
        m_cameraZoomLevels.erase(external_id);
    }
    stopDroidMjpegProxy(external_id);
}

int IotRuntime::adjustZoom(const std::string& external_id, int delta)
{
    std::lock_guard lock(m_cameraStreamMutex);
    int zoom = m_cameraZoomLevels[external_id];
    zoom = std::max(0, std::min(100, zoom + delta));
    m_cameraZoomLevels[external_id] = zoom;
    return zoom;
}

std::shared_ptr<std::atomic<bool>> IotRuntime::beginDroidMjpegProxy(const std::string& external_id)
{
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard lock(m_droidMjpegMutex);
    m_droidMjpegStop[external_id] = flag;
    return flag;
}

void IotRuntime::stopDroidMjpegProxy(const std::string& external_id)
{
    std::lock_guard lock(m_droidMjpegMutex);
    const auto it = m_droidMjpegStop.find(external_id);
    if (it != m_droidMjpegStop.end())
        it->second->store(true, std::memory_order_release);
}

void IotRuntime::finishDroidMjpegProxy(const std::string& external_id, bool phone_lost)
{
    {
        std::lock_guard lock(m_droidMjpegMutex);
        m_droidMjpegStop.erase(external_id);
    }

    if (!phone_lost)
        return;

    const auto id = dev::parseDeviceID(external_id);
    if (id == 0)
        return;
    if (auto* device = m_app.deviceManager.findDevice(id))
        if (auto* droid = dynamic_cast<dev::DroidCam*>(device))
            droid->markPhoneOffline();
}

void IotRuntime::shutdown()
{
    {
        std::lock_guard lock(m_cameraStreamMutex);
        m_cameraStreamViewers.clear();
        m_cameraZoomLevels.clear();
    }
    {
        std::lock_guard lock(m_droidMjpegMutex);
        for (auto& [device_id, flag] : m_droidMjpegStop)
        {
            (void)device_id;
            if (flag)
                flag->store(true, std::memory_order_release);
        }
        m_droidMjpegStop.clear();
    }
}

AppState& AppState::get()
{
    assert(s_instance != nullptr);
    return *s_instance;
}

AppState::AppState() :
    tts(*this),
    iot(*this)
{
    assert(s_instance == nullptr);
    s_instance = this;
}

AppState::~AppState()
{
    shutdown();
    s_instance = nullptr;
}

service::ActionQueue& AppState::actionQueue()
{
    assert(m_actionQueue != nullptr);
    return *m_actionQueue;
}

service::RuleStore& AppState::ruleStore()
{
    assert(m_ruleStore != nullptr);
    return *m_ruleStore;
}

service::TriggerManager& AppState::triggerManager()
{
    assert(m_triggerManager != nullptr);
    return *m_triggerManager;
}

web::v1::GestureStore& AppState::gestureStore()
{
    assert(m_gestureStore != nullptr);
    return *m_gestureStore;
}

web::v1::IrStore& AppState::irStore()
{
    assert(m_irStore != nullptr);
    return *m_irStore;
}

bool AppState::hasGestureStore() const
{
    return m_gestureStore != nullptr;
}

bool AppState::hasIrStore() const
{
    return m_irStore != nullptr;
}

bool AppState::automationReady() const
{
    return m_ruleStore != nullptr && m_actionQueue != nullptr && m_triggerManager != nullptr;
}

bool AppState::hasRuleStore() const
{
    return m_ruleStore != nullptr;
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
    if (test_mode || !m_dbReady.load(std::memory_order_acquire))
        return nullptr;
    return drogon::app().getDbClient();
}

bool AppState::loadDeviceManifests(const drogon::orm::DbClientPtr& client)
{
    const auto devices_path = resolvePath(config.device_list_path);

    if (!client)
    {
        LOG_WARN("Database client unavailable; device manager not loaded");
        return false;
    }

    if (!std::filesystem::exists(devices_path))
    {
        LOG_WARN("Device list not found ({}); device manager not loaded", devices_path.string());
        return false;
    }

    try
    {
        json devices_json;
        {
            std::ifstream in(devices_path);
            in >> devices_json;
        }

        json rooms_json;
        rooms_json["rooms"] = json::array();
        auto rows = client->execSqlSync("SELECT id, name, description FROM room ORDER BY id");
        if (rows.empty())
        {
            LOG_WARN("No rooms in database; device manager not loaded");
            return false;
        }

        for (const auto& row : rows)
        {
            json room;
            room["id"] = dev::roomIDToString(static_cast<dev::RoomID>(row["id"].as<int64_t>()));
            room["name"] = row["name"].as<std::string>();
            room["description"] = row["description"].as<std::string>();
            rooms_json["rooms"].push_back(std::move(room));
        }

        if (!deviceManager.load(rooms_json, devices_json))
        {
            LOG_WARN("Device manager load returned false");
            return false;
        }

        LOG_INFO(
            "Device manager loaded ({} rooms from DB, {} devices)",
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

void AppState::startAutomationServices()
{
    m_ruleStore = std::make_unique<service::RuleStore>();
    m_gestureStore = std::make_unique<web::v1::GestureStore>();
    m_irStore = std::make_unique<web::v1::IrStore>();

    std::string ir_error;
    if (!m_irStore->load(resolvePath("device/ir_list.json"), ir_error))
        LOG_WARN("IrStore load failed: {}", ir_error);

    std::string gesture_error;
    if (!m_gestureStore->load(
            resolvePath(config.gesture_sets_path),
            [this](const std::string& relative) { return resolvePath(relative); },
            gesture_error))
    {
        LOG_WARN("GestureStore load failed: {}", gesture_error);
    }

    if (no_devices)
        return;

    m_actionQueue = std::make_unique<service::ActionQueue>();
    m_triggerManager = std::make_unique<service::TriggerManager>();

    m_actionQueue->start(deviceManager);

    m_ruleStore->setOnChanged([this]()
    {
        if (m_triggerManager)
            m_triggerManager->reconcile();
    });
}

void AppState::onDatabaseReady(const drogon::orm::DbClientPtr& client)
{
    if (!m_ruleStore || !client)
        return;

    m_ruleStore->setDatabaseClient(client);

    if (m_gestureStore)
    {
        m_gestureStore->setDatabaseClient(client);
        std::string gesture_db_error;
        if (!m_gestureStore->syncFromDatabase(gesture_db_error, config.db_read_only))
            LOG_WARN("GestureStore DB sync failed: {}", gesture_db_error);
        else if (config.db_read_only)
            LOG_INFO("GestureStore loaded device mappings (read-only DB)");
    }

    std::string rules_error;
    if (!m_ruleStore->loadFromDatabase(rules_error))
        LOG_WARN("RuleStore load failed: {}", rules_error);
    else
        LOG_INFO("RuleStore loaded from automation_rule");

    startTriggerRuntime();

    if (!no_devices)
    {
        if (!deviceManager.manifestLoaded())
        {
            if (!loadDeviceManifests(client))
                LOG_WARN("Device manager load failed");
            else
                deviceManager.startDevicesAsync();
        }

        service::SleepManager::get().reconcile();
        service::SleepManager::get().start();
        LOG_INFO("SleepManager started");

        service::AlarmManager::get().start();
        LOG_INFO("AlarmManager started");
    }

    m_dbReady.store(true, std::memory_order_release);
}

void AppState::startTriggerRuntime()
{
    if (m_triggerRuntimeStarted || no_devices || !m_ruleStore || !m_actionQueue || !m_triggerManager)
        return;

    m_triggerManager->start(
        *m_ruleStore,
        *m_actionQueue,
        deviceManager,
        [this](const std::string& relative) { return resolvePath(relative); });

    m_triggerRuntimeStarted = true;
    LOG_INFO("TriggerManager started");
}

void AppState::stopAutomationServices()
{
    if (m_triggerManager)
        m_triggerManager->stop();
    if (m_actionQueue)
        m_actionQueue->stop();

    m_triggerManager.reset();
    m_actionQueue.reset();
    m_ruleStore.reset();
    m_gestureStore.reset();
    m_triggerRuntimeStarted = false;
}

void AppState::init(const LaunchOptions& launch)
{
    if (m_initialized)
        return;

    test_mode = launch.profile == "test";
    demo_mode = launch.profile == "demo";

    std::filesystem::path resolved_config(launch.config_path);
    if (!resolved_config.is_absolute())
    {
        const auto base_dir = getExecutableDir();
        resolved_config = base_dir.empty()
            ? std::filesystem::weakly_canonical(std::filesystem::current_path() / resolved_config)
            : base_dir / resolved_config;
    }
    config_dir = resolved_config.parent_path();

    if (!AppConfig::loadFromFile(resolved_config, launch.profile, config))
    {
        LOG_ERROR("Failed to load app config: {}", resolved_config.string());
        return;
    }

    no_devices = demo_mode || launch.no_devices || !config.devices_enabled;
    anchor_date = config.anchor_date;

    if (launch.port)
        config.server["port"] = *launch.port;
    if (launch.document_root)
        config.server["document_root"] = *launch.document_root;

    if (!server.init(config.server, test_mode, demo_mode))
    {
        LOG_ERROR("Web server init failed");
        return;
    }

    if (!test_mode)
    {
        if (no_devices && demo_mode)
            LOG_INFO("Devices skipped (demo profile)");
        else if (no_devices)
            LOG_INFO("Devices skipped (--no-devices or devices_enabled=false)");

        startAutomationServices();
    }
    else
    {
        LOG_INFO("Test profile: skipping settings, devices, and database");
    }

    server.run();
    running.store(true, std::memory_order_release);

    if (demo_mode)
    {
        DemoAutomationRuntime::get().start();
        LOG_INFO("DemoAutomationRuntime started (demo_mode)");
    }

    if (!test_mode && !no_devices)
    {
        ws::service::PowerManager::get().start();

        std::string tts_error;
        if (!tts.warmUp(tts_error))
            LOG_WARN("TTS warmup failed: {}", tts_error);
    }

    LOG_INFO(
        "App initialized (config: {}, profile: {}, test_mode: {}, demo_mode: {}, no_devices: {}, anchor_date: {})",
        resolved_config.string(),
        launch.profile,
        test_mode,
        demo_mode,
        no_devices,
        anchor_date.empty() ? "(none)" : anchor_date);
    m_initialized = true;
}

void AppState::shutdown()
{
    if (!m_initialized)
        return;

    LOG_INFO("Shutting down app...");
    running.store(false, std::memory_order_release);
    m_dbReady.store(false, std::memory_order_release);

    DemoAutomationRuntime::get().stop();
    ws::service::PowerManager::get().stop();
    service::SleepManager::get().stop();
    service::AlarmManager::get().stop();
    stopAutomationServices();
    deviceManager.shutdown();

    iot.shutdown();
    tts.shutdown();
    service::Go2RtcService::get().shutdownAll();

    server.shutdown();

    LOG_INFO("App shutdown complete");
    m_initialized = false;
}

WAVE_NAMESPACE_END
