#include "device_manager.h"

#include <stdexcept>

#include "../core/logger.h"
#include "platform/philips_wiz_e29.h"
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
        if (className == "reolink_e1_pro")
            return std::make_unique<ReolinkE1Pro>();
        if (className == "tuya_ep2h")
            return std::make_unique<TuyaEP2H>();
        if (className.rfind("philips_wiz_e29", 0) == 0)
            return std::make_unique<PhilipsWizE29>();
        return nullptr;
    }
}

bool DeviceManager::load(const json& room_list, const json& device_list)
{
    if (!room_list.contains("rooms") || !room_list["rooms"].is_array())
        throw std::invalid_argument("room_list requires array field 'rooms'");

    m_ownedRooms.clear();
    m_rooms.clear();
    m_roomMap.clear();
    m_devices.clear();
    m_deviceMap.clear();
    m_roomDeviceMap.clear();

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

        const std::string className = entry.value("class", "");
        auto device = createDevice(className);
        if (!device)
        {
            LOG_WARN("Device manager: unsupported class '{}' (id={})", className, entry.value("id", "?"));
            continue;
        }

        const int rc = device->init(entry);
        if (rc == -2)
        {
            LOG_INFO("Device manager: '{}' disabled (id={})", entry.value("name", className), entry.value("id", "?"));
        }
        else if (rc != 0)
        {
            LOG_WARN(
                "Device manager: '{}' init failed with {} ({})",
                entry.value("name", className),
                rc,
                device->getErrorString(rc));
            continue;
        }

        const DeviceID deviceId = device->getId();
        if (deviceId == 0)
        {
            LOG_ERROR("Device manager: '{}' has invalid id", entry.value("name", className));
            continue;
        }

        DeviceHandle handle = device.get();
        m_deviceMap[deviceId] = handle;

        const RoomID roomId = device->getRoomId();
        if (roomId != 0)
            m_roomDeviceMap[roomId].push_back(handle);

        m_devices.push_back(std::move(device));
    }

    return true;
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
