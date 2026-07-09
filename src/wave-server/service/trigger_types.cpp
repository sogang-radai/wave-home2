#include "trigger_types.h"

#include <sstream>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    uint64_t fnv1a64(std::string_view data)
    {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : data)
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return hash;
    }
}

ExecMode parseExecMode(const std::string& value)
{
    if (value == "repeat")
        return ExecMode::Repeat;
    if (value == "toggle")
        return ExecMode::Toggle;
    return ExecMode::Once;
}

std::string execModeToString(ExecMode mode)
{
    switch (mode)
    {
    case ExecMode::Repeat:
        return "repeat";
    case ExecMode::Toggle:
        return "toggle";
    default:
        return "once";
    }
}

TriggerKind parseTriggerKind(const std::string& value)
{
    if (value == "device_state")
        return TriggerKind::DeviceState;
    if (value == "ir_recv")
        return TriggerKind::IrRecv;
    return TriggerKind::Gesture;
}

std::string triggerKindToString(TriggerKind kind)
{
    switch (kind)
    {
    case TriggerKind::DeviceState:
        return "device_state";
    case TriggerKind::IrRecv:
        return "ir_recv";
    default:
        return "gesture";
    }
}

std::string makeTriggerId(const Trigger& trigger)
{
    std::ostringstream oss;
    oss << triggerKindToString(trigger.kind) << '|' << trigger.sourceDeviceId;
    switch (trigger.kind)
    {
    case TriggerKind::Gesture:
        oss << '|' << trigger.gestureSetPath << '|' << trigger.classId;
        break;
    case TriggerKind::DeviceState:
        oss << '|' << trigger.query << '|' << trigger.op << '|' << trigger.value;
        break;
    case TriggerKind::IrRecv:
        oss << '|' << trigger.commandId;
        break;
    }
    return "trg_" + std::to_string(fnv1a64(oss.str()));
}

bool parseTriggerFromJson(const json& value, Trigger& out_trigger, std::string& out_error)
{
    if (!value.is_object())
    {
        out_error = "trigger must be an object";
        return false;
    }

    if (!value.contains("kind") || !value["kind"].is_string())
    {
        out_error = "trigger.kind is required";
        return false;
    }

    out_trigger = Trigger{};
    out_trigger.kind = parseTriggerKind(value["kind"].get<std::string>());

    if (!value.contains("deviceId") || !value["deviceId"].is_string())
    {
        out_error = "trigger.deviceId is required";
        return false;
    }
    out_trigger.sourceDeviceId = value["deviceId"].get<std::string>();

    switch (out_trigger.kind)
    {
    case TriggerKind::Gesture:
        if (!value.contains("gestureSetPath") || !value["gestureSetPath"].is_string())
        {
            out_error = "trigger.gestureSetPath is required";
            return false;
        }
        if (!value.contains("classId") || !value["classId"].is_number_integer())
        {
            out_error = "trigger.classId is required";
            return false;
        }
        out_trigger.gestureSetPath = value["gestureSetPath"].get<std::string>();
        out_trigger.classId = value["classId"].get<int32_t>();
        break;
    case TriggerKind::DeviceState:
        if (!value.contains("query") || !value["query"].is_string())
        {
            out_error = "trigger.query is required";
            return false;
        }
        if (!value.contains("op") || !value["op"].is_string())
        {
            out_error = "trigger.op is required";
            return false;
        }
        if (!value.contains("value") || !value["value"].is_number())
        {
            out_error = "trigger.value is required";
            return false;
        }
        out_trigger.query = value["query"].get<std::string>();
        out_trigger.op = value["op"].get<std::string>();
        out_trigger.value = value["value"].get<double>();
        break;
    case TriggerKind::IrRecv:
        if (!value.contains("commandId") || !value["commandId"].is_string())
        {
            out_error = "trigger.commandId is required";
            return false;
        }
        out_trigger.commandId = value["commandId"].get<std::string>();
        break;
    }

    out_trigger.id = makeTriggerId(out_trigger);
    return true;
}

bool parseRuleScheduleFromJson(const json& value, RuleSchedule& out_schedule, std::string& out_error)
{
    if (!value.is_object())
    {
        out_error = "schedule must be an object";
        return false;
    }

    if (!value.contains("repeat") || !value["repeat"].is_string())
    {
        out_error = "schedule.repeat is required";
        return false;
    }

    out_schedule = RuleSchedule{};
    out_schedule.repeat = value["repeat"].get<std::string>();

    if (value.contains("delayMinutes") && !value["delayMinutes"].is_null())
        out_schedule.delayMinutes = value["delayMinutes"].get<uint32_t>();
    if (value.contains("time") && value["time"].is_string())
        out_schedule.time = value["time"].get<std::string>();
    if (value.contains("daysOfWeek") && value["daysOfWeek"].is_array())
    {
        for (const auto& day : value["daysOfWeek"])
        {
            if (day.is_string())
                out_schedule.daysOfWeek.push_back(day.get<std::string>());
        }
    }

    return true;
}

json triggerToJson(const Trigger& trigger)
{
    json out = json::object();
    out["kind"] = triggerKindToString(trigger.kind);
    out["deviceId"] = trigger.sourceDeviceId;
    switch (trigger.kind)
    {
    case TriggerKind::Gesture:
        out["gestureSetPath"] = trigger.gestureSetPath;
        out["classId"] = trigger.classId;
        break;
    case TriggerKind::DeviceState:
        out["query"] = trigger.query;
        out["op"] = trigger.op;
        out["value"] = trigger.value;
        break;
    case TriggerKind::IrRecv:
        out["commandId"] = trigger.commandId;
        break;
    }
    return out;
}

json ruleScheduleToJson(const RuleSchedule& schedule)
{
    json out = json::object();
    out["repeat"] = schedule.repeat;
    if (schedule.delayMinutes)
        out["delayMinutes"] = *schedule.delayMinutes;
    if (schedule.time)
        out["time"] = *schedule.time;
    if (!schedule.daysOfWeek.empty())
        out["daysOfWeek"] = schedule.daysOfWeek;
    return out;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
