#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "device.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

class DeviceManager
{
public:
    using RoomPtr = Room*;
    using DevicePtr = std::unique_ptr<Device>;
    using DeviceHandle = Device*;

    DeviceManager() = default;
    ~DeviceManager() = default;

    bool load(const json& room_list, const json& device_list);

    const std::vector<RoomPtr>& enumerateRooms() const;
    RoomPtr findRoom(RoomID room_id) const;

    const std::vector<DevicePtr>& enumerateDevices() const;
    DeviceHandle findDevice(DeviceID id) const;

    std::vector<DeviceHandle> enumerateDevicesByRoom(RoomID room_id) const;
    
private:
    std::vector<std::unique_ptr<Room>> m_ownedRooms;
    std::vector<RoomPtr> m_rooms;
    std::unordered_map<RoomID, RoomPtr> m_roomMap;

    std::vector<DevicePtr> m_devices;
    std::unordered_map<DeviceID, DeviceHandle> m_deviceMap;
    std::unordered_map<RoomID, std::vector<DeviceHandle>> m_roomDeviceMap;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
