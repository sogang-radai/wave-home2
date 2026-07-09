#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

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

    void setAssignmentsPath(const std::filesystem::path& path);

    Json::Value listGestureSets() const;
    Json::Value getGestureSetDefinition(const std::string& gesture_set_id, std::string& code) const;
    Json::Value getRadarGestureSet(const std::string& device_id, std::string& code) const;
    Json::Value setRadarGestureSet(const std::string& device_id, const std::string& gesture_set_id, std::string& code);

private:
    struct RegistryEntry
    {
        std::string id;
        std::string name;
        std::string path;
        bool enabled = true;
    };

    static std::string gestureSetIdFromPath(const std::string& path);
    Json::Value loadSetDefinition(const RegistryEntry& entry) const;
    void persistAssignments() const;

    mutable std::mutex m_mutex;
    std::vector<RegistryEntry> m_registry;
    std::unordered_map<std::string, std::string> m_assignments;
    std::function<std::filesystem::path(const std::string&)> m_resolvePath;
    std::filesystem::path m_assignmentsPath;
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
