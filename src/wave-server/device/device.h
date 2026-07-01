#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <atomic>

#include "../core/json.h"
#include "room.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

using DeviceID = uint64_t;

DeviceID generateDeviceID(uint64_t seed);
DeviceID parseDeviceID(std::string_view id);
std::string deviceIDToString(DeviceID id);

// ============================================================================
// Device
// ============================================================================

enum class DeviceState
{
    Uninitialized,
    Initializing,
    Running,
    ShuttingDown,
    Stopped,
};

class Device
{
public:
    Device();
    virtual ~Device() = default;

    virtual int init(const json& config) = 0;
    virtual void shutdown() = 0;

    std::string_view getName() const;
    std::string_view getDescription() const;
    virtual std::string_view getClass() const = 0;
    bool isEnabled() const;

    DeviceID getId() const;
    RoomID getRoomId() const;

    DeviceState getState() const;

    virtual std::string_view getErrorString(int error_code) const;

protected:
    void loadBaseConfig(const json& config);

    std::atomic<DeviceState> m_state;
    DeviceID m_id;
    RoomID m_roomId;
    std::string m_name;
    std::string m_description;
    bool m_enabled;
    json m_errorJson;
};

// ============================================================================
// Queryable
// ============================================================================

struct Query
{
    enum Type
    {
        Interface,
        Json
    };

    Type type;
    std::string name;
    std::string description;
    json paramsSchema;
};

class Queryable
{
public:
    virtual ~Queryable() = default;

    virtual const std::vector<Query>& enumerateQueries() const;
    virtual const Query* findQuery(std::string_view name) const;

    virtual json query(std::string_view name, const json& params) = 0;
    virtual std::future<json> queryAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) = 0;

protected:
    std::vector<Query> m_queries;
    std::unordered_map<std::string, const Query*> m_queryMap;
};

// ============================================================================
// Actionable
// ============================================================================

struct Action
{
    enum Type
    {
        Interface, // for extendability, not used yet
        Json
    };

    Type type;
    std::string name;
    std::string description;
    json paramsSchema;
};

class Actionable
{
public:
    virtual ~Actionable() = default;

    virtual const std::vector<Action>& enumerateActions() const;
    virtual const Action* findAction(std::string_view name) const;

    virtual int invoke(std::string_view name, const json& params) = 0;
    virtual std::future<int> invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) = 0;

protected:
    std::vector<Action> m_actions;
    std::unordered_map<std::string, const Action*> m_actionMap;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
