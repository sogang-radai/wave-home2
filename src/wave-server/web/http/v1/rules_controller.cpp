#include "rules_controller.h"

#include <sstream>

#include "../../../core/json.h"
#include "../../../app/app_state.h"
#include "../../../device/device_manager.h"
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
}

void RulesController::listRules(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!requireRuleStore(callback, state))
        return;

    const auto device_filter = req->getParameter("deviceId");
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
    if (!requireRuleStore(callback, state))
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        respondError(callback, 400, "INVALID_RULE", "룰 데이터가 올바르지 않습니다.");
        return;
    }

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
    if (!requireRuleStore(callback, state))
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        respondError(callback, 400, "INVALID_RULE", "룰 데이터가 올바르지 않습니다.");
        return;
    }

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
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    auto& state = AppState::get();
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
    if (!requireRuleStore(callback, state))
        return;

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("enabled") || !(*json)["enabled"].isBool())
    {
        respondError(callback, 400, "INVALID_RULE", "enabled 값이 필요합니다.", "enabled");
        return;
    }

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
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    auto& state = AppState::get();
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
