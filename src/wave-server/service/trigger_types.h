#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/json.h"
#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

enum class ExecMode
{
    Once,
    Repeat,
    Toggle,
};

enum class TriggerKind
{
    Gesture,
    DeviceState,
    IrRecv,
};

struct RuleAction
{
    std::string deviceId;
    std::string name;
    json params = json::object();
};

struct RuleSchedule
{
    std::string repeat; // once | daily | weekly
    std::optional<uint32_t> delayMinutes;
    std::optional<std::string> time; // HH:MM
    std::vector<std::string> daysOfWeek;
};

struct Trigger
{
    std::string id;
    TriggerKind kind = TriggerKind::Gesture;
    std::string sourceDeviceId;

    std::string gestureSetPath;
    int32_t classId = -1;

    std::string query;
    std::string op;
    double value = 0.0;

    std::string commandId;
};

struct Rule
{
    std::string id;
    std::string name;
    bool enabled = true;
    std::string triggerId;
    json triggerJson = json(); // inline trigger for API/persist
    std::optional<RuleSchedule> schedule;
    RuleAction action;
    ExecMode execMode = ExecMode::Once;
    uint32_t cooldownMs = 0;
    uint32_t repeatIntervalMs = 0;
};

struct TriggerBinding
{
    std::string triggerId;
    std::string ruleId;
    std::string ruleName;
    ExecMode execMode = ExecMode::Once;
    uint32_t cooldownMs = 0;
    uint32_t repeatIntervalMs = 0;
    RuleAction action;
    int32_t gestureClassId = -1;
};

struct GestureIndexKey
{
    std::string radarDeviceId;
    std::string gestureSetPath;

    bool operator<(const GestureIndexKey& other) const
    {
        if (radarDeviceId != other.radarDeviceId)
            return radarDeviceId < other.radarDeviceId;
        return gestureSetPath < other.gestureSetPath;
    }
};

struct DeviceStateIndexKey
{
    std::string deviceId;
    std::string query;

    bool operator<(const DeviceStateIndexKey& other) const
    {
        if (deviceId != other.deviceId)
            return deviceId < other.deviceId;
        return query < other.query;
    }
};

struct IrRecvIndexKey
{
    std::string deviceId;
    std::string commandId;

    bool operator<(const IrRecvIndexKey& other) const
    {
        if (deviceId != other.deviceId)
            return deviceId < other.deviceId;
        return commandId < other.commandId;
    }
};

struct TriggerIndex
{
    std::unordered_map<std::string, Trigger> triggers;
    std::map<GestureIndexKey, std::vector<TriggerBinding>> gesture;
    std::map<DeviceStateIndexKey, std::vector<TriggerBinding>> deviceState;
    std::map<IrRecvIndexKey, std::vector<TriggerBinding>> irRecv;
    std::vector<Rule> scheduleRules;
};

using TriggerIndexSnapshot = std::shared_ptr<const TriggerIndex>;

ExecMode parseExecMode(const std::string& value);
std::string execModeToString(ExecMode mode);
TriggerKind parseTriggerKind(const std::string& value);
std::string triggerKindToString(TriggerKind kind);
std::string makeTriggerId(const Trigger& trigger);
bool parseTriggerFromJson(const json& value, Trigger& out_trigger, std::string& out_error);
bool parseRuleScheduleFromJson(const json& value, RuleSchedule& out_schedule, std::string& out_error);
json triggerToJson(const Trigger& trigger);
json ruleScheduleToJson(const RuleSchedule& schedule);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
