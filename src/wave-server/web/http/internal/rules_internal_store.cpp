#include "rules_internal_store.h"

#include <sstream>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../device/device_manager.h"
#include "../../../service/rule_store.h"
#include "devices_internal_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    ws::json json_from_request(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::istringstream stream(Json::writeString(builder, value));
        return ws::json::parse(stream);
    }

    Json::Value json_to_response(const ws::json& value)
    {
        Json::CharReaderBuilder reader;
        Json::Value out;
        std::string errors;
        std::istringstream stream(value.dump());
        Json::parseFromStream(reader, stream, &out, &errors);
        return out;
    }

    Json::Value rule_view_to_json(const service::RuleView& view)
    {
        ws::json payload = ws::json::object();
        payload["id"] = view.rule.id;
        payload["name"] = view.rule.name;
        payload["enabled"] = view.rule.enabled;
        payload["cooldownMs"] = view.rule.cooldownMs;
        payload["repeatIntervalMs"] = view.rule.repeatIntervalMs;
        payload["execMode"] = service::execModeToString(view.rule.execMode);
        payload["action"] = ws::json::object();
        payload["action"]["deviceId"] = view.rule.action.deviceId;
        payload["action"]["name"] = view.rule.action.name;
        payload["action"]["params"] = view.rule.action.params;

        if (!view.rule.triggerId.empty())
            payload["trigger"] = view.rule.triggerJson;
        else
            payload["trigger"] = nullptr;

        if (view.rule.schedule)
            payload["schedule"] = service::ruleScheduleToJson(*view.rule.schedule);
        else
            payload["schedule"] = nullptr;

        payload["actionDeviceName"] = view.actionDeviceName;
        if (!view.triggerDeviceName.empty())
            payload["triggerDeviceName"] = view.triggerDeviceName;

        return json_to_response(payload);
    }

    service::RuleView enrich_view(dev::DeviceManager& devices, const service::RuleView& view)
    {
        service::RuleView out = view;
        const auto device_name = [&](const std::string& external_id) {
            const auto id = dev::parseDeviceID(external_id);
            if (id == 0)
                return external_id;
            if (auto* device = devices.findDevice(id))
                return std::string(device->getName());
            return external_id;
        };
        out.actionDeviceName = device_name(view.rule.action.deviceId);
        if (!view.triggerDeviceName.empty())
            out.triggerDeviceName = device_name(view.triggerDeviceName);
        return out;
    }

    bool require_rule_store(std::string& code)
    {
        if (!AppState::get().hasRuleStore())
        {
            code = "AUTOMATION_UNAVAILABLE";
            return false;
        }
        return true;
    }

    std::optional<int64_t> room_id_for_device(const std::string& device_id)
    {
        DevicesInternalStore store(AppState::get().db());
        DeviceListFilter filter;
        filter.enabled = std::nullopt;
        std::string code;
        const Json::Value listed = store.listDevices(filter, code);
        if (!code.empty())
            return std::nullopt;

        for (const auto& item : listed["items"])
        {
            if (!item.isObject() || item.get("id", "").asString() != device_id)
                continue;
            const auto& room = item["room"];
            if (!room.isObject())
                return std::nullopt;
            if (room["id"].isInt64())
                return room["id"].asInt64();
            if (room["id"].isInt())
                return room["id"].asInt();
            if (room["id"].isString())
            {
                try
                {
                    return std::stoll(room["id"].asString());
                }
                catch (...)
                {
                    return std::nullopt;
                }
            }
        }
        return std::nullopt;
    }

    bool matches_rule_filter(const service::RuleView& view, const RuleListFilter& filter)
    {
        if (filter.device_id)
        {
            if (view.rule.action.deviceId != *filter.device_id)
            {
                const auto trigger_device = view.rule.triggerJson.value("deviceId", std::string());
                if (trigger_device != *filter.device_id)
                    return false;
            }
        }

        if (filter.enabled && view.rule.enabled != *filter.enabled)
            return false;
        if (filter.has_schedule && static_cast<bool>(view.rule.schedule) != *filter.has_schedule)
            return false;
        if (filter.has_trigger && (view.rule.triggerId.empty() == *filter.has_trigger))
            return false;

        if (filter.room_id)
        {
            const auto room_id = room_id_for_device(view.rule.action.deviceId);
            if (!room_id || *room_id != *filter.room_id)
                return false;
        }
        return true;
    }
}

Json::Value RulesInternalStore::listRules(const RuleListFilter& filter, std::string& code) const
{
    if (!require_rule_store(code))
        return Json::Value();

    auto& state = AppState::get();
    const auto views = filter.device_id
        ? state.ruleStore().listForDevice(*filter.device_id)
        : state.ruleStore().list();

    Json::Value items(Json::arrayValue);
    for (const auto& view : views)
    {
        const auto enriched = enrich_view(state.deviceManager, view);
        if (!matches_rule_filter(enriched, filter))
            continue;
        items.append(rule_view_to_json(enriched));
    }

    Json::Value body;
    body["items"] = items;
    body["count"] = static_cast<Json::UInt>(items.size());
    return body;
}

Json::Value RulesInternalStore::getRule(const std::string& rule_id, std::string& code) const
{
    if (!require_rule_store(code))
        return Json::Value();

    const auto view = AppState::get().ruleStore().get(rule_id);
    if (!view)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    return rule_view_to_json(enrich_view(AppState::get().deviceManager, *view));
}

Json::Value RulesInternalStore::createRule(const Json::Value& body, std::string& code) const
{
    if (!require_rule_store(code))
        return Json::Value();

    ws::json payload;
    try
    {
        payload = json_from_request(body);
    }
    catch (...)
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }

    std::string error;
    if (!service::RuleStore::validate_payload(payload, error))
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }

    try
    {
        const auto view = enrich_view(
            AppState::get().deviceManager,
            AppState::get().ruleStore().createAsync(payload).get());
        return rule_view_to_json(view);
    }
    catch (...)
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }
}

Json::Value RulesInternalStore::updateRule(
    const std::string& rule_id,
    const Json::Value& body,
    std::string& code) const
{
    if (!require_rule_store(code))
        return Json::Value();

    ws::json patch;
    try
    {
        patch = json_from_request(body);
    }
    catch (...)
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }

    try
    {
        const auto view = enrich_view(
            AppState::get().deviceManager,
            AppState::get().ruleStore().updateAsync(rule_id, patch).get());
        return rule_view_to_json(view);
    }
    catch (...)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }
}

Json::Value RulesInternalStore::deleteRule(const std::string& rule_id, std::string& code) const
{
    if (!require_rule_store(code))
        return Json::Value();

    try
    {
        (void)AppState::get().ruleStore().deleteAsync(rule_id).get();
        Json::Value body;
        body["id"] = rule_id;
        return body;
    }
    catch (...)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }
}

Json::Value RulesInternalStore::setRuleEnabled(
    const std::string& rule_id,
    bool enabled,
    std::string& code) const
{
    if (!require_rule_store(code))
        return Json::Value();

    try
    {
        const auto view = enrich_view(
            AppState::get().deviceManager,
            AppState::get().ruleStore().setEnabledAsync(rule_id, enabled).get());
        return rule_view_to_json(view);
    }
    catch (...)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }
}

Json::Value RulesInternalStore::executeRule(const std::string& rule_id, std::string& code) const
{
    if (!require_rule_store(code))
        return Json::Value();

    auto& state = AppState::get();
    if (!state.automationReady())
    {
        code = "AUTOMATION_UNAVAILABLE";
        return Json::Value();
    }

    const auto rule = state.ruleStore().get(rule_id);
    if (!rule)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    if (!rule->rule.enabled)
    {
        code = "RULE_DISABLED";
        return Json::Value();
    }

    service::ActionJob job;
    job.targetDeviceId = rule->rule.action.deviceId;
    job.actionName = rule->rule.action.name;
    job.params = rule->rule.action.params;
    job.execMode = rule->rule.execMode;
    job.repeatIntervalMs = rule->rule.repeatIntervalMs;
    job.ruleId = rule->rule.id;
    job.sourceRef = rule->rule.schedule ? "schedule:" + rule->rule.id : "rule:" + rule->rule.id;
    job.logMessage = "수동 룰 실행: " + rule->rule.name;

    const auto result = state.actionQueue().enqueueAndWait(job, 5000).get();
    if (!result.code.empty())
    {
        code = result.code;
        return Json::Value();
    }

    Json::Value body;
    body["skipped"] = false;
    return body;
}

Json::Value RulesInternalStore::toolSchedule(const Json::Value& body, std::string& code) const
{
    if (!body.isMember("roomId") || !body.isMember("device") || !body.isMember("action") || !body.isMember("schedule"))
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }

    DevicesInternalStore devices(AppState::get().db());
    const int64_t room_id = body["roomId"].isInt64()
        ? body["roomId"].asInt64()
        : static_cast<int64_t>(body["roomId"].asInt());
    std::optional<int64_t> user_id;
    if (body.isMember("userId"))
    {
        user_id = body["userId"].isInt64() ? body["userId"].asInt64() : static_cast<int64_t>(body["userId"].asInt());
    }

    const auto resolved = devices.resolveDeviceByName(room_id, body["device"].asString(), user_id, code);
    if (!resolved)
        return Json::Value();

    Json::Value rule_body;
    rule_body["name"] = body.isMember("name") ? body["name"] : Json::Value("예약: " + resolved->device_name);
    rule_body["enabled"] = true;
    rule_body["trigger"] = Json::nullValue;
    rule_body["schedule"] = body["schedule"];
    rule_body["execMode"] = body.isMember("execMode") ? body["execMode"] : "once";
    rule_body["cooldownMs"] = body.isMember("cooldownMs") ? body["cooldownMs"] : 0;
    if (body.isMember("repeatIntervalMs"))
        rule_body["repeatIntervalMs"] = body["repeatIntervalMs"];

    Json::Value action;
    action["deviceId"] = resolved->device_id;
    action["name"] = body["action"];
    action["params"] = body.isMember("params") ? body["params"] : Json::Value(Json::objectValue);
    rule_body["action"] = action;

    const auto created = createRule(rule_body, code);
    if (!code.empty())
        return Json::Value();

    Json::Value response;
    response["ok"] = true;
    response["rule"] = created;
    return response;
}

Json::Value RulesInternalStore::toolScheduleList(const Json::Value& body, std::string& code) const
{
    RuleListFilter filter;
    filter.has_schedule = true;
    if (body.isMember("enabled") && body["enabled"].isBool())
        filter.enabled = body["enabled"].asBool();
    if (body.isMember("roomId"))
    {
        filter.room_id = body["roomId"].isInt64()
            ? body["roomId"].asInt64()
            : static_cast<int64_t>(body["roomId"].asInt());
    }
    if (body.isMember("deviceId"))
        filter.device_id = body["deviceId"].asString();

    return listRules(filter, code);
}

Json::Value RulesInternalStore::toolScheduleCancel(const Json::Value& body, std::string& code) const
{
    if (!body.isMember("ruleId"))
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }

    const auto rule_id = body["ruleId"].asString();
    const auto existing = getRule(rule_id, code);
    if (!code.empty())
        return Json::Value();

    const auto removed = deleteRule(rule_id, code);
    if (!code.empty())
        return Json::Value();

    Json::Value response;
    response["ok"] = true;
    response["ruleId"] = rule_id;
    response["name"] = existing.get("name", "");
    return response;
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
