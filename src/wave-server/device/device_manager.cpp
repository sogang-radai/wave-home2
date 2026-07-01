#include "device_manager.h"

#include <stdexcept>

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

bool DeviceManager::load(const json& room_list, const json& device_list)
{
    (void)device_list;

    if (!room_list.contains("rooms") || !room_list["rooms"].is_array())
        throw std::invalid_argument("room_list requires array field 'rooms'");

    m_ownedRooms.clear();
    m_rooms.clear();
    m_roomMap.clear();

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
