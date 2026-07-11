#include "device_wire_id.hpp"

#include "../app/app_state.h"
#include "device.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    std::optional<int64_t> parseSmallPositiveInt(const std::string& value)
    {
        if (value.empty())
            return std::nullopt;
        try
        {
            size_t consumed = 0;
            const auto parsed = std::stoll(value, &consumed);
            if (consumed != value.size() || parsed <= 0)
                return std::nullopt;
            return parsed;
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    std::optional<int64_t> dbIdExists(
        const drogon::orm::DbClientPtr& client,
        int64_t db_id)
    {
        if (!client || db_id <= 0)
            return std::nullopt;

        auto rows = client->execSqlSync(
            "SELECT id FROM device WHERE id = ? AND archived = 0 LIMIT 1",
            db_id);
        if (rows.empty())
            return std::nullopt;
        return db_id;
    }
}

bool manifestHasWireId(const std::string& wire_id)
{
    if (wire_id.empty())
        return false;

    for (const auto& entry : AppState::get().deviceManager.manifestEntries())
    {
        if (entry.config.value("id", "") == wire_id)
            return true;
    }
    return false;
}

std::string wireIdForDbRow(int64_t db_id, const std::string& /*db_name*/)
{
    if (db_id <= 0)
        return {};
    return deviceIDToString(static_cast<DeviceID>(db_id));
}

std::optional<int64_t> dbIdForWireId(
    const drogon::orm::DbClientPtr& client,
    const std::string& wire_id)
{
    if (wire_id.empty())
        return std::nullopt;

    const auto parsed = parseDeviceID(wire_id);
    if (parsed != 0)
    {
        if (const auto found = dbIdExists(client, static_cast<int64_t>(parsed)))
            return found;
    }

    if (const auto as_int = parseSmallPositiveInt(wire_id))
    {
        if (const auto found = dbIdExists(client, *as_int))
            return found;
    }

    return std::nullopt;
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
