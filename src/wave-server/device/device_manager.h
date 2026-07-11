#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "device.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

enum class DeviceEntryState
{
    Pending,
    Initializing,
    Ready,
    Failed,
    Unsupported,
    Disabled,
};

struct DeviceManifestEntry
{
    json config;
    DeviceEntryState state = DeviceEntryState::Pending;
    int initResult = 0;
    std::string initError;
};

class DeviceManager
{
public:
    using RoomPtr = Room*;
    using DevicePtr = std::unique_ptr<Device>;
    using DeviceHandle = Device*;

    DeviceManager() = default;
    ~DeviceManager();

    bool load(const json& room_list, const json& device_list);
    void startDevicesAsync();
    void shutdown();

    /** Immediate reconnect attempt for one manifest device (also used by API). */
    bool retryDevice(const std::string& external_id, std::string& error);

    bool manifestLoaded() const { return m_manifestLoaded; }
    bool startupComplete() const { return m_startupComplete.load(std::memory_order_acquire); }

    const std::vector<DeviceManifestEntry>& manifestEntries() const { return m_manifest; }

    const std::vector<RoomPtr>& enumerateRooms() const;
    RoomPtr findRoom(RoomID room_id) const;

    const std::vector<DevicePtr>& enumerateDevices() const;
    DeviceHandle findDevice(DeviceID id) const;

    std::vector<DeviceHandle> enumerateDevicesByRoom(RoomID room_id) const;

private:
    void registerDevice(std::unique_ptr<Device> device);
    void unregisterDevice(DeviceID id);
    bool tryInitEntry(DeviceManifestEntry& entry, bool manual_retry);
    bool entryNeedsRetry(const DeviceManifestEntry& entry) const;
    void startRetryLoop();
    void retryFailedDevices(bool manual);

    std::vector<std::unique_ptr<Room>> m_ownedRooms;
    std::vector<RoomPtr> m_rooms;
    std::unordered_map<RoomID, RoomPtr> m_roomMap;

    std::vector<DevicePtr> m_devices;
    std::unordered_map<DeviceID, DeviceHandle> m_deviceMap;
    std::unordered_map<RoomID, std::vector<DeviceHandle>> m_roomDeviceMap;

    std::vector<DeviceManifestEntry> m_manifest;
    bool m_manifestLoaded = false;
    std::atomic<bool> m_startupComplete{false};
    std::atomic<bool> m_startRequested{false};
    std::atomic<bool> m_shutdown{false};
    std::thread m_startThread;
    std::thread m_retryThread;
    std::atomic<bool> m_retryStop{false};
    int m_retryIntervalSec = 16;
    mutable std::mutex m_mutex;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
