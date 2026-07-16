#include "alarms_controller.h"

#include <cstdint>
#include <optional>

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../demo/demo_session_writes.h"
#include "../../../service/alarm_manager.h"
#include "../internal/alarms_internal_store.h"
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
        const drogon::HttpRequestPtr& req,
        const std::function<void(const drogon::HttpResponsePtr&)>& callback)
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

    void notify_alarm_manager()
    {
        service::AlarmManager::get().reconcile();
    }

    drogon::HttpResponsePtr demo_response(
        const drogon::HttpRequestPtr& req,
        const Json::Value& body,
        const std::string& runtime_id,
        drogon::HttpStatusCode status = drogon::k200OK)
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(status);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        return resp;
    }
}

void AlarmsController::listAlarms(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = require_active_user(req, callback);
    if (!user_id)
        return;

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        callback(demo_response(
            req,
            without_user_ids(demoListAlarms(runtime_id, *user_id, AppState::get().db())),
            runtime_id));
        return;
    }

    internal::AlarmListFilter filter;
    filter.user_id = *user_id;

    internal::AlarmsInternalStore store(AppState::get().db());
    callback(drogon::HttpResponse::newHttpJsonResponse(without_user_ids(store.listAlarms(filter))));
}

void AlarmsController::createAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
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
    const auto created = demoVirtualDevicesEnabled()
        ? demoCreateAlarm(runtime_id, body, AppState::get().db(), error, field)
        : internal::AlarmsInternalStore(AppState::get().db()).createAlarm(body, error, field);
    if (created.isNull())
    {
        respondError(callback, 400, "VALIDATION_ERROR", error, field);
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        callback(demo_response(req, without_user_id(created), runtime_id, drogon::k201Created));
        return;
    }

    notify_alarm_manager();
    auto resp = drogon::HttpResponse::newHttpJsonResponse(without_user_id(created));
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void AlarmsController::updateAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string alarmId)
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
    const auto updated = demoVirtualDevicesEnabled()
        ? demoUpdateAlarm(runtime_id, *id, *json, error, field)
        : internal::AlarmsInternalStore(AppState::get().db()).updateAlarm(*user_id, *id, *json, error, field);
    if (updated.isNull())
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        respondError(callback, status, status == 404 ? "NOT_FOUND" : "VALIDATION_ERROR", error, field);
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        callback(demo_response(req, without_user_id(updated), runtime_id));
        return;
    }

    notify_alarm_manager();
    callback(drogon::HttpResponse::newHttpJsonResponse(without_user_id(updated)));
}

void AlarmsController::deleteAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string alarmId)
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
    Json::Value removed;
    if (demoVirtualDevicesEnabled())
    {
        if (demoDeleteAlarm(runtime_id, *id))
            removed["id"] = static_cast<Json::Int64>(*id);
        else
            error = "세션 알람을 찾을 수 없습니다.";
    }
    else
    {
        removed = internal::AlarmsInternalStore(AppState::get().db()).deleteAlarm(*user_id, *id, error);
    }
    if (removed.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        callback(demo_response(req, removed, runtime_id));
        return;
    }

    notify_alarm_manager();
    callback(drogon::HttpResponse::newHttpJsonResponse(removed));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
