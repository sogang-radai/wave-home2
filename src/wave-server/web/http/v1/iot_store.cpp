#include "iot_store.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <atomic>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../device/interface/camera.h"
#include "../../../device/interface/ptz.h"
#include "../../../service/go2rtc_service.h"
#ifdef WAVE_BUILD_TTS
#include "../../../core/task_queue.h"
#include "../../../service/tts_service.h"
#include <sherpa-onnx/c-api/cxx-api.h>
#endif

#include "../../../device/platform/radai_ws.h"
#include "../../../device/platform/reolink_e1pro.h"
#include "../../../device/platform/samsung_tizen.h"
#include "../../../device/platform/tuya_ep2h.h"
#include "../../../service/power_manager.h"
#include "session_store.h"
#include "../../../core/logger.h"
#include "../../../core/time_util.h"
#include "../../../device/device.h"
#include "../../../device/room.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {
namespace
{
    std::mutex g_event_mutex;
    std::vector<Json::Value> g_events;
    int64_t g_next_event_id = 1;

    std::mutex g_camera_stream_mutex;
    std::unordered_map<std::string, int> g_camera_stream_viewers;
    std::unordered_map<std::string, int> g_camera_zoom_levels;

#ifdef WAVE_BUILD_TTS
    std::mutex g_tts_mutex;
    std::mutex g_tts_generate_mutex;
    std::unique_ptr<tts::Service> g_tts_service;
    bool g_tts_task_queue_ready = false;
    std::atomic<bool> g_tts_ready{false};

    bool ensureTtsService(tts::Service*& out_service, std::string& code)
    {
        out_service = nullptr;
        if (!g_tts_task_queue_ready)
        {
            if (!TaskQueue::get().init())
            {
                code = "TTS_UNAVAILABLE";
                LOG_ERROR("TTS: TaskQueue init failed");
                return false;
            }
            g_tts_task_queue_ready = true;
        }

        std::lock_guard lock(g_tts_mutex);
        if (!g_tts_service)
        {
            g_tts_service = std::make_unique<tts::Service>();
            const auto& state = AppState::get();
            const auto config_path = state.resolvePath(state.config.tts_model_path);
            const auto base_dir = state.config_dir.string();
            std::ifstream in(config_path);
            if (!in)
            {
                LOG_ERROR("TTS: config not found at {}", config_path.string());
                code = "TTS_UNAVAILABLE";
                return false;
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
                return false;
            }

            const auto init_rc = g_tts_service->init(base_dir, config_json);
            if (init_rc != tts::SUCCESS)
            {
                LOG_ERROR(
                    "TTS: model init failed (rc={}, base_dir={}, config={})",
                    static_cast<int>(init_rc),
                    base_dir,
                    config_path.string());
                g_tts_service.reset();
                code = "TTS_UNAVAILABLE";
                return false;
            }
            LOG_INFO("TTS: service ready (base_dir={})", base_dir);
            g_tts_ready.store(true, std::memory_order_release);
        }

        out_service = g_tts_service.get();
        return true;
    }

    bool write_wav_file(const std::string& path, const std::vector<float>& samples, int32_t sample_rate)
    {
        sherpa_onnx::cxx::Wave wave;
        wave.samples = samples;
        wave.sample_rate = sample_rate;
        return sherpa_onnx::cxx::WriteWave(path, wave);
    }
#endif

    std::string isoNowKst()
    {
        const auto now = formatTimestamp();
        if (now.size() >= 19)
            return now.substr(0, 10) + "T" + now.substr(11, 8) + "+09:00";
        return now;
    }

    dev::Queryable* asQueryable(dev::Device* device)
    {
        return dynamic_cast<dev::Queryable*>(device);
    }

    dev::Actionable* asActionable(dev::Device* device)
    {
        return dynamic_cast<dev::Actionable*>(device);
    }

    bool isManifestInitializing(const dev::DeviceManager& devices, const std::string& external_id)
    {
        for (const auto& entry : devices.manifestEntries())
        {
            if (entry.config.value("id", "") != external_id)
                continue;
            return entry.state == dev::DeviceEntryState::Pending
                || entry.state == dev::DeviceEntryState::Initializing;
        }
        return false;
    }
}

Json::Value IotStore::toJsonValue(const nlohmann::json& value)
{
    Json::CharReaderBuilder builder;
    std::string errors;
    Json::Value out;
    std::istringstream stream(value.dump());
    if (!Json::parseFromStream(builder, stream, &out, &errors))
        return Json::Value(Json::objectValue);
    return out;
}

bool IotStore::isQueryError(const nlohmann::json& value)
{
    return value.is_object() && value.contains("code") && value["code"].is_number_integer()
        && value["code"].get<int>() < 0;
}

void logIotEvent(
    const std::string& type,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& message,
    const std::string& triggered_by,
    const Json::Value& detail)
{
    std::lock_guard lock(g_event_mutex);
    Json::Value event;
    event["id"] = static_cast<Json::Int64>(g_next_event_id++);
    event["type"] = type;
    event["occurredAt"] = isoNowKst();
    if (!device_id.empty())
        event["deviceId"] = device_id;
    event["deviceName"] = device_name;
    event["message"] = message;
    if (!triggered_by.empty())
        event["triggeredBy"] = triggered_by;
    event["detail"] = detail;
    g_events.insert(g_events.begin(), event);
    if (g_events.size() > 300)
        g_events.resize(300);
}

IotStore::IotStore(dev::DeviceManager& devices) :
    m_devices(devices)
{
}

bool IotStore::devicesAvailable() const
{
    return m_devices.manifestLoaded();
}

dev::Device* IotStore::findDevice(const std::string& external_id) const
{
    const auto id = dev::parseDeviceID(external_id);
    if (id == 0)
        return nullptr;
    return m_devices.findDevice(id);
}

bool IotStore::isConnected(const dev::Device* device) const
{
    if (!device || !device->isEnabled() || device->getState() != dev::DeviceState::Running)
        return false;

    const auto class_name = std::string(device->getClass());
    if (class_name == dev::RadaiWs::kClass)
    {
        const auto* ws = dynamic_cast<const dev::RadaiWs*>(device);
        return ws && ws->isLinkConnected();
    }

    if (class_name == "reolink_e1_pro")
    {
        // Avoid a blocking TCP probe on every /iot/devices poll; init/retry
        // already tracks reachability and PTZ uses its own ONVIF path.
        return dynamic_cast<const dev::ReolinkE1Pro*>(device) != nullptr;
    }

    if (class_name == "samsung_g7" || class_name == "tizen_tv")
    {
        const auto* tv = dynamic_cast<const dev::SamsungTizen*>(device);
        return tv && tv->isApiReachable();
    }

    return true;
}

std::string IotStore::connectionStatusForEntry(const dev::DeviceManifestEntry& entry) const
{
    return connectionStatus(entry);
}

std::string IotStore::connectionStatus(const dev::DeviceManifestEntry& entry) const
{
    if (entry.state == dev::DeviceEntryState::Unsupported)
        return "missing";

    if (entry.state == dev::DeviceEntryState::Failed)
        return "offline";

    if (entry.state == dev::DeviceEntryState::Pending || entry.state == dev::DeviceEntryState::Initializing)
        return "initializing";

    const auto external_id = entry.config.value("id", "");
    auto* device = findDevice(external_id);
    if (!device)
        return entry.state == dev::DeviceEntryState::Disabled ? "offline" : "missing";

    if (!device->isEnabled())
        return "offline";

    switch (device->getState())
    {
    case dev::DeviceState::Initializing:
        return "initializing";
    case dev::DeviceState::Running:
        if (!m_devices.startupComplete())
        {
            if (std::string(device->getClass()) == dev::RadaiWs::kClass)
            {
                if (!isConnected(device))
                    return "offline";
            }
            return "online";
        }
        if (!isConnected(device))
            return "offline";
        return "online";
    default:
        return "offline";
    }
}

std::string IotStore::panelForClass(const std::string& class_name) const
{
    if (class_name == "srs_r4sn")
        return "radar";
    if (class_name == "wave_station")
        return "wave_station";
    if (class_name == "reolink_e1_pro")
        return "camera";
    if (class_name == "tuya_ep2h")
        return "plug";
    if (class_name == "samsung_g7" || class_name == "tizen_tv")
        return "tv";
    if (class_name.rfind("philips_wiz_e29", 0) == 0)
        return "light";
    return "plug";
}

std::string IotStore::classLabel(const std::string& class_name) const
{
    if (class_name == "srs_r4sn")
        return "mmWave 레이더";
    if (class_name == "wave_station")
        return "Wave Station";
    if (class_name == "reolink_e1_pro")
        return "IoT 카메라";
    if (class_name == "tuya_ep2h")
        return "스마트 플러그";
    if (class_name == "samsung_g7" || class_name == "tizen_tv")
        return "Tizen TV";
    if (class_name == "philips_wiz_e29_color")
        return "WiZ 컬러 조명";
    if (class_name == "philips_wiz_e29_white")
        return "WiZ 화이트 조명";
    return class_name;
}

Json::Value IotStore::normalizeState(const dev::Device* device, const nlohmann::json& raw) const
{
    const auto class_name = std::string(device->getClass());
    Json::Value state = toJsonValue(raw);

    if (class_name == "tuya_ep2h")
    {
        Json::Value normalized;
        normalized["switch"] = state.isMember("switch") ? state["switch"] : false;
        if (state.isMember("power_w"))
            normalized["power"] = state["power_w"];
        else if (state.isMember("power"))
            normalized["power"] = state["power"];
        else
            normalized["power"] = 0.0;

        if (state.isMember("voltage_v"))
            normalized["voltage"] = state["voltage_v"];
        else if (state.isMember("voltage"))
            normalized["voltage"] = state["voltage"];
        else
            normalized["voltage"] = 0.0;

        if (state.isMember("current_ma"))
            normalized["current"] = state["current_ma"];
        else if (state.isMember("current"))
            normalized["current"] = state["current"];
        else
            normalized["current"] = 0.0;

        if (state.isMember("energy_kwh"))
            normalized["energy"] = state["energy_kwh"];
        else         if (state.isMember("energy"))
            normalized["energy"] = state["energy"];
        else
            normalized["energy"] = 0.0;
        return normalized;
    }

    if (class_name == "wave_station")
    {
        Json::Value normalized;
        normalized["micLevel"] = state.isMember("mic_level") ? state["mic_level"].asDouble()
            : (state.isMember("micLevel") ? state["micLevel"].asDouble() : 0.0);
        Json::Value env;
        if (state.isMember("env") && state["env"].isObject())
            env = state["env"];
        else
        {
            if (state.isMember("lux"))
                env["lux"] = state["lux"];
            if (state.isMember("temperature_c"))
                env["tempC"] = state["temperature_c"];
            else if (state.isMember("tempC"))
                env["tempC"] = state["tempC"];
            if (state.isMember("humidity_percent"))
                env["humidity"] = state["humidity_percent"];
            else if (state.isMember("humidity"))
                env["humidity"] = state["humidity"];
        }
        if (!env.isMember("lux"))
            env["lux"] = 0;
        if (!env.isMember("tempC"))
            env["tempC"] = 0.0;
        if (!env.isMember("humidity"))
            env["humidity"] = 0;
        normalized["env"] = env;
        return normalized;
    }

    if (class_name == "reolink_e1_pro")
    {
        Json::Value normalized;
        normalized["streaming"] = state.isMember("streaming") && state["streaming"].asBool();
        normalized["micLevel"] = state.isMember("micLevel") ? state["micLevel"].asDouble() : 0.0;
        return normalized;
    }

    return state;
}

std::string IotStore::stateSummary(
    const dev::DeviceManifestEntry& entry,
    const dev::Device* device,
    const Json::Value& state) const
{
    const auto status = connectionStatus(entry);
    if (status == "missing")
        return "장치 없음";
    if (status == "initializing")
        return "초기화 중";
    if (status == "offline" && entry.initResult != 0)
        return "연결 실패 · 코드 " + std::to_string(entry.initResult);
    if (!device || !isConnected(device))
        return "연결 끊김";

    const auto external_id = entry.config.value("id", "");
    const auto class_name = std::string(device->getClass());
    if (class_name == "tuya_ep2h")
    {
        if (const auto reading = ws::service::PowerManager::get().getReading(external_id))
        {
            char buffer[64];
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%s · %.1fW",
                reading->switch_on ? "켜짐" : "꺼짐",
                reading->connected && reading->switch_on ? reading->power_w : 0.0);
            return buffer;
        }
    }

    if (class_name == "tuya_ep2h")
    {
        const bool on = state.isMember("switch") && state["switch"].asBool();
        const double power = state.isMember("power") ? state["power"].asDouble() : 0.0;
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s · %.1fW", on ? "켜짐" : "꺼짐", on ? power : 0.0);
        return buffer;
    }
    if (class_name.rfind("philips_wiz_e29", 0) == 0)
    {
        const bool on = state.isMember("on") && state["on"].asBool();
        if (!on)
            return "꺼짐";
        if (state.isMember("brightness"))
        {
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "켜짐 · 밝기 %d%%", state["brightness"].asInt());
            return buffer;
        }
        return "켜짐";
    }
    if (class_name == "samsung_g7" || class_name == "tizen_tv")
    {
        const bool on = state.isMember("on") && state["on"].asBool();
        if (!on)
            return "꺼짐";
        if (state.isMember("volume"))
        {
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "켜짐 · 볼륨 %d", state["volume"].asInt());
            return buffer;
        }
        return "켜짐";
    }
    if (class_name == "reolink_e1_pro")
        return state.isMember("streaming") && state["streaming"].asBool() ? "스트리밍 중" : "대기 중";
    if (class_name == "wave_station" && state.isMember("micLevel"))
    {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "마이크 레벨 %d%%", static_cast<int>(state["micLevel"].asDouble() * 100));
        return buffer;
    }
    return device->isEnabled() ? "활성" : "비활성";
}

Json::Value IotStore::getSummary() const
{
    int online = 0;
    int initializing = 0;
    const auto& manifest = m_devices.manifestEntries();
    for (const auto& entry : manifest)
    {
        const auto status = connectionStatus(entry);
        if (status == "online")
            ++online;
        else if (status == "initializing")
            ++initializing;
    }

    const auto day_ago = std::chrono::system_clock::now() - std::chrono::hours(24);
    int today_events = 0;
    {
        std::lock_guard lock(g_event_mutex);
        for (const auto& event : g_events)
        {
            if (!event.isMember("occurredAt"))
                continue;
            ++today_events;
            (void)day_ago;
        }
    }

    Json::Value body;
    body["onlineDeviceCount"] = online;
    body["totalDeviceCount"] = static_cast<Json::Int64>(manifest.size());
    body["initializingDeviceCount"] = initializing;
    body["devicesStarting"] = !m_devices.startupComplete();
    body["todayEventCount"] = today_events;
    body["activeRuleCount"] = 0;
    return body;
}

Json::Value IotStore::listDevices() const
{
    Json::Value body(Json::arrayValue);
    for (const auto& entry : m_devices.manifestEntries())
    {
        const auto& cfg = entry.config;
        const auto external_id = cfg.value("id", "");
        const auto class_name = cfg.value("class", "");
        const auto status = connectionStatus(entry);
        auto* device = findDevice(external_id);

        Json::Value item;
        item["id"] = external_id;
        item["name"] = cfg.value("name", "");
        item["description"] = cfg.value("description", "");
        item["vendor"] = cfg.value("vendor", "");
        item["model"] = cfg.value("model", "");
        item["class"] = class_name;
        item["classLabel"] = classLabel(class_name);
        item["panel"] = panelForClass(class_name);
        item["connectionStatus"] = status;
        item["connected"] = status == "online";
        item["available"] = true;
        item["enabled"] = cfg.value("enabled", true);
        item["stateSummary"] = stateSummary(entry, device, Json::Value());

        if (entry.initResult != 0 || !entry.initError.empty())
        {
            Json::Value err;
            err["code"] = entry.initResult;
            err["message"] = entry.initError.empty()
                ? ("error " + std::to_string(entry.initResult))
                : entry.initError;
            item["connectionError"] = err;
        }

        const auto room_id_text = cfg.value("room_id", "");
        if (!room_id_text.empty())
        {
            const auto room_id = dev::parseRoomID(room_id_text);
            Json::Value room;
            room["id"] = room_id_text;
            if (const auto* room_ptr = m_devices.findRoom(room_id))
                room["name"] = room_ptr->name;
            else
                room["name"] = "미지정";
            item["room"] = room;
        }
        else if (device && device->getRoomId() != 0)
        {
            Json::Value room;
            room["id"] = dev::roomIDToString(device->getRoomId());
            if (const auto* room_ptr = m_devices.findRoom(device->getRoomId()))
                room["name"] = room_ptr->name;
            else
                room["name"] = "미지정";
            item["room"] = room;
        }
        else
        {
            item["room"] = Json::Value(Json::nullValue);
        }

        body.append(item);
    }
    return body;
}

Json::Value IotStore::queryDevice(const std::string& external_id, const std::string& query_name, std::string& code) const
{
    if (isManifestInitializing(m_devices, external_id))
    {
        code = "DEVICE_INITIALIZING";
        return Json::Value();
    }

    auto* device = findDevice(external_id);
    if (!device)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }
    if (device->getState() == dev::DeviceState::Initializing)
    {
        code = "DEVICE_INITIALIZING";
        return Json::Value();
    }
    if (!isConnected(device))
    {
        code = "DEVICE_OFFLINE";
        return Json::Value();
    }

    auto* queryable = asQueryable(device);
    if (!queryable)
    {
        code = "QUERY_UNAVAILABLE";
        return Json::Value();
    }

    if (std::string(device->getClass()) == "tuya_ep2h" && query_name == "status")
    {
        if (const auto plug = queryPlugStatus(external_id, false))
            return *plug;
        code = "QUERY_FAILED";
        return Json::Value();
    }

    const auto raw = queryable->query(query_name, {});
    if (isQueryError(raw) && (query_name == "status" || query_name == "state"))
    {
        const auto fallback = queryable->query(query_name == "status" ? "state" : "status", {});
        if (!isQueryError(fallback))
            return normalizeState(device, fallback);
    }
    if (isQueryError(raw))
    {
        code = "QUERY_FAILED";
        return Json::Value();
    }

    if (query_name == "status" || query_name == "state")
        return normalizeState(device, raw);

    return toJsonValue(raw);
}

Json::Value IotStore::invokeDevice(
    const std::string& external_id,
    const std::string& action_name,
    const Json::Value& params,
    std::string& code)
{
    if (isManifestInitializing(m_devices, external_id))
    {
        code = "DEVICE_INITIALIZING";
        return Json::Value();
    }

    auto* device = findDevice(external_id);
    if (!device)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }
    if (device->getState() == dev::DeviceState::Initializing)
    {
        code = "DEVICE_INITIALIZING";
        return Json::Value();
    }
    if (!isConnected(device))
    {
        code = "DEVICE_OFFLINE";
        return Json::Value();
    }

    auto* actionable = asActionable(device);
    if (!actionable || !actionable->findAction(action_name))
    {
        code = "ACTION_NOT_FOUND";
        return Json::Value();
    }

    nlohmann::json nlohmann_params = nlohmann::json::object();
    if (params.isObject())
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::istringstream stream(Json::writeString(builder, params));
        nlohmann_params = nlohmann::json::parse(stream);
    }

    const int rc = actionable->invoke(action_name, nlohmann_params);
    if (rc != 0)
    {
        code = "INVOKE_FAILED";
        return Json::Value();
    }

    Json::Value detail;
    detail["action"] = action_name;
    detail["params"] = params.isObject() ? params : Json::Value(Json::objectValue);
    logIotEvent(
        "execution",
        external_id,
        std::string(device->getName()),
        "수동 제어: " + action_name,
        "manual",
        detail);

    std::string ignored;
    const auto query_name = std::string(device->getClass()).find("tizen") != std::string::npos
        || std::string(device->getClass()) == "samsung_g7"
        ? "state"
        : "status";
    return queryDevice(external_id, query_name, ignored);
}

bool IotStore::reconnectDevice(const std::string& external_id, std::string& error, std::string& code)
{
    for (const auto& entry : m_devices.manifestEntries())
    {
        if (entry.config.value("id", "") != external_id)
            continue;

        if (entry.state == dev::DeviceEntryState::Unsupported)
        {
            error = "지원하지 않는 장치 클래스입니다.";
            code = "NOT_FOUND";
            return false;
        }

        if (m_devices.retryDevice(external_id, error))
            return true;

        code = "DEVICE_OFFLINE";
        return false;
    }

    error = "기기를 찾을 수 없습니다.";
    code = "NOT_FOUND";
    return false;
}

Json::Value IotStore::listEvents(const std::string& device_id) const
{
    Json::Value body(Json::arrayValue);
    std::lock_guard lock(g_event_mutex);
    for (const auto& event : g_events)
    {
        if (!device_id.empty() && event.isMember("deviceId") && event["deviceId"].asString() != device_id)
            continue;
        body.append(event);
    }
    return body;
}

void IotStore::logEvent(
    const std::string& type,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& message,
    const std::string& triggered_by,
    const Json::Value& detail)
{
    logIotEvent(type, device_id, device_name, message, triggered_by, detail);
}

std::optional<Json::Value> IotStore::queryPlugStatus(const std::string& external_id, bool force_refresh) const
{
    if (force_refresh)
        ws::service::PowerManager::get().samplePlugNow(external_id);

    const auto reading = ws::service::PowerManager::get().getReading(external_id);
    if (!reading || !reading->connected)
        return std::nullopt;

    Json::Value state;
    state["switch"] = reading->switch_on;
    state["power"] = reading->power_w;
    state["voltage"] = reading->voltage_v;
    state["current"] = reading->current_ma;
    return state;
}

std::vector<std::string> IotStore::listPlugIds() const
{
    std::vector<std::string> ids;
    for (const auto& entry : m_devices.manifestEntries())
    {
        if (entry.config.value("class", "") != "tuya_ep2h")
            continue;
        ids.push_back(entry.config.value("id", ""));
    }
    return ids;
}

dev::ReolinkE1Pro* IotStore::requireReolinkCamera(const std::string& external_id, std::string& code)
{
    if (isManifestInitializing(m_devices, external_id))
    {
        code = "DEVICE_INITIALIZING";
        return nullptr;
    }

    auto* device = findDevice(external_id);
    if (!device)
    {
        code = "NOT_FOUND";
        return nullptr;
    }
    if (device->getState() == dev::DeviceState::Initializing)
    {
        code = "DEVICE_INITIALIZING";
        return nullptr;
    }
    if (!isConnected(device))
    {
        code = "DEVICE_OFFLINE";
        return nullptr;
    }

    auto* camera = dynamic_cast<dev::ReolinkE1Pro*>(device);
    if (!camera)
    {
        code = "UNSUPPORTED_DEVICE";
        return nullptr;
    }
    return camera;
}

const dev::ReolinkE1Pro* IotStore::requireReolinkCamera(const std::string& external_id, std::string& code) const
{
    return const_cast<IotStore*>(this)->requireReolinkCamera(external_id, code);
}

Json::Value IotStore::getCameraStream(const std::string& external_id, std::string& code) const
{
    const auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return Json::Value();

    int viewers = 0;
    {
        std::lock_guard lock(g_camera_stream_mutex);
        const auto it = g_camera_stream_viewers.find(external_id);
        if (it != g_camera_stream_viewers.end())
            viewers = it->second;
    }

    const bool streaming = viewers > 0 && camera->isGo2rtcStreamActive();
    Json::Value body;
    body["status"] = streaming ? "streaming" : "idle";
    body["mode"] = "mse";
    if (streaming)
        body["url"] = "/api/v1/iot/devices/" + external_id + "/stream/mp4";
    else
        body["url"] = Json::Value(Json::nullValue);
    return body;
}

Json::Value IotStore::setCameraStream(const std::string& external_id, bool streaming, std::string& code)
{
    auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return Json::Value();

    {
        std::lock_guard lock(g_camera_stream_mutex);
        int& viewers = g_camera_stream_viewers[external_id];
        if (streaming)
        {
            ++viewers;
            if (viewers == 1 && !camera->ensureGo2rtcStream())
            {
                --viewers;
                if (viewers <= 0)
                    g_camera_stream_viewers.erase(external_id);
                code = "STREAM_UNAVAILABLE";
                return Json::Value();
            }
        }
        else if (viewers > 0)
        {
            --viewers;
            if (viewers <= 0)
            {
                g_camera_stream_viewers.erase(external_id);
                camera->releaseGo2rtcStream();
            }
        }
    }

    code.clear();
    return getCameraStream(external_id, code);
}

bool IotStore::exchangeCameraWebRtc(
    const std::string& external_id,
    const std::string& offer_sdp,
    std::string& answer_sdp,
    std::string& code)
{
    auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return false;

    if (!camera->isGo2rtcStreamActive() && !camera->ensureGo2rtcStream())
    {
        code = "STREAM_UNAVAILABLE";
        return false;
    }

    const std::string stream_name(camera->getGo2rtcStreamName());
    if (!service::Go2RtcService::get().exchangeWebRtc(stream_name, offer_sdp, answer_sdp))
    {
        code = "STREAM_UNAVAILABLE";
        return false;
    }

    code.clear();
    return true;
}

Json::Value IotStore::getPtzCapabilities(const std::string& external_id, std::string& code) const
{
    const auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return Json::Value();

    const auto caps = camera->getPtzCapabilities();
    Json::Value body;
    body["pan"] = caps.pan;
    body["tilt"] = caps.tilt;
    body["zoom"] = caps.zoom;
    body["absolute"] = caps.absolute;
    body["presets"] = caps.presets;
    body["home"] = caps.home;
    body["maxPresets"] = static_cast<Json::UInt>(caps.maxPresets);
    return body;
}

bool IotStore::moveCameraPtz(const std::string& external_id, const Json::Value& vector, std::string& code)
{
    auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return false;

    dev::PtzVector velocity;
    constexpr float kPtzScale = 0.35f;
    velocity.pan = (vector.isMember("pan") ? static_cast<float>(vector["pan"].asDouble()) : 0.0f) * kPtzScale;
    velocity.tilt = (vector.isMember("tilt") ? static_cast<float>(vector["tilt"].asDouble()) : 0.0f) * kPtzScale;
    velocity.zoom = vector.isMember("zoom") ? static_cast<float>(vector["zoom"].asDouble()) : 0.0f;

    (void)camera->movePtzAsync(velocity, 0);
    code.clear();
    return true;
}

bool IotStore::stopCameraPtz(const std::string& external_id, std::string& code)
{
    auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return false;

    (void)std::async(std::launch::async, [camera]()
    {
        (void)camera->stopPtz();
    });
    code.clear();
    return true;
}

Json::Value IotStore::zoomCameraPtz(const std::string& external_id, int delta, std::string& code)
{
    const auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return Json::Value();

    int zoom = 0;
    {
        std::lock_guard lock(g_camera_stream_mutex);
        zoom = g_camera_zoom_levels[external_id];
        zoom = std::max(0, std::min(100, zoom + delta));
        g_camera_zoom_levels[external_id] = zoom;
    }

    Json::Value body;
    body["zoom"] = zoom;
    return body;
}

bool IotStore::captureCameraSnapshot(
    const std::string& external_id,
    std::vector<uint8_t>& out_jpeg,
    std::string& occurred_at,
    std::string& code)
{
    auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return false;

    dev::CameraFrame frame;
    if (!camera->captureFrame(frame) || frame.data.empty())
    {
        code = "SNAPSHOT_FAILED";
        return false;
    }

    out_jpeg = std::move(frame.data);
    occurred_at = isoNowKst();

    Json::Value detail;
    detail["occurredAt"] = occurred_at;
    logIotEvent(
        "snapshot",
        external_id,
        std::string(camera->getName()),
        "스냅샷 캡처",
        "manual",
        detail);

    code.clear();
    return true;
}

bool IotStore::sendDeviceTts(
    const std::string& external_id,
    const std::string& text,
    int speaker_id,
    float speed,
    std::string& code)
{
    auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return false;

    if (text.empty())
    {
        code = "INVALID_BODY";
        return false;
    }

#ifdef WAVE_BUILD_TTS
    tts::Service* tts_service = nullptr;
    if (!ensureTtsService(tts_service, code))
        return false;

    tts::Input input;
    input.locale = "ko-KR";
    input.text = text;
    input.speakerID = static_cast<uint32_t>(std::max(0, speaker_id));
    input.speed = speed > 0.0f ? speed : 1.0f;

    std::vector<float> audio;
    {
        std::lock_guard lock(g_tts_generate_mutex);
        if (tts_service->generate(input, audio) != tts::SUCCESS || audio.empty())
        {
            code = "TTS_FAILED";
            return false;
        }
    }

    const int32_t sample_rate = tts_service->sampleRate(input.locale);
    if (sample_rate <= 0)
    {
        code = "TTS_FAILED";
        return false;
    }

    const auto& state = AppState::get();
    const auto wav_dir = state.resolvePath("cache/tts");
    std::error_code dir_ec;
    std::filesystem::create_directories(wav_dir, dir_ec);

    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto wav_path = (wav_dir / ("tts_" + external_id + "_" + std::to_string(stamp) + ".wav")).string();
    if (!write_wav_file(wav_path, audio, sample_rate))
    {
        code = "TTS_FAILED";
        return false;
    }

    if (!camera->playAudioFile(wav_path))
    {
        code = "TTS_PLAYBACK_FAILED";
        std::error_code ec;
        std::filesystem::remove(wav_path, ec);
        return false;
    }

    // go2rtc/ffmpeg reads the file asynchronously; keep it until playback finishes.
    const auto cleanup_delay = std::chrono::milliseconds(
        static_cast<int64_t>(audio.size()) * 1000 / sample_rate + 3000);
    (void)TaskQueue::enqueueAsync([wav_path, cleanup_delay]()
    {
        std::this_thread::sleep_for(cleanup_delay);
        std::error_code ec;
        std::filesystem::remove(wav_path, ec);
    });

    Json::Value detail;
    detail["text"] = text;
    detail["speakerId"] = speaker_id;
    logIotEvent(
        "tts",
        external_id,
        std::string(camera->getName()),
        "TTS 재생",
        "manual",
        detail);

    code.clear();
    return true;
#else
    (void)speaker_id;
    (void)speed;
    code = "TTS_UNAVAILABLE";
    return false;
#endif
}

void resetCameraStreamSession(const std::string& external_id)
{
    std::lock_guard lock(g_camera_stream_mutex);
    g_camera_stream_viewers.erase(external_id);
    g_camera_zoom_levels.erase(external_id);
}

bool warmUpTtsService(std::string& error)
{
#ifdef WAVE_BUILD_TTS
    tts::Service* service = nullptr;
    std::string code;
    if (!ensureTtsService(service, code))
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

bool isTtsServiceReady()
{
#ifdef WAVE_BUILD_TTS
    return g_tts_ready.load(std::memory_order_acquire);
#else
    return false;
#endif
}

void queueDeviceTts(
    const std::string& external_id,
    const std::string& text,
    int speaker_id,
    float speed)
{
#ifdef WAVE_BUILD_TTS
    (void)TaskQueue::enqueueAsync([external_id, text, speaker_id, speed]()
    {
        auto& app = AppState::get();
        IotStore worker(app.deviceManager);
        std::string code;
        if (!worker.sendDeviceTts(external_id, text, speaker_id, speed, code))
            LOG_ERROR("Async TTS failed for {}: {}", external_id, code);
    });
#else
    (void)external_id;
    (void)text;
    (void)speaker_id;
    (void)speed;
#endif
}

void shutdownBackgroundServices()
{
    {
        std::lock_guard lock(g_camera_stream_mutex);
        g_camera_stream_viewers.clear();
        g_camera_zoom_levels.clear();
    }

#ifdef WAVE_BUILD_TTS
    g_tts_ready.store(false, std::memory_order_release);
    std::lock_guard lock(g_tts_mutex);
    g_tts_service.reset();
#endif

    service::Go2RtcService::get().shutdownAll();
}

bool IotStore::openCameraMp4Stream(const std::string& external_id, std::string& stream_name, std::string& code)
{
    auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return false;

    if (!camera->isGo2rtcStreamActive() && !camera->ensureGo2rtcStream())
    {
        code = "STREAM_UNAVAILABLE";
        return false;
    }

    stream_name = std::string(camera->getGo2rtcStreamName());
    code.clear();
    return true;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
