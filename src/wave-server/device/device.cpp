#include "device.h"

#include <cstdio>
#include <stdexcept>

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    json default_error_json()
    {
        return json{
            {"0", "success"}
        };
    }

    void validate_errors_object(const json& errors)
    {
        for (const auto& [key, value] : errors.items())
        {
            if (!value.is_string())
                throw std::invalid_argument("device config field 'errors' values must be strings");
            (void)key;
        }
    }

    void validate_device_config(const json& config)
    {
        if (!config.is_object())
            throw std::invalid_argument("device config must be a JSON object");

        if (!config.contains("id") || !config["id"].is_string())
            throw std::invalid_argument("device config requires string field 'id'");

        const auto& id = config["id"].get<std::string>();
        if (id.size() != 16 || parseDeviceID(id) == 0)
            throw std::invalid_argument("device config field 'id' must be a 16-character hex string");

        if (!config.contains("name") || !config["name"].is_string())
            throw std::invalid_argument("device config requires string field 'name'");

        if (config.contains("room_id"))
        {
            if (!config["room_id"].is_string())
                throw std::invalid_argument("device config field 'room_id' must be a string");

            const auto& roomId = config["room_id"].get<std::string>();
            if (roomId.size() != 16 || parseRoomID(roomId) == 0)
                throw std::invalid_argument("device config field 'room_id' must be a 16-character hex string");
        }

        if (config.contains("description") && !config["description"].is_string())
            throw std::invalid_argument("device config field 'description' must be a string");

        if (config.contains("vendor") && !config["vendor"].is_string())
            throw std::invalid_argument("device config field 'vendor' must be a string");

        if (config.contains("model") && !config["model"].is_string())
            throw std::invalid_argument("device config field 'model' must be a string");

        if (!config.contains("enabled") || !config["enabled"].is_boolean())
            throw std::invalid_argument("device config requires boolean field 'enabled'");

        if (!config.contains("class") || !config["class"].is_string())
            throw std::invalid_argument("device config requires string field 'class'");

        if (config.contains("errors"))
        {
            if (!config["errors"].is_object())
                throw std::invalid_argument("device config field 'errors' must be an object");

            validate_errors_object(config["errors"]);
        }
    }
}

// ============================================================================
// DeviceID
// ============================================================================

DeviceID generateDeviceID(uint64_t seed)
{
    uint64_t value = seed;
    value ^= 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

DeviceID parseDeviceID(std::string_view id)
{
    if (id.size() != 16)
        return 0;

    for (char ch : id)
    {
        const bool isHex =
            (ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f') ||
            (ch >= 'A' && ch <= 'F');
        if (!isHex)
            return 0;
    }

    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%.*s", static_cast<int>(id.size()), id.data());
    return std::strtoull(buffer, nullptr, 16);
}

std::string deviceIDToString(DeviceID id)
{
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(id));
    return std::string(buffer);
}

// ============================================================================
// RoomID
// ============================================================================

RoomID generateRoomID(uint64_t seed)
{
    return generateDeviceID(seed ^ 0x6a09e667f3bcc908ULL);
}

RoomID parseRoomID(std::string_view id)
{
    return parseDeviceID(id);
}

std::string roomIDToString(RoomID id)
{
    return deviceIDToString(id);
}

// ============================================================================
// Device
// ============================================================================

Device::Device() :
    m_state(DeviceState::Uninitialized),
    m_baseInfo(),
    m_errorJson(default_error_json())
{
}

const DeviceBaseInfo& Device::getBaseInfo() const
{
    return m_baseInfo;
}

std::string_view Device::getName() const
{
    return m_baseInfo.name;
}

std::string_view Device::getDescription() const
{
    return m_baseInfo.description;
}

std::string_view Device::getVendor() const
{
    return m_baseInfo.vendor;
}

std::string_view Device::getModel() const
{
    return m_baseInfo.model;
}

std::string_view Device::getClass() const
{
    return m_baseInfo.className;
}

bool Device::isEnabled() const
{
    return m_baseInfo.enabled;
}

DeviceID Device::getId() const
{
    return m_baseInfo.id;
}

RoomID Device::getRoomId() const
{
    return m_baseInfo.roomId;
}

DeviceState Device::getState() const
{
    return m_state.load();
}

std::string_view Device::getErrorString(int error_code) const
{
    static constexpr const char kUnknownError[] = "unknown error";

    const std::string key = std::to_string(error_code);
    if (m_errorJson.contains(key) && m_errorJson[key].is_string())
        return m_errorJson[key].get_ref<const std::string&>();

    return kUnknownError;
}

const DeviceBaseInfo& Device::loadBaseConfig(const json& config)
{
    validate_device_config(config);

    m_baseInfo.id = parseDeviceID(config["id"].get<std::string>());
    m_baseInfo.roomId = config.contains("room_id")
        ? parseRoomID(config["room_id"].get<std::string>())
        : 0;
    m_baseInfo.name = config["name"].get<std::string>();
    m_baseInfo.description = config.value("description", "");
    m_baseInfo.vendor = config.value("vendor", "");
    m_baseInfo.model = config.value("model", "");
    m_baseInfo.className = config["class"].get<std::string>();
    m_baseInfo.enabled = config["enabled"].get<bool>();

    m_errorJson = default_error_json();
    if (config.contains("errors"))
        m_errorJson.update(config["errors"]);

    return m_baseInfo;
}

// ============================================================================
// Queryable
// ============================================================================

const std::vector<Query>& Queryable::enumerateQueries() const
{
    return m_queries;
}

const Query* Queryable::findQuery(std::string_view name) const
{
    const auto it = m_queryMap.find(std::string(name));
    if (it == m_queryMap.end())
        return nullptr;
    return it->second;
}

// ============================================================================
// Actionable
// ============================================================================

const std::vector<Action>& Actionable::enumerateActions() const
{
    return m_actions;
}

const Action* Actionable::findAction(std::string_view name) const
{
    const auto it = m_actionMap.find(std::string(name));
    if (it == m_actionMap.end())
        return nullptr;
    return it->second;
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
