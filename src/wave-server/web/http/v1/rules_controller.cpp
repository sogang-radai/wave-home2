#include "rules_controller.h"
#include "../../../db/database.h"

#include <sstream>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../demo/demo_session_writes.h"
#include "../../../facade/rules_facade.h"
#include "../../../device/device_manager.h"
#include "../../../device/device_wire_id.hpp"
#include "../../../service/rule_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

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

    bool require_rule_store(
        const HttpResponseCallback& callback,
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

    service::RuleView enrich_view(dev::DeviceManager& devices, const service::RuleView& view)
    {
        service::RuleView out = view;
        out.actionDeviceName = deviceName(devices, view.rule.action.deviceId);
        if (!view.triggerDeviceName.empty())
            out.triggerDeviceName = deviceName(devices, view.triggerDeviceName);
        return out;
    }

    std::string demo_device_name(const db::DbClientPtr& client, const std::string& wire_id)
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

    bool demo_rule_matches_device(const Json::Value& rule, const std::string& device_id)
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

    Json::Value prepare_demo_rule_view(const Json::Value& rule, const db::DbClientPtr& client)
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
            out["actionDeviceName"] = demo_device_name(client, action_id);

        if (!out.isMember("triggerDeviceName"))
        {
            if (out.isMember("trigger") && out["trigger"].isObject())
            {
                out["triggerDeviceName"] = demo_device_name(
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

void RulesController::listRules(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    auto& state = AppState::get();
    const auto device_filter = req->getParameter("deviceId");
    const bool demo = demoVirtualDevicesEnabled();
    const auto runtime_id = demo ? resolveDemoRuntimeId(req, nullptr) : std::string();

    if (!demo && !require_rule_store(callback, state))
        return;

    facade::RuleListFilter filter;
    if (!device_filter.empty())
        filter.device_id = device_filter;

    std::string code;
    const auto listed = state.runtime().rules().list(filter, runtime_id, 0, code);
    if (!code.empty())
    {
        respondError(callback, 503, code, "룰 목록을 조회할 수 없습니다.");
        return;
    }

    Json::Value body(Json::arrayValue);
    const Json::Value& items =
        (listed.isObject() && listed.isMember("items")) ? listed["items"] : listed;
    for (const auto& item : items)
    {
        if (!item.isObject())
            continue;
        if (demo)
            body.append(prepare_demo_rule_view(item, state.db()));
        else
            body.append(item);
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void RulesController::createRule(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    auto& state = AppState::get();
    const auto json = req->getJsonObject();
    if (!json)
    {
        respondError(callback, 400, "INVALID_RULE", "룰 데이터가 올바르지 않습니다.");
        return;
    }

    Json::Value body = *json;
    const bool demo = demoVirtualDevicesEnabled();
    const auto runtime_id = demo ? resolveDemoRuntimeId(req, &body) : std::string();
    if (demo)
        body["demoRuntimeId"] = runtime_id;
    else if (!require_rule_store(callback, state))
        return;

    std::string code;
    const auto created = state.runtime().rules().create(body, runtime_id, code);
    if (created.isNull() || !code.empty())
    {
        respondError(callback, 400, code.empty() ? "INVALID_RULE" : code, "룰을 생성할 수 없습니다.");
        return;
    }
    Json::Value view = demo ? prepare_demo_rule_view(created, state.db()) : created;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(view);
    resp->setStatusCode(drogon::k201Created);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void RulesController::updateRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId)
{
    auto& state = AppState::get();
    const auto json = req->getJsonObject();
    if (!json)
    {
        respondError(callback, 400, "INVALID_RULE", "룰 데이터가 올바르지 않습니다.");
        return;
    }

    Json::Value body = *json;
    const bool demo = demoVirtualDevicesEnabled();
    const auto runtime_id = demo ? resolveDemoRuntimeId(req, &body) : std::string();
    if (!demo && !require_rule_store(callback, state))
        return;

    std::string code;
    const auto updated = state.runtime().rules().update(ruleId, body, runtime_id, code);
    if (updated.isNull() || !code.empty())
    {
        respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
        return;
    }
    Json::Value view = demo ? prepare_demo_rule_view(updated, state.db()) : updated;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(view);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void RulesController::deleteRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId)
{
    auto& state = AppState::get();
    const bool demo = demoVirtualDevicesEnabled();
    const auto runtime_id = demo ? resolveDemoRuntimeId(req, nullptr) : std::string();
    if (!demo && !require_rule_store(callback, state))
        return;

    std::string code;
    const auto removed = state.runtime().rules().remove(ruleId, runtime_id, code);
    if (removed.isNull() || !code.empty())
    {
        respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
        return;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(removed);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void RulesController::setRuleEnabled(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId)
{
    auto& state = AppState::get();
    const auto json = req->getJsonObject();
    if (!json || !json->isMember("enabled") || !(*json)["enabled"].isBool())
    {
        respondError(callback, 400, "INVALID_RULE", "enabled 값이 필요합니다.", "enabled");
        return;
    }

    const bool demo = demoVirtualDevicesEnabled();
    const auto runtime_id = demo ? resolveDemoRuntimeId(req, json.get()) : std::string();
    if (!demo && !require_rule_store(callback, state))
        return;

    std::string code;
    const auto updated = state.runtime().rules().setEnabled(
        ruleId, (*json)["enabled"].asBool(), runtime_id, code);
    if (updated.isNull() || !code.empty())
    {
        respondError(callback, 404, "NOT_FOUND", "룰을 찾을 수 없습니다.");
        return;
    }
    Json::Value view = demo ? prepare_demo_rule_view(updated, state.db()) : updated;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(view);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void RulesController::executeRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId)
{
    auto& state = AppState::get();
    const bool demo = demoVirtualDevicesEnabled();
    const auto runtime_id = demo ? resolveDemoRuntimeId(req, nullptr) : std::string();
    if (!demo && !require_rule_store(callback, state))
        return;

    std::string code;
    const auto body = state.runtime().rules().execute(ruleId, runtime_id, code);
    if (code == "NOT_FOUND")
        respondError(callback, 404, code, "룰을 찾을 수 없습니다.");
    else if (code == "RULE_DISABLED")
        respondError(callback, 409, code, "비활성화된 룰입니다.");
    else if (code == "INVALID_RULE")
        respondError(callback, 400, code, "룰 액션이 없습니다.");
    else if (!code.empty())
        respondError(callback, 500, code, "룰 실행에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        if (!runtime_id.empty())
            attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
    }
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
