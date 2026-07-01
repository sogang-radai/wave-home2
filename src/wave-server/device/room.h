#pragma once

#include <string>
#include <string_view>

#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

using RoomID = uint64_t;

RoomID generateRoomID(uint64_t seed);
RoomID parseRoomID(std::string_view id);
std::string roomIDToString(RoomID id);

struct Room
{
    RoomID id = 0;
    std::string name;
    std::string description;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
