#include "alarms_controller.h"

#include <cstdint>
#include <optional>

#include "../../../app/app_state.h"
#include "../../../service/alarm_manager.h"
#include "../internal/alarms_internal_store.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    std::optional<int64_t> parseInt64(const std::string& raw)
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

    std::optional<int64_t> requireActiveUser(
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

    Json::Value withoutUserId(const Json::Value& item)
    {
        Json::Value view = item;
        view.removeMember("userId");
        return view;
    }

    Json::Value withoutUserIds(const Json::Value& items)
    {
        Json::Value out(Json::arrayValue);
        for (const auto& item : items)
            out.append(withoutUserId(item));
        return out;
    }

    void notifyAlarmManager()
    {
        service::AlarmManager::get().reconcile();
    }
}

void AlarmsController::listAlarms(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = requireActiveUser(req, callback);
    if (!user_id)
        return;

    internal::AlarmListFilter filter;
    filter.user_id = *user_id;

    internal::AlarmsInternalStore store(AppState::get().db());
    callback(drogon::HttpResponse::newHttpJsonResponse(withoutUserIds(store.listAlarms(filter))));
}

void AlarmsController::createAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = requireActiveUser(req, callback);
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

    internal::AlarmsInternalStore store(AppState::get().db());
    std::string error;
    std::string field;
    const auto created = store.createAlarm(body, error, field);
    if (created.isNull())
    {
        respondError(callback, 400, "VALIDATION_ERROR", error, field);
        return;
    }

    notifyAlarmManager();
    auto resp = drogon::HttpResponse::newHttpJsonResponse(withoutUserId(created));
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void AlarmsController::updateAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string alarmId)
{
    const auto user_id = requireActiveUser(req, callback);
    if (!user_id)
        return;

    const auto id = parseInt64(alarmId);
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

    internal::AlarmsInternalStore store(AppState::get().db());
    std::string error;
    std::string field;
    const auto updated = store.updateAlarm(*user_id, *id, *json, error, field);
    if (updated.isNull())
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        respondError(callback, status, status == 404 ? "NOT_FOUND" : "VALIDATION_ERROR", error, field);
        return;
    }

    notifyAlarmManager();
    callback(drogon::HttpResponse::newHttpJsonResponse(withoutUserId(updated)));
}

void AlarmsController::deleteAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string alarmId)
{
    const auto user_id = requireActiveUser(req, callback);
    if (!user_id)
        return;

    const auto id = parseInt64(alarmId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "알람을 찾을 수 없습니다.");
        return;
    }

    internal::AlarmsInternalStore store(AppState::get().db());
    std::string error;
    const auto removed = store.deleteAlarm(*user_id, *id, error);
    if (removed.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    notifyAlarmManager();
    callback(drogon::HttpResponse::newHttpJsonResponse(removed));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
