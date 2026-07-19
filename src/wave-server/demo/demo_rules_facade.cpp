#include "demo_rules_facade.h"

#include "../app/app_state.h"
#include "demo_device_backend.h"
#include "demo_session_writes.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    bool matches_demo_rule_filter(const Json::Value& item, const facade::RuleListFilter& filter)
    {
        if (!item.isObject())
            return false;

        const bool has_schedule = item.isMember("schedule") && item["schedule"].isObject()
            && !item["schedule"].isNull() && item["schedule"].isMember("repeat");
        const bool has_trigger = item.isMember("trigger") && item["trigger"].isObject()
            && !item["trigger"].isNull() && !item["trigger"].empty();

        if (filter.device_id)
        {
            const auto action_id = item.isMember("action") && item["action"].isObject()
                ? item["action"].get("deviceId", "").asString()
                : std::string{};
            const auto trigger_id = has_trigger
                ? item["trigger"].get("deviceId", "").asString()
                : std::string{};
            if (action_id != *filter.device_id && trigger_id != *filter.device_id)
                return false;
        }
        if (filter.enabled && item.get("enabled", true).asBool() != *filter.enabled)
            return false;
        if (filter.has_schedule && has_schedule != *filter.has_schedule)
            return false;
        if (filter.has_trigger && has_trigger != *filter.has_trigger)
            return false;
        return true;
    }
}

Json::Value DemoRulesFacade::list(
    const facade::RuleListFilter& filter,
    const std::string& runtime_id,
    int64_t user_id,
    std::string& code)
{
    code.clear();
    ensureDemoSessionSeeded(runtime_id, AppState::get().db());
    Json::Value items(Json::arrayValue);
    for (const auto& raw : demoListRules(runtime_id, user_id))
    {
        if (!matches_demo_rule_filter(raw, filter))
            continue;
        items.append(raw);
    }
    Json::Value body;
    body["items"] = items;
    body["count"] = static_cast<Json::UInt>(items.size());
    return body;
}

Json::Value DemoRulesFacade::get(
    const std::string& rule_id,
    const std::string& runtime_id,
    std::string& code)
{
    ensureDemoSessionSeeded(runtime_id, AppState::get().db());
    for (const auto& item : demoListRules(runtime_id, 0))
    {
        if (item.isObject() && item.get("id", "").asString() == rule_id)
            return item;
    }
    code = "NOT_FOUND";
    return Json::Value();
}

Json::Value DemoRulesFacade::create(
    const Json::Value& body,
    const std::string& runtime_id,
    std::string& code)
{
    ensureDemoSessionSeeded(runtime_id, AppState::get().db());
    return demoCreateRule(runtime_id, body, code);
}

Json::Value DemoRulesFacade::update(
    const std::string& rule_id,
    const Json::Value& body,
    const std::string& runtime_id,
    std::string& code)
{
    ensureDemoSessionSeeded(runtime_id, AppState::get().db());
    return demoUpdateRule(runtime_id, rule_id, body, code);
}

Json::Value DemoRulesFacade::remove(
    const std::string& rule_id,
    const std::string& runtime_id,
    std::string& code)
{
    ensureDemoSessionSeeded(runtime_id, AppState::get().db());
    if (!demoDeleteRule(runtime_id, rule_id))
    {
        code = "NOT_FOUND";
        return Json::Value();
    }
    Json::Value body;
    body["id"] = rule_id;
    return body;
}

Json::Value DemoRulesFacade::setEnabled(
    const std::string& rule_id,
    bool enabled,
    const std::string& runtime_id,
    std::string& code)
{
    Json::Value body;
    body["enabled"] = enabled;
    return update(rule_id, body, runtime_id, code);
}

Json::Value DemoRulesFacade::execute(
    const std::string& rule_id,
    const std::string& runtime_id,
    std::string& code)
{
    auto& state = AppState::get();
    ensureDemoSessionSeeded(runtime_id, state.db());

    Json::Value rule;
    for (const auto& item : demoListRules(runtime_id, 0))
    {
        if (item.isObject() && item.get("id", "").asString() == rule_id)
        {
            rule = item;
            break;
        }
    }
    if (rule.isNull() || !rule.isObject())
    {
        code = "NOT_FOUND";
        return Json::Value();
    }
    if (!rule.get("enabled", true).asBool())
    {
        code = "RULE_DISABLED";
        return Json::Value();
    }
    if (!rule.isMember("action") || !rule["action"].isObject())
    {
        code = "INVALID_RULE";
        return Json::Value();
    }

    const auto device_id = rule["action"].get("deviceId", "").asString();
    const auto action_name = rule["action"].get("name", "").asString();
    Json::Value invoke_body(Json::objectValue);
    invoke_body["demoRuntimeId"] = runtime_id;
    if (rule["action"].isMember("params") && rule["action"]["params"].isObject())
        invoke_body["params"] = rule["action"]["params"];

    DemoDeviceBackend backend(state.db());
    backend.invokeAction(runtime_id, device_id, action_name, invoke_body, code);
    if (!code.empty())
        return Json::Value();

    Json::Value body;
    body["skipped"] = false;
    return body;
}

WAVE_NAMESPACE_END
