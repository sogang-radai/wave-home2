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
#include "../../../service/action_queue.h"
#include "../../../device/interface/camera.h"
#include "../../../device/interface/ptz.h"
#include "../../../service/go2rtc_service.h"
#ifdef WAVE_BUILD_TTS
#include "../../../core/task_queue.h"
#include "../../../service/tts_service.h"
#include <sherpa-onnx/c-api/cxx-api.h>
#endif

#include "../../../device/platform/droid_cam.h"
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
#ifdef WAVE_BUILD_TTS
    bool write_wav_file(const std::string& path, const std::vector<float>& samples, int32_t sample_rate)
    {
        sherpa_onnx::cxx::Wave wave;
        wave.samples = samples;
        wave.sample_rate = sample_rate;
        return sherpa_onnx::cxx::WriteWave(path, wave);
    }

    std::vector<int16_t> floatSamplesToPcm16(const std::vector<float>& samples)
    {
        std::vector<int16_t> pcm;
        pcm.reserve(samples.size());
        for (const float sample : samples)
        {
            const float clamped = std::max(-1.0f, std::min(1.0f, sample));
            pcm.push_back(static_cast<int16_t>(clamped * 32767.0f));
        }
        return pcm;
    }

    std::vector<int16_t> resampleLinearPcm16(
        const std::vector<int16_t>& input,
        int32_t input_rate,
        int32_t output_rate)
    {
        if (input.empty() || input_rate <= 0 || output_rate <= 0 || input_rate == output_rate)
            return input;

        const double ratio = static_cast<double>(output_rate) / static_cast<double>(input_rate);
        const size_t output_count = static_cast<size_t>(std::ceil(input.size() * ratio));
        std::vector<int16_t> output(output_count);
        for (size_t i = 0; i < output_count; ++i)
        {
            const double src_pos = static_cast<double>(i) / ratio;
            const size_t idx = static_cast<size_t>(src_pos);
            const double frac = src_pos - static_cast<double>(idx);
            const int16_t a = input[std::min(idx, input.size() - 1)];
            const int16_t b = input[std::min(idx + 1, input.size() - 1)];
            output[i] = static_cast<int16_t>(a + (b - a) * frac);
        }
        return output;
    }

    bool playTtsOnRadaiWs(dev::RadaiWs& wave_station, const std::vector<float>& audio, int32_t sample_rate)
    {
        if (audio.empty() || sample_rate <= 0)
            return false;

        auto pcm = floatSamplesToPcm16(audio);
        const auto sink_rate = static_cast<int32_t>(wave_station.getAudioConfig().sampleRate);
        if (sink_rate > 0 && sink_rate != sample_rate)
            pcm = resampleLinearPcm16(pcm, sample_rate, sink_rate);

        constexpr size_t kMaxSamplesPerFrame = 960;
        for (size_t offset = 0; offset < pcm.size(); offset += kMaxSamplesPerFrame)
        {
            const size_t count = std::min(kMaxSamplesPerFrame, pcm.size() - offset);
            dev::AudioFrame frame;
            frame.samples.assign(pcm.begin() + static_cast<std::ptrdiff_t>(offset),
                pcm.begin() + static_cast<std::ptrdiff_t>(offset + count));
            if (!wave_station.playFrame(frame))
                return false;
        }
        return true;
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

    dev::Device* lookupCameraDevice(
        const dev::DeviceManager& devices,
        const std::string& external_id,
        std::string& code,
        bool (*is_supported)(const dev::Device*))
    {
        if (isManifestInitializing(devices, external_id))
        {
            code = "DEVICE_INITIALIZING";
            return nullptr;
        }

        const auto id = dev::parseDeviceID(external_id);
        if (id == 0)
        {
            code = "NOT_FOUND";
            return nullptr;
        }

        auto* device = devices.findDevice(id);
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
        if (!device->isEnabled() || device->getState() != dev::DeviceState::Running)
        {
            code = "DEVICE_OFFLINE";
            return nullptr;
        }
        if (!is_supported(device))
        {
            code = "UNSUPPORTED_DEVICE";
            return nullptr;
        }
        return device;
    }

    bool isReolinkCamera(const dev::Device* device)
    {
        return dynamic_cast<const dev::ReolinkE1Pro*>(device) != nullptr;
    }

    bool isRadaiWs(const dev::Device* device)
    {
        return dynamic_cast<const dev::RadaiWs*>(device) != nullptr;
    }

    bool isGo2RtcCamera(const dev::Device* device)
    {
        return isReolinkCamera(device) || dynamic_cast<const dev::DroidCam*>(device) != nullptr;
    }

    bool cameraReleaseStreamOnViewerDrop(const dev::Device* device)
    {
        return dynamic_cast<const dev::ReolinkE1Pro*>(device) != nullptr;
    }

    bool cameraEnsureGo2rtcStream(dev::Device* device)
    {
        if (dynamic_cast<dev::DroidCam*>(device))
            return false;
        if (auto* reolink = dynamic_cast<dev::ReolinkE1Pro*>(device))
            return reolink->ensureGo2rtcStream();
        return false;
    }

    void cameraReleaseGo2rtcStream(dev::Device* device)
    {
        if (dynamic_cast<dev::DroidCam*>(device))
            return;
        if (auto* reolink = dynamic_cast<dev::ReolinkE1Pro*>(device))
            reolink->releaseGo2rtcStream();
    }

    bool cameraIsGo2rtcStreamActive(const dev::Device* device)
    {
        if (dynamic_cast<const dev::DroidCam*>(device))
            return false;
        if (auto* reolink = dynamic_cast<const dev::ReolinkE1Pro*>(device))
            return reolink->isGo2rtcStreamActive();
        return false;
    }

    void syncDroidStreamViewers(dev::Device* device, const std::string& external_id)
    {
        auto* droid = dynamic_cast<dev::DroidCam*>(device);
        if (!droid)
            return;

        int viewers = AppState::get().iot.streamViewers(external_id);
        droid->setStreamViewerCount(viewers);
    }

    std::string_view cameraGo2rtcStreamName(const dev::Device* device)
    {
        if (auto* reolink = dynamic_cast<const dev::ReolinkE1Pro*>(device))
            return reolink->getGo2rtcStreamName();
        return {};
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

    if (class_name == "reolink_e1_pro" || class_name == dev::DroidCam::kClass)
    {
        if (class_name == dev::DroidCam::kClass)
        {
            const auto* droid = dynamic_cast<const dev::DroidCam*>(device);
            return droid && droid->isAppAlive();
        }
        return isGo2RtcCamera(device);
    }

    if (class_name == "samsung_g7" || class_name == "tizen_tv")
    {
        // Avoid blocking TCP probes on every poll; session state tracks TV reachability.
        return dynamic_cast<const dev::SamsungTizen*>(device) != nullptr;
    }

    return true;
}

std::string IotStore::connectionStatusForEntry(const dev::DeviceManifestEntry& entry) const
{
    return connectionStatus(entry);
}

std::string IotStore::connectionStatus(const dev::DeviceManifestEntry& entry) const
{
    const auto external_id = entry.config.value("id", "");
    return connectionStatus(entry, findDevice(external_id));
}

std::string IotStore::connectionStatus(const dev::DeviceManifestEntry& entry, const dev::Device* device) const
{
    if (entry.state == dev::DeviceEntryState::Unsupported)
        return "missing";

    if (entry.state == dev::DeviceEntryState::Failed)
        return "offline";

    if (entry.state == dev::DeviceEntryState::Pending || entry.state == dev::DeviceEntryState::Initializing)
        return "initializing";

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
            if (std::string(device->getClass()) == dev::RadaiWs::kClass
                || std::string(device->getClass()) == dev::DroidCam::kClass)
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
    if (class_name == "reolink_e1_pro" || class_name == dev::DroidCam::kClass)
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
    if (class_name == dev::DroidCam::kClass)
        return "폰 카메라";
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

    if (class_name == "reolink_e1_pro" || class_name == dev::DroidCam::kClass)
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
    const Json::Value& state,
    const std::string& status) const
{
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
    if (class_name == "reolink_e1_pro" || class_name == dev::DroidCam::kClass)
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
    const int today_events = AppState::get().iot.eventCount();
    (void)day_ago;

    Json::Value body;
    body["onlineDeviceCount"] = online;
    body["totalDeviceCount"] = static_cast<Json::Int64>(manifest.size());
    body["initializingDeviceCount"] = initializing;
    body["devicesStarting"] = !m_devices.startupComplete();
    body["todayEventCount"] = today_events;
    body["activeRuleCount"] = static_cast<Json::UInt>(
        AppState::get().hasRuleStore() ? AppState::get().ruleStore().activeCount() : 0);
    return body;
}

Json::Value IotStore::listDevices() const
{
    struct ListedDevice
    {
        Json::Value item;
        bool connected_first = false;
        size_t manifest_index = 0;
    };

    std::vector<ListedDevice> listed;
    listed.reserve(m_devices.manifestEntries().size());

    size_t manifest_index = 0;
    for (const auto& entry : m_devices.manifestEntries())
    {
        const auto& cfg = entry.config;
        const auto external_id = cfg.value("id", "");
        const auto class_name = cfg.value("class", "");
        auto* device = findDevice(external_id);
        const auto status = connectionStatus(entry, device);

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
        Json::Value summary_state(Json::objectValue);
        if (status == "online" && device)
        {
            const std::string query_name =
                (class_name == "samsung_g7" || class_name == "tizen_tv")
                    ? "state"
                    : (class_name.rfind("philips_wiz_e29", 0) == 0 || class_name == "reolink_e1_pro"
                       || class_name == dev::DroidCam::kClass || class_name == "wave_station")
                        ? "status"
                        : std::string();
            if (!query_name.empty())
            {
                std::string ignored;
                summary_state = queryDevice(external_id, query_name, ignored);
            }
        }
        item["stateSummary"] = stateSummary(entry, device, summary_state, status);

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

        const bool connected_first = status == "online" || status == "initializing";
        listed.push_back(ListedDevice{std::move(item), connected_first, manifest_index});
        ++manifest_index;
    }

    std::stable_sort(
        listed.begin(),
        listed.end(),
        [](const ListedDevice& a, const ListedDevice& b)
        {
            if (a.connected_first != b.connected_first)
                return a.connected_first > b.connected_first;
            return a.manifest_index < b.manifest_index;
        });

    Json::Value body(Json::arrayValue);
    for (auto& entry : listed)
        body.append(std::move(entry.item));
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
        if (const auto plug = queryPlugStatus(external_id, true))
            return *plug;

        const auto raw = queryable->query("status", {});
        if (!isQueryError(raw))
            return normalizeState(device, raw);

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

    auto& app = AppState::get();
    if (app.automationReady())
    {
        service::ActionJob job;
        job.targetDeviceId = external_id;
        job.actionName = action_name;
        job.params = nlohmann_params;
        job.execMode = service::ExecMode::Once;
        job.sourceRef = "manual";
        job.logMessage = "수동 제어: " + action_name;

        const auto result = app.actionQueue().enqueueAndWait(job, 5000).get();
        if (!result.code.empty())
        {
            code = result.code;
            return Json::Value();
        }
    }
    else
    {
        const int rc = actionable->invoke(action_name, nlohmann_params);
        if (rc != 0)
        {
            code = "INVOKE_FAILED";
            return Json::Value();
        }

        Json::Value detail;
        detail["action"] = action_name;
        detail["params"] = params.isObject() ? params : Json::Value(Json::objectValue);
        AppState::get().iot.logEvent(
            "execution",
            external_id,
            std::string(device->getName()),
            "수동 제어: " + action_name,
            "manual",
            detail);
    }

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
    return AppState::get().iot.listEvents(device_id);
}

void IotStore::logEvent(
    const std::string& type,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& message,
    const std::string& triggered_by,
    const Json::Value& detail)
{
    AppState::get().iot.logEvent(type, device_id, device_name, message, triggered_by, detail);
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
    auto* device = lookupCameraDevice(m_devices, external_id, code, isReolinkCamera);
    if (!device)
        return nullptr;
    if (!isConnected(device))
    {
        code = "DEVICE_OFFLINE";
        return nullptr;
    }
    return dynamic_cast<dev::ReolinkE1Pro*>(device);
}

const dev::ReolinkE1Pro* IotStore::requireReolinkCamera(const std::string& external_id, std::string& code) const
{
    return const_cast<IotStore*>(this)->requireReolinkCamera(external_id, code);
}

dev::Device* IotStore::requireGo2RtcCamera(const std::string& external_id, std::string& code)
{
    auto* device = lookupCameraDevice(m_devices, external_id, code, isGo2RtcCamera);
    if (!device)
        return nullptr;
    if (!isConnected(device))
    {
        code = "DEVICE_OFFLINE";
        return nullptr;
    }
    return device;
}

const dev::Device* IotStore::requireGo2RtcCamera(const std::string& external_id, std::string& code) const
{
    return const_cast<IotStore*>(this)->requireGo2RtcCamera(external_id, code);
}

dev::DroidCam* IotStore::requireDroidCam(const std::string& external_id, std::string& code)
{
    auto* device = lookupCameraDevice(m_devices, external_id, code, isGo2RtcCamera);
    if (!device)
        return nullptr;

    auto* droid = dynamic_cast<dev::DroidCam*>(device);
    if (!droid)
    {
        code = "UNSUPPORTED_DEVICE";
        return nullptr;
    }
    if (!droid->isAppAlive())
    {
        code = "DEVICE_OFFLINE";
        return nullptr;
    }
    return droid;
}

const dev::DroidCam* IotStore::requireDroidCam(const std::string& external_id, std::string& code) const
{
    return const_cast<IotStore*>(this)->requireDroidCam(external_id, code);
}

dev::RadaiWs* IotStore::requireRadaiWs(const std::string& external_id, std::string& code)
{
    if (isManifestInitializing(m_devices, external_id))
    {
        code = "DEVICE_INITIALIZING";
        return nullptr;
    }

    const auto id = dev::parseDeviceID(external_id);
    if (id == 0)
    {
        code = "NOT_FOUND";
        return nullptr;
    }

    auto* device = m_devices.findDevice(id);
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
    if (!device->isEnabled() || device->getState() != dev::DeviceState::Running)
    {
        code = "DEVICE_OFFLINE";
        return nullptr;
    }
    if (!isRadaiWs(device))
    {
        code = "UNSUPPORTED_DEVICE";
        return nullptr;
    }
    if (!isConnected(device))
    {
        code = "DEVICE_OFFLINE";
        return nullptr;
    }

    code.clear();
    return dynamic_cast<dev::RadaiWs*>(device);
}

const dev::RadaiWs* IotStore::requireRadaiWs(const std::string& external_id, std::string& code) const
{
    return const_cast<IotStore*>(this)->requireRadaiWs(external_id, code);
}

Json::Value IotStore::snapshotWaveStationTelemetry(const std::string& external_id, std::string& code) const
{
    auto* wave_station = const_cast<IotStore*>(this)->requireRadaiWs(external_id, code);
    if (!wave_station)
        return Json::Value();

    Json::Value body;
    body["ok"] = true;

    int mic_rc = 0;
    if (auto* actionable = dynamic_cast<dev::Actionable*>(wave_station))
    {
        const auto& caps = wave_station->getCapabilities();
        const auto& audio = wave_station->getAudioConfig();
        json sub_params;
        if (audio.preferCompressedMic && caps.micOpus)
        {
            sub_params["target"] = "mic_opus";
            sub_params["compressed"] = true;
        }
        else
        {
            sub_params["target"] = "mic_pcm";
        }
        mic_rc = actionable->invoke("subscribe", sub_params);
    }
    else
    {
        mic_rc = -1;
    }

    if (mic_rc != 0)
    {
        body["ok"] = false;
        body["micLevel"] = Json::nullValue;
    }
    else if (auto* queryable = dynamic_cast<dev::Queryable*>(wave_station))
    {
        const auto status = queryable->query("status", json::object());
        if (!isQueryError(status) && status.contains("mic_level"))
            body["micLevel"] = status["mic_level"].get<double>();
        else
            body["micLevel"] = Json::nullValue;
    }
    else
    {
        body["micLevel"] = Json::nullValue;
    }

    if (auto* queryable = dynamic_cast<dev::Queryable*>(wave_station))
    {
        const auto env = queryable->query("env", json::object());
        if (!isQueryError(env) && !env.empty())
        {
            Json::Value env_json;
            if (env.contains("lux"))
                env_json["lux"] = env["lux"].get<double>();
            if (env.contains("temperature_c"))
                env_json["tempC"] = env["temperature_c"].get<double>();
            if (env.contains("humidity_percent"))
                env_json["humidity"] = env["humidity_percent"].get<double>();
            body["env"] = env_json;
        }
        else
        {
            body["env"] = Json::nullValue;
        }
    }
    else
    {
        body["env"] = Json::nullValue;
    }

    code.clear();
    return body;
}

Json::Value IotStore::learnIr(const std::string& external_id, uint32_t timeout_ms, std::string& code)
{
    auto* wave_station = requireRadaiWs(external_id, code);
    if (!wave_station)
        return Json::Value();

    auto& app = AppState::get();
    if (!app.hasIrStore())
    {
        code = "IR_STORE_UNAVAILABLE";
        return Json::Value();
    }

    return app.irStore().learnFromDevice(*wave_station, timeout_ms, code);
}

Json::Value IotStore::getCameraStream(const std::string& external_id, std::string& code) const
{
    const auto* camera = requireGo2RtcCamera(external_id, code);
    if (!camera)
        return Json::Value();

    const auto* droid = dynamic_cast<const dev::DroidCam*>(camera);
    const bool is_droid = droid != nullptr;

    const int viewers = AppState::get().iot.streamViewers(external_id);

    const bool streaming = viewers > 0
        && (is_droid ? droid->isAppAlive() : cameraIsGo2rtcStreamActive(camera));
    Json::Value body;
    body["status"] = streaming ? "streaming" : "idle";
    body["mode"] = is_droid ? "mjpeg" : "mse";
    if (streaming)
    {
        body["url"] = is_droid
            ? "/api/v1/iot/devices/" + external_id + "/stream/mjpeg"
            : "/api/v1/iot/devices/" + external_id + "/stream/mp4";
    }
    else
        body["url"] = Json::Value(Json::nullValue);
    return body;
}

Json::Value IotStore::setCameraStream(const std::string& external_id, bool streaming, std::string& code)
{
    auto* camera = requireGo2RtcCamera(external_id, code);
    if (!camera)
        return Json::Value();

    const auto* droid = dynamic_cast<const dev::DroidCam*>(camera);
    auto& iot = AppState::get().iot;
    if (!iot.changeStreamViewers(
        external_id,
        streaming,
        [&]() { return droid ? droid->isAppAlive() : cameraEnsureGo2rtcStream(camera); },
        [&]()
        {
            if (cameraReleaseStreamOnViewerDrop(camera))
                cameraReleaseGo2rtcStream(camera);
        }))
    {
        code = droid && !droid->isAppAlive() ? "DEVICE_OFFLINE" : "STREAM_UNAVAILABLE";
        return Json::Value();
    }

    syncDroidStreamViewers(camera, external_id);

    if (!streaming && droid)
        iot.stopDroidMjpegProxy(external_id);

    code.clear();
    return getCameraStream(external_id, code);
}

bool IotStore::exchangeCameraWebRtc(
    const std::string& external_id,
    const std::string& offer_sdp,
    std::string& answer_sdp,
    std::string& code)
{
    auto* camera = requireGo2RtcCamera(external_id, code);
    if (!camera)
        return false;

    if (dynamic_cast<const dev::DroidCam*>(camera))
    {
        code = "UNSUPPORTED_DEVICE";
        return false;
    }

    if (!cameraIsGo2rtcStreamActive(camera) && !cameraEnsureGo2rtcStream(camera))
    {
        code = "STREAM_UNAVAILABLE";
        return false;
    }

    const std::string stream_name(cameraGo2rtcStreamName(camera));
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

    const int zoom = AppState::get().iot.adjustZoom(external_id, delta);

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
    auto* camera = requireGo2RtcCamera(external_id, code);
    if (!camera)
        return false;

    auto* image_provider = dynamic_cast<dev::IImageProvider*>(camera);
    if (!image_provider)
    {
        code = "UNSUPPORTED_DEVICE";
        return false;
    }

    dev::CameraFrame frame;
    if (!image_provider->captureFrame(frame) || frame.data.empty())
    {
        code = "SNAPSHOT_FAILED";
        return false;
    }

    out_jpeg = std::move(frame.data);
    occurred_at = isoNowKst();

    Json::Value detail;
    detail["occurredAt"] = occurred_at;
    AppState::get().iot.logEvent(
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
    if (text.empty())
    {
        code = "INVALID_BODY";
        return false;
    }

#ifdef WAVE_BUILD_TTS
    auto& app = AppState::get();
    tts::Service* tts_service = app.tts.service(code);
    if (!tts_service)
        return false;

    tts::Input input;
    input.locale = "ko-KR";
    input.text = text;
    input.speakerID = static_cast<uint32_t>(std::max(0, speaker_id));
    input.speed = speed > 0.0f ? speed : 1.0f;

    std::vector<float> audio;
    {
        std::lock_guard lock(app.tts.generateMutex());
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

    auto* wave_station = requireRadaiWs(external_id, code);
    if (wave_station)
    {
        if (!playTtsOnRadaiWs(*wave_station, audio, sample_rate))
        {
            code = "TTS_PLAYBACK_FAILED";
            return false;
        }

        Json::Value detail;
        detail["text"] = text;
        detail["speakerId"] = speaker_id;
        AppState::get().iot.logEvent(
            "tts",
            external_id,
            std::string(wave_station->getName()),
            "TTS 재생",
            "manual",
            detail);

        code.clear();
        return true;
    }

    if (code != "UNSUPPORTED_DEVICE")
        return false;

    code.clear();
    auto* camera = requireReolinkCamera(external_id, code);
    if (!camera)
        return false;

    const auto wav_dir = app.resolvePath("cache/tts");
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
    AppState::get().iot.logEvent(
        "tts",
        external_id,
        std::string(camera->getName()),
        "TTS 재생",
        "manual",
        detail);

    code.clear();
    return true;
#else
    (void)external_id;
    (void)speaker_id;
    (void)speed;
    code = "TTS_UNAVAILABLE";
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

bool IotStore::openCameraMp4Stream(const std::string& external_id, std::string& stream_name, std::string& code)
{
    auto* camera = requireGo2RtcCamera(external_id, code);
    if (!camera)
        return false;

    if (dynamic_cast<const dev::DroidCam*>(camera))
    {
        code = "UNSUPPORTED_DEVICE";
        return false;
    }

    if (!cameraIsGo2rtcStreamActive(camera) && !cameraEnsureGo2rtcStream(camera))
    {
        code = "STREAM_UNAVAILABLE";
        return false;
    }

    stream_name = std::string(cameraGo2rtcStreamName(camera));
    code.clear();
    return true;
}

bool IotStore::openCameraMjpegStream(
    const std::string& external_id,
    std::string& host,
    uint16_t& port,
    std::string& path,
    std::string& code)
{
    const auto* droid = requireDroidCam(external_id, code);
    if (!droid)
        return false;

    const auto& iface = droid->getInterfaceConfig();
    host = iface.host;
    port = iface.port;
    path = iface.videoPath.empty() ? "/video" : iface.videoPath;
    code.clear();
    return true;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
