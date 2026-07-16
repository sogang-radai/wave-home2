#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../db/database.h"
#include <json/json.h>

#include "../../../core/json.h"
#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class GestureStore
{
public:
    bool load(
        const std::filesystem::path& registry_path,
        const std::function<std::filesystem::path(const std::string&)>& resolve_path,
        std::string& out_error);

    void setDatabaseClient(const db::DbClientPtr& client);
    bool syncFromDatabase(std::string& out_error, bool read_only = false);

    Json::Value listGestureSets() const;
    Json::Value getGestureSetDefinition(const std::string& gesture_set_id, std::string& code) const;
    Json::Value getRadarGestureSet(const std::string& device_id, std::string& code) const;
    Json::Value setRadarGestureSet(
        const std::string& device_id,
        const std::string& gesture_set_id,
        std::string& code);

private:
    struct RegistryEntry
    {
        int64_t db_id = 0;
        std::string wire_id;
        std::string name;
        std::string path;
        bool enabled = true;
    };

    Json::Value loadSetDefinition(const RegistryEntry& entry) const;
    const RegistryEntry* findRegistryEntry(const std::string& gesture_set_id) const;
    std::optional<int64_t> dbIdForGestureSetWireId(const std::string& wire_id) const;
    std::optional<std::string> wireIdForGestureSetDbId(int64_t db_id) const;
    bool syncRegistryToDatabase(std::string& out_error);
    bool loadDeviceMappingsFromDatabase(std::string& out_error);
    bool persistDeviceMapping(
        const std::string& device_wire_id,
        const std::string& gesture_set_wire_id,
        std::string& out_error);

    mutable std::mutex m_mutex;
    std::vector<RegistryEntry> m_registry;
    std::unordered_map<std::string, std::string> m_deviceToSetWire;
    std::function<std::filesystem::path(const std::string&)> m_resolvePath;
    db::DbClientPtr m_db;
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
