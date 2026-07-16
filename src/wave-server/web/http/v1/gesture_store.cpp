#include "gesture_store.h"
#include "../../../db/database.h"

#include <fstream>

#include "../../../core/logger.h"
#include "../../../device/device_wire_id.hpp"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace {

    Json::Value trigger_to_frontend(const json& trigger)
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

    std::string gesture_class_kind(const json& cls)
    {
    if (cls.contains("kind") && cls["kind"].is_string())
        return cls["kind"].get<std::string>();
    const int class_id = cls.value("class_id", 0);
    return class_id < 3 ? "state" : "trigger";
    }

    Json::Value class_to_frontend(const json& cls, const std::string& asset_set_dir)
    {
    Json::Value out;
    out["classId"] = cls.value("class_id", 0);
    out["name"] = cls.value("name", std::string());
    out["kind"] = gesture_class_kind(cls);

    const auto thumb = cls.value("thumbnail", std::string());
    if (!thumb.empty())
        out["thumbnail"] = "/gestures/" + asset_set_dir + "/" + thumb;
    else
        out["thumbnail"] = Json::nullValue;

    if (cls.contains("trigger") && cls["trigger"].is_object())
        out["trigger"] = trigger_to_frontend(cls["trigger"]);

    return out;
    }

    void append_class_counts(const json& set_config, Json::Value& item)
    {
    if (!set_config.contains("classes") || !set_config["classes"].is_array())
        return;

    const auto& classes = set_config["classes"];
    item["classCount"] = static_cast<Json::UInt>(classes.size());

    Json::UInt trigger_count = 0;
    for (const auto& cls : classes)
    {
        if (gesture_class_kind(cls) == "trigger")
            ++trigger_count;
    }
    item["triggerClassCount"] = trigger_count;
    }

    Json::Value set_config_to_frontend(
    const json& set_config,
    const std::string& set_wire_id,
    const std::string& entry_name,
    const std::string& entry_path)
    {
    const auto asset_set_dir = std::filesystem::path(entry_path).parent_path().filename().string();

    Json::Value out;
    out["id"] = set_wire_id;
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
            classes.append(class_to_frontend(cls, asset_set_dir));
    }
    out["classes"] = classes;
    return out;
    }

    } // namespace

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

        int64_t db_id = 0;
        for (const auto& item : root["gesture_sets"])
        {
            if (!item.contains("id") || !item["id"].is_string() || item["id"].get<std::string>().empty())
            {
                out_error = "gesture_sets[].id is required";
                return false;
            }

            RegistryEntry entry;
            entry.db_id = ++db_id;
            entry.wire_id = item["id"].get<std::string>();
            entry.name = item.value("name", std::string());
            entry.path = item.value("path", std::string());
            entry.enabled = item.value("enabled", true);
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

void GestureStore::setDatabaseClient(const db::DbClientPtr& client)
{
    std::lock_guard lock(m_mutex);
    m_db = client;
}

bool GestureStore::syncFromDatabase(std::string& out_error, const bool read_only)
{
    std::lock_guard lock(m_mutex);
    if (!m_db)
    {
        out_error = "database client is not set";
        return false;
    }

    if (!read_only && !syncRegistryToDatabase(out_error))
        return false;

    return loadDeviceMappingsFromDatabase(out_error);
}

bool GestureStore::syncRegistryToDatabase(std::string& out_error)
{
    try
    {
        for (const auto& entry : m_registry)
        {
            m_db->execSqlSync(
                "INSERT INTO gesture_set (id, name, archived) VALUES (?, ?, ?) "
                "ON CONFLICT(id) DO UPDATE SET name = excluded.name, archived = excluded.archived",
                entry.db_id,
                entry.name,
                entry.enabled ? 0 : 1);
        }
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
}

bool GestureStore::loadDeviceMappingsFromDatabase(std::string& out_error)
{
    try
    {
        m_deviceToSetWire.clear();
        auto rows = m_db->execSqlSync(
            "SELECT gdm.device_id, gdm.gesture_set_id, d.name "
            "FROM gesture_device_map gdm "
            "JOIN device d ON d.id = gdm.device_id");

        for (const auto& row : rows)
        {
            const int64_t device_db_id = row["device_id"].as<int64_t>();
            const int64_t set_db_id = row["gesture_set_id"].as<int64_t>();
            const std::string device_name = row["name"].as<std::string>();

            const auto set_wire = wireIdForGestureSetDbId(set_db_id);
            if (!set_wire)
                continue;

            const auto device_wire = dev::wireIdForDbRow(device_db_id, device_name);
            if (device_wire.empty())
                continue;

            m_deviceToSetWire[device_wire] = *set_wire;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
}

const GestureStore::RegistryEntry* GestureStore::findRegistryEntry(const std::string& gesture_set_id) const
{
    for (const auto& entry : m_registry)
    {
        if (entry.wire_id == gesture_set_id)
            return &entry;

        const auto& path = entry.path;
        const auto first = path.find('/');
        if (first == std::string::npos)
            continue;
        const auto second = path.find('/', first + 1);
        if (second == std::string::npos)
            continue;
        if (path.substr(first + 1, second - first - 1) == gesture_set_id)
            return &entry;
    }
    return nullptr;
}

std::optional<int64_t> GestureStore::dbIdForGestureSetWireId(const std::string& wire_id) const
{
    if (const auto* entry = findRegistryEntry(wire_id))
        return entry->db_id;
    return std::nullopt;
}

std::optional<std::string> GestureStore::wireIdForGestureSetDbId(int64_t db_id) const
{
    for (const auto& entry : m_registry)
    {
        if (entry.db_id == db_id)
            return entry.wire_id;
    }
    return std::nullopt;
}

bool GestureStore::persistDeviceMapping(
    const std::string& device_wire_id,
    const std::string& gesture_set_wire_id,
    std::string& out_error)
{
    if (!m_db)
    {
        out_error = "database client is not set";
        return false;
    }

    const auto device_db_id = dev::dbIdForWireId(m_db, device_wire_id);
    if (!device_db_id)
    {
        out_error = "device not found";
        return false;
    }

    try
    {
        if (gesture_set_wire_id.empty())
        {
            m_db->execSqlSync("DELETE FROM gesture_device_map WHERE device_id = ?", *device_db_id);
            return true;
        }

        const auto set_db_id = dbIdForGestureSetWireId(gesture_set_wire_id);
        if (!set_db_id)
        {
            out_error = "gesture set not found";
            return false;
        }

        m_db->execSqlSync(
            "INSERT INTO gesture_device_map (device_id, gesture_set_id) VALUES (?, ?) "
            "ON CONFLICT(device_id) DO UPDATE SET gesture_set_id = excluded.gesture_set_id",
            *device_db_id,
            *set_db_id);
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
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
        item["id"] = entry.wire_id;
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
                append_class_counts(set_config, item);
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
        out["id"] = entry.wire_id;
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

    return set_config_to_frontend(set_config, entry.wire_id, entry.name, entry.path);
}

Json::Value GestureStore::getGestureSetDefinition(const std::string& gesture_set_id, std::string& code) const
{
    std::lock_guard lock(m_mutex);
    if (const auto* entry = findRegistryEntry(gesture_set_id))
        return loadSetDefinition(*entry);

    code = "NOT_FOUND";
    return Json::Value();
}

Json::Value GestureStore::getRadarGestureSet(const std::string& device_id, std::string& code) const
{
    std::lock_guard lock(m_mutex);
    Json::Value out;
    out["deviceId"] = device_id;

    const auto it = m_deviceToSetWire.find(device_id);
    if (it == m_deviceToSetWire.end())
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

    const RegistryEntry* resolved_entry = nullptr;
    if (!gesture_set_id.empty())
    {
        resolved_entry = findRegistryEntry(gesture_set_id);
        if (!resolved_entry)
        {
            code = "NOT_FOUND";
            return Json::Value();
        }
    }

    const std::string wire_set_id = resolved_entry ? resolved_entry->wire_id : std::string();

    std::string persist_error;
    if (!persistDeviceMapping(device_id, wire_set_id, persist_error))
    {
        LOG_WARN("GestureStore: failed to persist device mapping: {}", persist_error);
        code = "PERSIST_FAILED";
        return Json::Value();
    }

    if (wire_set_id.empty())
        m_deviceToSetWire.erase(device_id);
    else
        m_deviceToSetWire[device_id] = wire_set_id;

    Json::Value out;
    out["deviceId"] = device_id;
    if (wire_set_id.empty())
        out["gestureSetId"] = Json::nullValue;
    else
        out["gestureSetId"] = wire_set_id;
    return out;
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
