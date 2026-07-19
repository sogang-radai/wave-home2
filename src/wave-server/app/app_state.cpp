#include "app_state.h"

#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include <drogon/drogon.h>

#include "../core/logger.h"
#include "../core/task_queue.h"
#include "../service/go2rtc_service.h"
#include "util/time_util.h"
#include "../device/device.h"
#include "../device/platform/droid_cam.h"
#include "util/exe_path.h"
#include "runtime/profile_runtime.h"

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
            WLOG_ERROR("TTS: TaskQueue init failed");
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
            WLOG_ERROR("TTS: config not found at {}", config_path.string());
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
            WLOG_ERROR("TTS: invalid config {} ({})", config_path.string(), e.what());
            code = "TTS_UNAVAILABLE";
            return nullptr;
        }

        const auto init_rc = m_service->init(base_dir, config_json);
        if (init_rc != tts::SUCCESS)
        {
            WLOG_ERROR(
                "TTS: model init failed (rc={}, base_dir={}, config={})",
                static_cast<int>(init_rc),
                base_dir,
                config_path.string());
            m_service.reset();
            code = "TTS_UNAVAILABLE";
            return nullptr;
        }
        WLOG_INFO("TTS: service ready (base_dir={})", base_dir);
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

#ifdef WAVE_BUILD_TTS
struct STTState::Session
{
    std::string id;
    std::string locale;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Json::Value> events;
    bool closed = false;

    void enqueue(Json::Value event)
    {
        {
            std::lock_guard lock(mutex);
            if (closed)
                return;
            events.push_back(std::move(event));
        }
        cv.notify_one();
    }
};

namespace
{
    std::string make_stt_session_id()
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0, 255);
        std::ostringstream stream;
        for (int i = 0; i < 16; ++i)
            stream << std::hex << std::setw(2) << std::setfill('0') << dist(rng);
        return stream.str();
    }
}
#endif

STTState::STTState(AppState& app) :
    m_app(app)
{
}

bool STTState::warmUp(std::string& error)
{
#ifdef WAVE_BUILD_TTS
    std::string code;
    if (!service(code))
    {
        error = code.empty() ? "STT_UNAVAILABLE" : code;
        return false;
    }
    error.clear();
    return true;
#else
    error = "STT_UNAVAILABLE";
    return false;
#endif
}

bool STTState::isReady() const
{
#ifdef WAVE_BUILD_TTS
    return m_ready.load(std::memory_order_acquire);
#else
    return false;
#endif
}

#ifdef WAVE_BUILD_TTS
stt::Service* STTState::service(std::string& code)
{
    code.clear();
    if (!m_taskQueueReady)
    {
        if (!TaskQueue::get().init())
        {
            code = "STT_UNAVAILABLE";
            WLOG_ERROR("STT: TaskQueue init failed");
            return nullptr;
        }
        m_taskQueueReady = true;
    }

    std::lock_guard lock(m_mutex);
    if (!m_service)
    {
        m_service = std::make_unique<stt::Service>();
        const auto config_path = m_app.resolvePath(m_app.config.stt_model_path);
        const auto base_dir = m_app.config_dir.string();
        std::ifstream in(config_path);
        if (!in)
        {
            WLOG_ERROR("STT: config not found at {}", config_path.string());
            code = "STT_UNAVAILABLE";
            return nullptr;
        }

        json config_json;
        try
        {
            in >> config_json;
        }
        catch (const std::exception& e)
        {
            WLOG_ERROR("STT: invalid config {} ({})", config_path.string(), e.what());
            code = "STT_UNAVAILABLE";
            return nullptr;
        }

        const auto init_rc = m_service->init(base_dir, config_json);
        if (init_rc != stt::SUCCESS)
        {
            WLOG_ERROR(
                "STT: model init failed (rc={}, base_dir={}, config={})",
                static_cast<int>(init_rc),
                base_dir,
                config_path.string());
            m_service.reset();
            code = "STT_UNAVAILABLE";
            return nullptr;
        }
        WLOG_INFO("STT: service ready (base_dir={})", base_dir);
        m_ready.store(true, std::memory_order_release);
    }

    return m_service.get();
}

void STTState::clear_session_locked()
{
    if (!m_activeSession)
        return;

    {
        std::lock_guard session_lock(m_activeSession->mutex);
        m_activeSession->closed = true;
        m_activeSession->events.clear();
    }
    m_activeSession->cv.notify_all();
    m_activeSession.reset();
}

bool STTState::createSession(const std::string& locale, std::string& session_id, std::string& code)
{
    code.clear();
    session_id.clear();

    std::string service_code;
    auto* stt = service(service_code);
    if (!stt)
    {
        code = service_code.empty() ? "STT_UNAVAILABLE" : service_code;
        return false;
    }

    const std::string use_locale = locale.empty() ? "ko-KR" : locale;
    std::lock_guard stream_lock(m_streamMutex);

    {
        std::lock_guard lock(m_mutex);
        if (m_activeSession)
        {
            bool closed = false;
            {
                std::lock_guard session_lock(m_activeSession->mutex);
                closed = m_activeSession->closed;
            }
            if (!closed)
            {
                code = "STT_BUSY";
                return false;
            }
            clear_session_locked();
        }
    }

    auto session = std::make_shared<Session>();
    session->id = make_stt_session_id();
    session->locale = use_locale;

    const auto begin_rc = stt->beginRecognizeStream(
        use_locale,
        [session](const stt::RecognizeResult& result)
        {
            Json::Value event(Json::objectValue);
            event["type"] = "partial";
            event["text"] = result.text;
            event["isEndpoint"] = result.isEndpoint;
            session->enqueue(std::move(event));
        });
    if (begin_rc != stt::SUCCESS)
    {
        code = begin_rc == stt::ERROR_INVALID_LOCALE ? "INVALID_LOCALE"
            : begin_rc == stt::ERROR_INVALID_INPUT ? "STT_BUSY"
            : "STT_UNAVAILABLE";
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_activeSession = session;
    }
    session_id = session->id;
    return true;
}

bool STTState::pushAudio(
    const std::string& session_id,
    const float* samples,
    size_t sample_count,
    uint32_t sample_rate,
    std::string& code)
{
    code.clear();
    if (!samples || sample_count == 0)
    {
        code = "INVALID_AUDIO";
        return false;
    }

    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(m_mutex);
        if (!m_activeSession || m_activeSession->id != session_id)
        {
            code = "NOT_FOUND";
            return false;
        }
        {
            std::lock_guard session_lock(m_activeSession->mutex);
            if (m_activeSession->closed)
            {
                code = "NOT_FOUND";
                return false;
            }
        }
        session = m_activeSession;
    }

    std::string service_code;
    auto* stt = service(service_code);
    if (!stt)
    {
        code = service_code.empty() ? "STT_UNAVAILABLE" : service_code;
        return false;
    }

    stt::AudioInput input;
    input.samples = samples;
    input.sampleCount = sample_count;
    input.sampleRate = sample_rate == 0 ? 16000 : sample_rate;

    std::lock_guard stream_lock(m_streamMutex);
    const auto push_rc = stt->pushAudio(session->locale, input);
    if (push_rc != stt::SUCCESS)
    {
        code = push_rc == stt::ERROR_INVALID_INPUT ? "INVALID_AUDIO" : "STT_UNAVAILABLE";
        return false;
    }
    return true;
}

bool STTState::endSession(const std::string& session_id, std::string& code)
{
    code.clear();
    std::shared_ptr<Session> session;
    std::string locale;
    {
        std::lock_guard lock(m_mutex);
        if (!m_activeSession || m_activeSession->id != session_id)
        {
            code = "NOT_FOUND";
            return false;
        }
        session = m_activeSession;
        locale = session->locale;
    }

    std::string service_code;
    auto* stt = service(service_code);
    if (stt)
    {
        std::lock_guard stream_lock(m_streamMutex);
        stt->endRecognizeStream(locale);
    }

    Json::Value done(Json::objectValue);
    done["type"] = "done";
    session->enqueue(std::move(done));
    {
        std::lock_guard session_lock(session->mutex);
        session->closed = true;
    }
    session->cv.notify_all();
    return true;
}

bool STTState::abortSession(const std::string& session_id, std::string& code)
{
    code.clear();
    std::shared_ptr<Session> session;
    std::string locale;
    {
        std::lock_guard lock(m_mutex);
        if (!m_activeSession || m_activeSession->id != session_id)
        {
            code = "NOT_FOUND";
            return false;
        }
        session = m_activeSession;
        locale = session->locale;
    }

    std::string service_code;
    auto* stt = service(service_code);
    if (stt)
    {
        std::lock_guard stream_lock(m_streamMutex);
        stt->endRecognizeStream(locale);
    }

    {
        std::lock_guard session_lock(session->mutex);
        session->closed = true;
        session->events.clear();
    }
    session->cv.notify_all();

    {
        std::lock_guard lock(m_mutex);
        if (m_activeSession && m_activeSession->id == session_id)
            m_activeSession.reset();
    }
    return true;
}

bool STTState::popEvent(
    const std::string& session_id,
    Json::Value& out_event,
    bool& session_closed,
    std::chrono::milliseconds timeout,
    std::string& code)
{
    code.clear();
    session_closed = false;
    out_event = Json::Value();

    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(m_mutex);
        if (!m_activeSession || m_activeSession->id != session_id)
        {
            // Ended sessions may already be cleared after abort; treat as closed.
            session_closed = true;
            return true;
        }
        session = m_activeSession;
    }

    std::unique_lock session_lock(session->mutex);
    if (session->events.empty() && !session->closed)
        session->cv.wait_for(session_lock, timeout);

    if (!session->events.empty())
    {
        out_event = std::move(session->events.front());
        session->events.pop_front();
        if (out_event.isMember("type") && out_event["type"].asString() == "done")
            session_closed = true;
        return true;
    }

    session_closed = session->closed;
    return true;
}
#endif

void STTState::shutdown()
{
#ifdef WAVE_BUILD_TTS
    {
        std::lock_guard stream_lock(m_streamMutex);
        if (m_service && m_activeSession)
            m_service->endRecognizeStream(m_activeSession->locale);
    }
    m_ready.store(false, std::memory_order_release);
    std::lock_guard lock(m_mutex);
    clear_session_locked();
    m_service.reset();
#endif
}

IotRuntime::IotRuntime(AppState& app) :
    m_app(app)
{
}

std::string IotRuntime::iso_now_kst()
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
    event["occurredAt"] = iso_now_kst();
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
    stt(*this),
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

db::DbClientPtr AppState::db() const
{
    if (test_mode || !m_dbReady.load(std::memory_order_acquire))
        return nullptr;
    return drogon::app().getDbClient();
}

bool AppState::loadDeviceManifests(const db::DbClientPtr& client)
{
    const auto devices_path = resolvePath(config.device_list_path);

    if (!client)
    {
        WLOG_WARN("Database client unavailable; device manager not loaded");
        return false;
    }

    if (!std::filesystem::exists(devices_path))
    {
        WLOG_WARN("Device list not found ({}); device manager not loaded", devices_path.string());
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
            WLOG_WARN("No rooms in database; device manager not loaded");
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
            WLOG_WARN("Device manager load returned false");
            return false;
        }

        WLOG_INFO(
            "Device manager loaded ({} rooms from DB, {} devices)",
            deviceManager.enumerateRooms().size(),
            deviceManager.enumerateDevices().size());
        return true;
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("Device manager load failed: {}", e.what());
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
        WLOG_WARN("IrStore load failed: {}", ir_error);

    std::string gesture_error;
    if (!m_gestureStore->load(
            resolvePath(config.gesture_sets_path),
            [this](const std::string& relative) { return resolvePath(relative); },
            gesture_error))
    {
        WLOG_WARN("GestureStore load failed: {}", gesture_error);
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

void AppState::onDatabaseReady(const db::DbClientPtr& client)
{
    if (!m_runtime)
        return;
    m_runtime->onDatabaseReady(*this, client);
}

void AppState::bindAutomationDatabase(const db::DbClientPtr& client)
{
    if (!m_ruleStore || !client)
        return;

    m_ruleStore->setDatabaseClient(client);

    if (m_gestureStore)
    {
        m_gestureStore->setDatabaseClient(client);
        std::string gesture_db_error;
        if (!m_gestureStore->syncFromDatabase(gesture_db_error, config.db_read_only))
            WLOG_WARN("GestureStore DB sync failed: {}", gesture_db_error);
        else if (config.db_read_only)
            WLOG_INFO("GestureStore loaded device mappings (read-only DB)");
    }

    std::string rules_error;
    if (!m_ruleStore->loadFromDatabase(rules_error))
        WLOG_WARN("RuleStore load failed: {}", rules_error);
    else
        WLOG_INFO("RuleStore loaded from automation_rule");
}

void AppState::markDatabaseReady()
{
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
    WLOG_INFO("TriggerManager started");
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
    m_irStore.reset();
    m_triggerRuntimeStarted = false;
}

IProfileRuntime& AppState::runtime()
{
    assert(m_runtime != nullptr);
    return *m_runtime;
}

const IProfileRuntime& AppState::runtime() const
{
    assert(m_runtime != nullptr);
    return *m_runtime;
}

void AppState::init(const LaunchOptions& launch)
{
    if (m_initialized)
        return;

    test_mode = launch.profile == "test";
    demo_mode = launch.profile == "demo";

    const ProfileKind kind = test_mode
        ? ProfileKind::Test
        : (demo_mode ? ProfileKind::Demo : ProfileKind::Production);
    m_runtime = createProfileRuntime(kind);

    std::filesystem::path resolved_config(launch.config_path);
    if (!resolved_config.is_absolute())
    {
        const auto base_dir = getExecutableDir();
        resolved_config = base_dir.empty()
            ? std::filesystem::weakly_canonical(std::filesystem::current_path() / resolved_config)
            : base_dir / resolved_config;
    }
    config_dir = resolved_config.parent_path();

    if (!AppConfig::load_from_file(resolved_config, launch.profile, config))
    {
        WLOG_ERROR("Failed to load app config: {}", resolved_config.string());
        return;
    }

    m_runtime->applyConfigDefaults(config);

    no_devices = demo_mode || launch.no_devices || !config.devices_enabled;
    anchor_date = config.anchor_date;

    if (launch.port)
        config.server["port"] = *launch.port;
    if (launch.document_root)
        config.server["document_root"] = *launch.document_root;

    if (!server.init(config.server, test_mode, demo_mode))
    {
        WLOG_ERROR("Web server init failed");
        return;
    }

    m_runtime->startServices(*this);

    server.run();
    running.store(true, std::memory_order_release);

    m_runtime->startPostListen(*this);

    WLOG_INFO(
        "App initialized (config: {}, profile: {}, test_mode: {}, demo_mode: {}, no_devices: {}, anchor_date: {})",
        resolved_config.string(),
        m_runtime->name(),
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

    WLOG_INFO("Shutting down app...");
    running.store(false, std::memory_order_release);
    m_dbReady.store(false, std::memory_order_release);

    if (m_runtime)
        m_runtime->shutdown(*this);

    deviceManager.shutdown();

    iot.shutdown();
    stt.shutdown();
    tts.shutdown();
    service::Go2RtcService::get().shutdownAll();

    server.shutdown();
    m_runtime.reset();

    WLOG_INFO("App shutdown complete");
    m_initialized = false;
}

WAVE_NAMESPACE_END
