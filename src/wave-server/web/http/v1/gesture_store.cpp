#include "gesture_store.h"

#include <fstream>

#include "../../../core/logger.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace {

Json::Value triggerToFrontend(const json& trigger)
{
    Json::Value out;
    if (trigger.contains("high_threshold"))
        out["highThreshold"] = trigger["high_threshold"].get<double>();
    if (trigger.contains("low_threshold"))
        out["lowThreshold"] = trigger["low_threshold"].get<double>();
    if (trigger.contains("high_hold_ms"))
        out["highHoldMs"] = static_cast<Json::Int64>(trigger["high_hold_ms"].get<int64_t>());
    if (trigger.contains("low_hold_ms"))
        out["lowHoldMs"] = static_cast<Json::Int64>(trigger["low_hold_ms"].get<int64_t>());
    if (trigger.contains("cooldown_ms"))
        out["cooldownMs"] = static_cast<Json::Int64>(trigger["cooldown_ms"].get<int64_t>());
    return out;
}

std::string gestureClassKind(const json& cls)
{
    if (cls.contains("kind") && cls["kind"].is_string())
        return cls["kind"].get<std::string>();
    const int class_id = cls.value("class_id", 0);
    return class_id < 3 ? "state" : "trigger";
}

Json::Value classToFrontend(const json& cls, const std::string& set_id)
{
    Json::Value out;
    out["classId"] = cls.value("class_id", 0);
    out["name"] = cls.value("name", std::string());
    out["kind"] = gestureClassKind(cls);

    const auto thumb = cls.value("thumbnail", std::string());
    if (!thumb.empty())
        out["thumbnail"] = "/gestures/" + set_id + "/" + thumb;
    else
        out["thumbnail"] = Json::nullValue;

    if (cls.contains("trigger") && cls["trigger"].is_object())
        out["trigger"] = triggerToFrontend(cls["trigger"]);

    return out;
}

void appendClassCounts(const json& set_config, Json::Value& item)
{
    if (!set_config.contains("classes") || !set_config["classes"].is_array())
        return;

    const auto& classes = set_config["classes"];
    item["classCount"] = static_cast<Json::UInt>(classes.size());

    Json::UInt trigger_count = 0;
    for (const auto& cls : classes)
    {
        if (gestureClassKind(cls) == "trigger")
            ++trigger_count;
    }
    item["triggerClassCount"] = trigger_count;
}

Json::Value setConfigToFrontend(const json& set_config, const std::string& set_id, const std::string& entry_name, const std::string& entry_path)
{
    Json::Value out;
    out["id"] = set_id;
    out["name"] = set_config.value("name", entry_name);
    out["path"] = entry_path;
    out["description"] = set_config.value("description", std::string());

    if (set_config.contains("model_path") && set_config["model_path"].is_string())
        out["modelPath"] = set_config["model_path"].get<std::string>();
    else
        out["modelPath"] = Json::nullValue;

    Json::Value classes(Json::arrayValue);
    if (set_config.contains("classes") && set_config["classes"].is_array())
    {
        for (const auto& cls : set_config["classes"])
            classes.append(classToFrontend(cls, set_id));
    }
    out["classes"] = classes;
    return out;
}

} // namespace

std::string GestureStore::gestureSetIdFromPath(const std::string& path)
{
    const auto normalized = std::filesystem::path(path);
    const auto parent = normalized.parent_path().filename().string();
    if (!parent.empty())
        return parent;
    return normalized.stem().string();
}

bool GestureStore::load(
    const std::filesystem::path& registry_path,
    const std::function<std::filesystem::path(const std::string&)>& resolve_path,
    std::string& out_error)
{
    std::lock_guard lock(m_mutex);
    m_resolvePath = resolve_path;
    m_registry.clear();

    if (!std::filesystem::exists(registry_path))
    {
        out_error = "gesture registry not found: " + registry_path.string();
        return false;
    }

    try
    {
        json root;
        {
            std::ifstream in(registry_path);
            in >> root;
        }

        if (!root.contains("gesture_sets") || !root["gesture_sets"].is_array())
        {
            out_error = "gesture_sets array missing";
            return false;
        }

        for (const auto& item : root["gesture_sets"])
        {
            RegistryEntry entry;
            entry.name = item.value("name", std::string());
            entry.path = item.value("path", std::string());
            entry.enabled = item.value("enabled", true);
            entry.id = gestureSetIdFromPath(entry.path);
            if (!entry.id.empty())
                m_registry.push_back(std::move(entry));
        }

        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
}

void GestureStore::persistAssignments() const
{
    if (m_assignmentsPath.empty())
        return;

    json root = json::object();
    for (const auto& [device_id, gesture_set_id] : m_assignments)
        root[device_id] = gesture_set_id;

    try
    {
        std::ofstream out(m_assignmentsPath);
        out << root.dump(4);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("GestureStore: failed to persist assignments: {}", e.what());
    }
}

Json::Value GestureStore::listGestureSets() const
{
    std::lock_guard lock(m_mutex);
    Json::Value out(Json::arrayValue);

    for (const auto& entry : m_registry)
    {
        if (!entry.enabled)
            continue;

        Json::Value item;
        item["id"] = entry.id;
        item["name"] = entry.name;
        item["path"] = entry.path;
        item["enabled"] = entry.enabled;

        const auto set_path = m_resolvePath(entry.path);
        if (std::filesystem::exists(set_path))
        {
            try
            {
                json set_config;
                {
                    std::ifstream in(set_path);
                    in >> set_config;
                }
                item["description"] = set_config.value("description", std::string());
                appendClassCounts(set_config, item);
            }
            catch (...)
            {
            }
        }

        out.append(item);
    }

    return out;
}

Json::Value GestureStore::loadSetDefinition(const RegistryEntry& entry) const
{
    const auto set_path = m_resolvePath(entry.path);
    if (!std::filesystem::exists(set_path))
    {
        Json::Value out;
        out["id"] = entry.id;
        out["name"] = entry.name;
        out["path"] = entry.path;
        out["description"] = "";
        out["modelPath"] = Json::nullValue;
        out["classes"] = Json::Value(Json::arrayValue);
        return out;
    }

    json set_config;
    {
        std::ifstream in(set_path);
        in >> set_config;
    }

    return setConfigToFrontend(set_config, entry.id, entry.name, entry.path);
}

Json::Value GestureStore::getGestureSetDefinition(const std::string& gesture_set_id, std::string& code) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& entry : m_registry)
    {
        if (entry.id != gesture_set_id)
            continue;
        return loadSetDefinition(entry);
    }

    code = "NOT_FOUND";
    return Json::Value();
}

Json::Value GestureStore::getRadarGestureSet(const std::string& device_id, std::string& code) const
{
    std::lock_guard lock(m_mutex);
    Json::Value out;
    out["deviceId"] = device_id;

    const auto it = m_assignments.find(device_id);
    if (it == m_assignments.end())
        out["gestureSetId"] = Json::nullValue;
    else
        out["gestureSetId"] = it->second;

    return out;
}

Json::Value GestureStore::setRadarGestureSet(
    const std::string& device_id,
    const std::string& gesture_set_id,
    std::string& code)
{
    std::lock_guard lock(m_mutex);

    if (!gesture_set_id.empty())
    {
        const auto found = std::find_if(m_registry.begin(), m_registry.end(), [&](const RegistryEntry& entry)
        {
            return entry.id == gesture_set_id;
        });
        if (found == m_registry.end())
        {
            code = "NOT_FOUND";
            return Json::Value();
        }
    }

    if (gesture_set_id.empty())
        m_assignments.erase(device_id);
    else
        m_assignments[device_id] = gesture_set_id;

    persistAssignments();

    Json::Value out;
    out["deviceId"] = device_id;
    if (gesture_set_id.empty())
        out["gestureSetId"] = Json::nullValue;
    else
        out["gestureSetId"] = gesture_set_id;
    return out;
}

void GestureStore::setAssignmentsPath(const std::filesystem::path& path)
{
    std::lock_guard lock(m_mutex);
    m_assignmentsPath = path;

    if (!std::filesystem::exists(path))
        return;

    try
    {
        json root;
        {
            std::ifstream in(path);
            in >> root;
        }

        if (!root.is_object())
            return;

        m_assignments.clear();
        for (auto it = root.begin(); it != root.end(); ++it)
        {
            if (it.value().is_string())
                m_assignments[it.key()] = it.value().get<std::string>();
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("GestureStore: failed to load assignments: {}", e.what());
    }
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
