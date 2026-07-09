#include "device_manager.h"

#include <algorithm>
#include <stdexcept>

#include "../app/app_state.h"
#include "../core/logger.h"
#include "platform/philips_wiz_e29.h"
#include "platform/droid_cam.h"
#include "platform/radai_ws.h"
#include "platform/reolink_e1pro.h"
#include "platform/samsung_tizen.h"
#include "platform/srs_r4sn.h"
#include "platform/tuya_ep2h.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    std::unique_ptr<Device> createDevice(const std::string& className)
    {
        if (className == "samsung_g7")
            return std::make_unique<SamsungTizen>();
        if (className == "srs_r4sn")
            return std::make_unique<SRSR4SN>();
        if (className == "wave_station")
            return std::make_unique<RadaiWs>();
        if (className == "droid_cam")
            return std::make_unique<DroidCam>();
        if (className == "reolink_e1_pro")
            return std::make_unique<ReolinkE1Pro>();
        if (className == "tuya_ep2h")
            return std::make_unique<TuyaEP2H>();
        if (className.rfind("philips_wiz_e29", 0) == 0)
            return std::make_unique<PhilipsWizE29>();
        return nullptr;
    }

    std::string manifestDeviceId(const DeviceManifestEntry& entry)
    {
        return entry.config.value("id", "");
    }

    std::string manifestDeviceName(const DeviceManifestEntry& entry)
    {
        return entry.config.value("name", entry.config.value("class", "device"));
    }

    void logConnectionEvent(
        const std::string& device_id,
        const std::string& device_name,
        bool success,
        int error_code,
        const std::string& error_message,
        bool manual)
    {
        Json::Value detail(Json::objectValue);
        detail["manual"] = manual;
        if (!success)
        {
            detail["errorCode"] = error_code;
            if (!error_message.empty())
                detail["errorMessage"] = error_message;
        }

        AppState::get().iot.logEvent(
            "connection",
            device_id,
            device_name,
            success ? "연결됨" : "연결 실패: " + std::to_string(error_code),
            manual ? "manual" : "auto",
            detail);
    }
}

DeviceManager::~DeviceManager()
{
    shutdown();
}

void DeviceManager::registerDevice(std::unique_ptr<Device> device)
{
    std::lock_guard lock(m_mutex);

    const DeviceID deviceId = device->getId();
    if (deviceId == 0)
        return;

    DeviceHandle handle = device.get();
    m_deviceMap[deviceId] = handle;

    const RoomID roomId = device->getRoomId();
    if (roomId != 0)
        m_roomDeviceMap[roomId].push_back(handle);

    m_devices.push_back(std::move(device));
}

void DeviceManager::unregisterDevice(DeviceID id)
{
    std::lock_guard lock(m_mutex);

    auto handle_it = m_deviceMap.find(id);
    DeviceHandle handle = handle_it != m_deviceMap.end() ? handle_it->second : nullptr;
    m_deviceMap.erase(id);

    if (handle)
    {
        for (auto& [room_id, handles] : m_roomDeviceMap)
        {
            (void)room_id;
            handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
        }
    }

    m_devices.erase(
        std::remove_if(
            m_devices.begin(),
            m_devices.end(),
            [id](const DevicePtr& owned) { return owned && owned->getId() == id; }),
        m_devices.end());
}

bool DeviceManager::entryNeedsRetry(const DeviceManifestEntry& entry) const
{
    if (entry.state == DeviceEntryState::Unsupported || entry.state == DeviceEntryState::Disabled)
        return false;

    if (entry.state == DeviceEntryState::Failed || entry.state == DeviceEntryState::Pending)
        return true;

    const auto id = parseDeviceID(manifestDeviceId(entry));
    if (id == 0)
        return false;

    auto* device = findDevice(id);
    if (!device)
        return entry.state == DeviceEntryState::Ready;

    if (!device->isEnabled())
        return false;

    return device->getState() != DeviceState::Running;
}

bool DeviceManager::tryInitEntry(DeviceManifestEntry& entry, bool manual_retry)
{
    if (entry.state == DeviceEntryState::Unsupported)
        return false;

    const std::string className = entry.config.value("class", "");
    const std::string deviceName = manifestDeviceName(entry);
    const std::string external_id = manifestDeviceId(entry);
    const DeviceID device_id = parseDeviceID(external_id);

    entry.state = DeviceEntryState::Initializing;
    entry.initResult = 0;
    entry.initError.clear();

    if (device_id != 0)
    {
        if (auto* existing = findDevice(device_id))
        {
            if (className == "reolink_e1_pro" || className == "droid_cam")
                AppState::get().iot.resetCameraStreamSession(external_id);
            existing->shutdown();
            unregisterDevice(device_id);
        }
    }

    auto device = createDevice(className);
    if (!device)
    {
        entry.state = DeviceEntryState::Unsupported;
        entry.initError = "unsupported device class";
        return false;
    }

    const int rc = device->init(entry.config);
    entry.initResult = rc;

    if (rc == -2)
    {
        LOG_INFO("Device manager: '{}' disabled (id={})", deviceName, external_id);
        entry.state = DeviceEntryState::Disabled;
        registerDevice(std::move(device));
        return true;
    }

    if (rc != 0)
    {
        entry.state = DeviceEntryState::Failed;
        entry.initError = std::string(device->getErrorString(rc));
        LOG_WARN(
            "Device manager: '{}' init failed with {} ({})",
            deviceName,
            rc,
            entry.initError);
        logConnectionEvent(external_id, deviceName, false, rc, entry.initError, manual_retry);
        return false;
    }

    entry.state = DeviceEntryState::Ready;
    registerDevice(std::move(device));

    const auto* handle = findDevice(device_id);
    const bool online = handle && handle->isEnabled() && handle->getState() == DeviceState::Running;
    if (online)
    {
        logConnectionEvent(external_id, deviceName, true, 0, {}, manual_retry);
        return true;
    }

    entry.state = DeviceEntryState::Failed;
    entry.initResult = -4;
    entry.initError = "device initialized but not running";
    logConnectionEvent(external_id, deviceName, false, entry.initResult, entry.initError, manual_retry);
    return false;
}

void DeviceManager::retryFailedDevices(bool manual)
{
    for (auto& entry : m_manifest)
    {
        if (!ws::AppState::get().running.load(std::memory_order_acquire))
            break;
        if (!entryNeedsRetry(entry))
            continue;
        tryInitEntry(entry, manual);
    }
}

void DeviceManager::startRetryLoop()
{
    m_retryThread = std::thread([this]()
    {
        while (!m_retryStop.load(std::memory_order_acquire)
            && ws::AppState::get().running.load(std::memory_order_acquire))
        {
            const int wait_sec = std::max(1, m_retryIntervalSec);
            for (int i = 0; i < wait_sec; ++i)
            {
                if (m_retryStop.load(std::memory_order_acquire)
                    || !ws::AppState::get().running.load(std::memory_order_acquire))
                    return;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            bool any_failed = false;
            for (const auto& entry : m_manifest)
            {
                if (entryNeedsRetry(entry))
                {
                    any_failed = true;
                    break;
                }
            }

            if (!any_failed)
            {
                m_retryIntervalSec = 16;
                continue;
            }

            LOG_INFO("Device manager: retrying offline devices (interval {}s)", m_retryIntervalSec);
            retryFailedDevices(false);

            if (m_retryIntervalSec < 3600)
                m_retryIntervalSec = std::min(3600, m_retryIntervalSec * 2);
        }
    });
}

bool DeviceManager::retryDevice(const std::string& external_id, std::string& error)
{
    for (auto& entry : m_manifest)
    {
        if (manifestDeviceId(entry) != external_id)
            continue;

        if (entry.state == DeviceEntryState::Unsupported)
        {
            error = "지원하지 않는 장치 클래스입니다.";
            return false;
        }

        if (tryInitEntry(entry, true))
        {
            m_retryIntervalSec = 16;
            return true;
        }

        error = entry.initError.empty()
            ? ("연결 실패 (코드 " + std::to_string(entry.initResult) + ")")
            : entry.initError;
        return false;
    }

    error = "기기를 찾을 수 없습니다.";
    return false;
}

bool DeviceManager::load(const json& room_list, const json& device_list)
{
    shutdown();

    if (!room_list.contains("rooms") || !room_list["rooms"].is_array())
        throw std::invalid_argument("room_list requires array field 'rooms'");

    m_ownedRooms.clear();
    m_rooms.clear();
    m_roomMap.clear();
    m_devices.clear();
    m_deviceMap.clear();
    m_roomDeviceMap.clear();
    m_manifest.clear();
    m_manifestLoaded = false;
    m_startupComplete.store(false, std::memory_order_release);
    m_startRequested.store(false, std::memory_order_release);
    m_retryIntervalSec = 16;

    for (const auto& entry : room_list["rooms"])
    {
        if (!entry.is_object())
            throw std::invalid_argument("room entry must be a JSON object");

        if (!entry.contains("id") || !entry["id"].is_string())
            throw std::invalid_argument("room entry requires string field 'id'");

        if (!entry.contains("name") || !entry["name"].is_string())
            throw std::invalid_argument("room entry requires string field 'name'");

        if (entry.contains("description") && !entry["description"].is_string())
            throw std::invalid_argument("room entry field 'description' must be a string");

        const RoomID roomId = parseRoomID(entry["id"].get<std::string>());
        if (roomId == 0)
            throw std::invalid_argument("room entry field 'id' must be a 16-character hex string");

        auto room = std::make_unique<Room>();
        room->id = roomId;
        room->name = entry["name"].get<std::string>();
        room->description = entry.value("description", "");

        RoomPtr roomPtr = room.get();
        m_ownedRooms.push_back(std::move(room));
        m_roomMap[roomId] = roomPtr;
        m_rooms.push_back(roomPtr);
    }

    if (!device_list.contains("device_list") || !device_list["device_list"].is_array())
        throw std::invalid_argument("device_list requires array field 'device_list'");

    for (const auto& entry : device_list["device_list"])
    {
        if (!entry.is_object())
            throw std::invalid_argument("device entry must be a JSON object");

        DeviceManifestEntry manifest_entry;
        manifest_entry.config = entry;

        const std::string className = entry.value("class", "");
        if (!createDevice(className))
        {
            manifest_entry.state = DeviceEntryState::Unsupported;
            manifest_entry.initError = "unsupported device class";
            LOG_WARN("Device manager: unsupported class '{}' (id={})", className, entry.value("id", "?"));
        }

        m_manifest.push_back(std::move(manifest_entry));
    }

    m_manifestLoaded = true;
    return true;
}

void DeviceManager::startDevicesAsync()
{
    if (!m_manifestLoaded || m_startRequested.exchange(true))
        return;

    m_startThread = std::thread([this]()
    {
        int online = 0;
        for (auto& entry : m_manifest)
        {
            if (!ws::AppState::get().running.load(std::memory_order_acquire))
                break;

            if (entry.state == DeviceEntryState::Unsupported)
                continue;

            if (tryInitEntry(entry, false))
            {
                const auto* handle = findDevice(parseDeviceID(manifestDeviceId(entry)));
                if (handle && handle->getState() == DeviceState::Running)
                    ++online;
            }
        }

        m_startupComplete.store(true, std::memory_order_release);
        LOG_INFO(
            "Device startup complete ({} running / {} manifest entries)",
            online,
            m_manifest.size());

        startRetryLoop();
    });
}

void DeviceManager::shutdown()
{
    m_retryStop.store(true, std::memory_order_release);

    for (auto& owned : m_devices)
    {
        if (owned)
            owned->shutdown();
    }

    if (m_retryThread.joinable())
        m_retryThread.join();

    if (m_startThread.joinable())
        m_startThread.join();

    m_startRequested.store(false, std::memory_order_release);
    m_retryStop.store(false, std::memory_order_release);

    m_devices.clear();
    m_deviceMap.clear();
    m_roomDeviceMap.clear();
}

const std::vector<DeviceManager::RoomPtr>& DeviceManager::enumerateRooms() const
{
    return m_rooms;
}

DeviceManager::RoomPtr DeviceManager::findRoom(RoomID room_id) const
{
    const auto it = m_roomMap.find(room_id);
    if (it == m_roomMap.end())
        return nullptr;
    return it->second;
}

const std::vector<DeviceManager::DevicePtr>& DeviceManager::enumerateDevices() const
{
    return m_devices;
}

DeviceManager::DeviceHandle DeviceManager::findDevice(DeviceID id) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_deviceMap.find(id);
    if (it == m_deviceMap.end())
        return nullptr;
    return it->second;
}

std::vector<DeviceManager::DeviceHandle> DeviceManager::enumerateDevicesByRoom(RoomID room_id) const
{
    const auto it = m_roomDeviceMap.find(room_id);
    if (it == m_roomDeviceMap.end())
        return {};

    return it->second;
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
