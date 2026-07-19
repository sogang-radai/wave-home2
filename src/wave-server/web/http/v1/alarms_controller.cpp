#include "alarms_controller.h"

#include <cstdint>
#include <optional>

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../facade/alarms_facade.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    std::optional<int64_t> parse_int64(const std::string& raw)
    {
        if (raw.empty())
            return std::nullopt;
        try
        {
            return std::stoll(raw);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    std::optional<int64_t> require_active_user(
        const HttpRequestPtr& req,
        const HttpResponseCallback& callback)
    {
        auto& state = AppState::get();
        if (!state.db())
        {
            respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
            return std::nullopt;
        }

        SessionStore sessions(state.db());
        SettingsStore settings(state.db());
        const auto user_id = settings.resolveActiveUserId(sessions, req);
        if (!user_id)
        {
            respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
            return std::nullopt;
        }
        return user_id;
    }

    Json::Value without_user_id(const Json::Value& item)
    {
        Json::Value view = item;
        view.removeMember("userId");
        return view;
    }

    Json::Value without_user_ids(const Json::Value& items)
    {
        Json::Value out(Json::arrayValue);
        for (const auto& item : items)
            out.append(without_user_id(item));
        return out;
    }

    drogon::HttpResponsePtr json_response(
        const HttpRequestPtr& req,
        const Json::Value& body,
        const std::string& runtime_id,
        drogon::HttpStatusCode status = drogon::k200OK)
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(status);
        if (!runtime_id.empty())
            attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        return resp;
    }
}

void AlarmsController::listAlarms(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto user_id = require_active_user(req, callback);
    if (!user_id)
        return;

    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string();
    const auto items = AppState::get().runtime().alarms().list(
        *user_id, std::nullopt, runtime_id, AppState::get().db());
    callback(json_response(req, without_user_ids(items), runtime_id));
}

void AlarmsController::createAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto user_id = require_active_user(req, callback);
    if (!user_id)
        return;

    const auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    Json::Value body = *json;
    body["userId"] = static_cast<Json::Int64>(*user_id);
    if (!body.isMember("enabled"))
        body["enabled"] = true;
    if (!body.isMember("daysOfWeek"))
        body["daysOfWeek"] = Json::Value(Json::arrayValue);

    std::string error;
    std::string field;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, &body)
        : std::string();
    const auto created = AppState::get().runtime().alarms().create(
        body, runtime_id, AppState::get().db(), error, field);
    if (created.isNull())
    {
        respondError(callback, 400, "VALIDATION_ERROR", error, field);
        return;
    }

    callback(json_response(req, without_user_id(created), runtime_id, drogon::k201Created));
}

void AlarmsController::updateAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string alarmId)
{
    const auto user_id = require_active_user(req, callback);
    if (!user_id)
        return;

    const auto id = parse_int64(alarmId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "알람을 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    std::string error;
    std::string field;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, json.get())
        : std::string();
    const auto updated = AppState::get().runtime().alarms().update(
        *user_id, *id, *json, runtime_id, AppState::get().db(), error, field);
    if (updated.isNull())
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        respondError(callback, status, status == 404 ? "NOT_FOUND" : "VALIDATION_ERROR", error, field);
        return;
    }

    callback(json_response(req, without_user_id(updated), runtime_id));
}

void AlarmsController::deleteAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string alarmId)
{
    const auto user_id = require_active_user(req, callback);
    if (!user_id)
        return;

    const auto id = parse_int64(alarmId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "알람을 찾을 수 없습니다.");
        return;
    }

    std::string error;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string();
    const auto removed = AppState::get().runtime().alarms().remove(
        *user_id, *id, runtime_id, AppState::get().db(), error);
    if (removed.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    callback(json_response(req, removed, runtime_id));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
