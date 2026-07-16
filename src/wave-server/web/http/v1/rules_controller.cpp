#include "rules_controller.h"
#include "../../../db/database.h"

#include <sstream>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../demo/demo_session_writes.h"
#include "../../../device/device_manager.h"
#include "../../../device/device_wire_id.hpp"
#include "../../../service/rule_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace
{
    ws::json jsonFromRequest(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::istringstream stream(Json::writeString(builder, value));
        return ws::json::parse(stream);
    }

    Json::Value jsonToResponse(const ws::json& value)
    {
        Json::CharReaderBuilder reader;
        Json::Value out;
        std::string errors;
        std::istringstream stream(value.dump());
        Json::parseFromStream(reader, stream, &out, &errors);
        return out;
    }

    Json::Value ruleViewToJson(const service::RuleView& view)
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

        return jsonToResponse(payload);
    }

    bool requireRuleStore(
        const std::function<void(const drogon::HttpResponsePtr&)>& callback,
        AppState& state)
    {
        if (!state.hasRuleStore())
        {
            respondError(callback, 503, "AUTOMATION_UNAVAILABLE", "룰 저장소를 사용할 수 없습니다.");
            return false;
        }
        return true;
    }

    std::string deviceName(dev::DeviceManager& devices, const std::string& external_id)
    {
        const auto id = dev::parseDeviceID(external_id);
        if (id == 0)
            return external_id;
        if (auto* device = devices.findDevice(id))
            return std::string(device->getName());
        return external_id;
    }

    service::RuleView enrichView(dev::DeviceManager& devices, const service::RuleView& view)
    {
        service::RuleView out = view;
        out.actionDeviceName = deviceName(devices, view.rule.action.deviceId);
        if (!view.triggerDeviceName.empty())
            out.triggerDeviceName = deviceName(devices, view.triggerDeviceName);
        return out;
    }

    std::string demoDeviceName(const db::DbClientPtr& client, const std::string& wire_id)
    {
        if (!client || wire_id.empty())
            return wire_id;
        const auto db_id = dev::dbIdForWireId(client, wire_id);
        if (!db_id)
            return wire_id;
        auto rows = client->execSqlSync(
            "SELECT name FROM device WHERE id = ? AND archived = 0 LIMIT 1",
            *db_id);
        if (rows.empty())
            return wire_id;
        return rows[0]["name"].as<std::string>();
    }

    bool demoRuleMatchesDevice(const Json::Value& rule, const std::string& device_id)
    {
        if (device_id.empty())
            return true;
        if (rule.isMember("action") && rule["action"].isObject()
            && rule["action"].get("deviceId", "").asString() == device_id)
        {
            return true;
        }
        if (rule.isMember("trigger") && rule["trigger"].isObject()
            && rule["trigger"].get("deviceId", "").asString() == device_id)
        {
            return true;
        }
        return false;
    }

    Json::Value prepareDemoRuleView(const Json::Value& rule, const db::DbClientPtr& client)
    {
        Json::Value out = rule;
        out.removeMember("demoRuntimeId");
        if (!out.isMember("trigger") || out["trigger"].isNull()
            || (out["trigger"].isObject() && out["trigger"].empty()))
        {
            out["trigger"] = Json::nullValue;
        }
        if (!out.isMember("schedule"))
            out["schedule"] = Json::nullValue;
        else if (out["schedule"].isObject())
            out["schedule"] = demoNormalizeRuleSchedule(out["schedule"]);
        if (!out.isMember("cooldownMs"))
            out["cooldownMs"] = 0;
        if (!out.isMember("execMode"))
            out["execMode"] = "once";
        if (!out.isMember("repeatIntervalMs"))
            out["repeatIntervalMs"] = Json::nullValue;
        if (!out.isMember("enabled"))
            out["enabled"] = true;

        const auto action_id = out.isMember("action") && out["action"].isObject()
            ? out["action"].get("deviceId", "").asString()
            : std::string{};
        if (!out.isMember("actionDeviceName") || out["actionDeviceName"].asString().empty())
            out["actionDeviceName"] = demoDeviceName(client, action_id);

        if (!out.isMember("triggerDeviceName"))
        {
            if (out.isMember("trigger") && out["trigger"].isObject())
            {
                out["triggerDeviceName"] = demoDeviceName(
                    client, out["trigger"].get("deviceId", "").asString());
            }
            else
            {
                out["triggerDeviceName"] = Json::nullValue;
            }
        }
        return out;
    }
}

void RulesController::listRules(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    const auto device_filter = req->getParameter("deviceId");

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        const auto client = state.db();
        ensureDemoSessionSeeded(runtime_id, client);
        Json::Value body(Json::arrayValue);
        for (const auto& item : demoListRules(runtime_id, 0))
        {
            if (!item.isObject() || !demoRuleMatchesDevice(item, device_filter))
                continue;
            body.append(prepareDemoRuleView(item, client));
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    if (!requireRuleStore(callback, state))
        return;

    const auto views = device_filter.empty()
        ? state.ruleStore().list()
        : state.ruleStore().listForDevice(device_filter);

    Json::Value body(Json::arrayValue);
    for (const auto& view : views)
        body.append(ruleViewToJson(enrichView(state.deviceManager, view)));

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void RulesController::createRule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    const auto json = req->getJsonObject();
    if (!json)
    {
        respondError(callback, 400, "INVALID_RULE", "룰 데이터가 올바르지 않습니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        Json::Value body = *json;
        const auto runtime_id = resolveDemoRuntimeId(req, &body);
        body["demoRuntimeId"] = runtime_id;
        ensureDemoSessionSeeded(runtime_id, state.db());
        std::string code;
        const auto created = demoCreateRule(runtime_id, body, code);
        if (created.isNull())
        {
            respondError(callback, 400, code.empty() ? "INVALID_RULE" : code, "룰을 생성할 수 없습니다.");
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(prepareDemoRuleView(created, state.db()));
        resp->setStatusCode(drogon::k201Created);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    if (!requireRuleStore(callback, state))
        return;

    ws::json payload;
    try
    {
        payload = jsonFromRequest(*json);
    }
    catch (...)
    {
        respondError(callback, 400, "INVALID_RULE", "룰 데이터가 올바르지 않습니다.");
        return;
    }

    std::string error;
    if (!service::RuleStore::validatePayload(payload, error))
    {
        respondError(callback, 400, "INVALID_RULE", error);
        return;
    }

    auto future = state.ruleStore().createAsync(payload);
    try
    {
        const auto view = enrichView(state.deviceManager, future.get());
        auto resp = drogon::HttpResponse::newHttpJsonResponse(ruleViewToJson(view));
        resp->setStatusCode(drogon::k201Created);
        callback(resp);
    }
    catch (const std::exception& e)
    {
        respondError(callback, 400, "INVALID_RULE", e.what());
    }
}

void RulesController::updateRule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    auto& state = AppState::get();
    const auto json = req->getJsonObject();
    if (!json)
    {
        respondError(callback, 400, "INVALID_RULE", "룰 데이터가 올바르지 않습니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        Json::Value body = *json;
        const auto runtime_id = resolveDemoRuntimeId(req, &body);
        ensureDemoSessionSeeded(runtime_id, state.db());
        std::string code;
        const auto updated = demoUpdateRule(runtime_id, ruleId, body, code);
        if (!code.empty())
        {
            respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(prepareDemoRuleView(updated, state.db()));
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    if (!requireRuleStore(callback, state))
        return;

    ws::json patch;
    try
    {
        patch = jsonFromRequest(*json);
    }
    catch (...)
    {
        respondError(callback, 400, "INVALID_RULE", "룰 데이터가 올바르지 않습니다.");
        return;
    }

    auto future = state.ruleStore().updateAsync(ruleId, patch);
    try
    {
        const auto view = enrichView(state.deviceManager, future.get());
        callback(drogon::HttpResponse::newHttpJsonResponse(ruleViewToJson(view)));
    }
    catch (const std::exception& e)
    {
        respondError(callback, 404, "NOT_FOUND", e.what());
    }
}

void RulesController::deleteRule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    auto& state = AppState::get();

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, state.db());
        if (!demoDeleteRule(runtime_id, ruleId))
        {
            respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
            return;
        }
        Json::Value body;
        body["id"] = ruleId;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    if (!requireRuleStore(callback, state))
        return;

    auto future = state.ruleStore().deleteAsync(ruleId);
    try
    {
        (void)future.get();
        Json::Value body;
        body["id"] = ruleId;
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    }
    catch (const std::exception&)
    {
        respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
    }
}

void RulesController::setRuleEnabled(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    auto& state = AppState::get();
    const auto json = req->getJsonObject();
    if (!json || !json->isMember("enabled") || !(*json)["enabled"].isBool())
    {
        respondError(callback, 400, "INVALID_RULE", "enabled 값이 필요합니다.", "enabled");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        Json::Value body;
        body["enabled"] = (*json)["enabled"].asBool();
        const auto runtime_id = resolveDemoRuntimeId(req, &body);
        ensureDemoSessionSeeded(runtime_id, state.db());
        std::string code;
        const auto updated = demoUpdateRule(runtime_id, ruleId, body, code);
        if (!code.empty())
        {
            respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(prepareDemoRuleView(updated, state.db()));
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    if (!requireRuleStore(callback, state))
        return;

    auto future = state.ruleStore().setEnabledAsync(ruleId, (*json)["enabled"].asBool());
    try
    {
        const auto view = enrichView(state.deviceManager, future.get());
        callback(drogon::HttpResponse::newHttpJsonResponse(ruleViewToJson(view)));
    }
    catch (const std::exception&)
    {
        respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
    }
}

void RulesController::executeRule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    auto& state = AppState::get();

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, state.db());
        Json::Value rule;
        for (const auto& item : demoListRules(runtime_id, 0))
        {
            if (item.isObject() && item.get("id", "").asString() == ruleId)
            {
                rule = item;
                break;
            }
        }
        if (rule.isNull() || !rule.isObject())
        {
            respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
            return;
        }
        if (!rule.get("enabled", true).asBool())
        {
            respondError(callback, 409, "RULE_DISABLED", "비활성화된 룰입니다.");
            return;
        }
        if (!rule.isMember("action") || !rule["action"].isObject())
        {
            respondError(callback, 400, "INVALID_RULE", "룰 액션이 없습니다.");
            return;
        }

        const auto device_id = rule["action"].get("deviceId", "").asString();
        const auto action_name = rule["action"].get("name", "").asString();
        Json::Value invoke_body(Json::objectValue);
        invoke_body["demoRuntimeId"] = runtime_id;
        if (rule["action"].isMember("params") && rule["action"]["params"].isObject())
            invoke_body["params"] = rule["action"]["params"];

        DemoDeviceBackend backend(state.db());
        std::string code;
        backend.invokeAction(runtime_id, device_id, action_name, invoke_body, code);
        if (!code.empty())
        {
            respondError(callback, 500, code, "룰 실행에 실패했습니다.");
            return;
        }

        Json::Value body;
        body["skipped"] = false;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    if (!requireRuleStore(callback, state))
        return;

    if (!state.automationReady())
    {
        respondError(callback, 503, "AUTOMATION_UNAVAILABLE", "액션 큐를 사용할 수 없습니다.");
        return;
    }

    const auto rule = state.ruleStore().get(ruleId);
    if (!rule)
    {
        respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
        return;
    }

    if (!rule->rule.enabled)
    {
        respondError(callback, 409, "RULE_DISABLED", "비활성화된 룰입니다.");
        return;
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

    auto future = state.actionQueue().enqueueAndWait(job, 5000);
    const auto result = future.get();
    if (!result.code.empty())
    {
        respondError(callback, 500, result.code, "룰 실행에 실패했습니다.");
        return;
    }

    Json::Value body;
    body["skipped"] = false;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
