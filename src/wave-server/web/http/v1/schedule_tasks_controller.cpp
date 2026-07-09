#include "schedule_tasks_controller.h"

#include <cstdint>
#include <optional>

#include "../../../app/app_state.h"
#include "../internal/schedule_tasks_internal_store.h"
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

    std::optional<bool> parseBool(const std::string& raw)
    {
        if (raw == "true" || raw == "1")
            return true;
        if (raw == "false" || raw == "0")
            return false;
        return std::nullopt;
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
}

void ScheduleTasksController::listTasks(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = requireActiveUser(req, callback);
    if (!user_id)
        return;

    internal::ScheduleTaskListFilter filter;
    filter.user_id = *user_id;
    if (!req->getParameter("dayOfWeek").empty())
        filter.day_of_week = req->getParameter("dayOfWeek");
    if (!req->getParameter("eventDate").empty())
        filter.event_date = req->getParameter("eventDate");
    if (!req->getParameter("scheduleKind").empty())
        filter.schedule_kind = req->getParameter("scheduleKind");
    if (!req->getParameter("from").empty())
        filter.from = req->getParameter("from");
    if (!req->getParameter("to").empty())
        filter.to = req->getParameter("to");
    if (const auto done = parseBool(req->getParameter("done")))
        filter.done = *done;

    internal::ScheduleTasksInternalStore store(AppState::get().db());
    callback(drogon::HttpResponse::newHttpJsonResponse(withoutUserIds(store.list(filter))));
}

void ScheduleTasksController::createTask(
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
    if (!body.isMember("createdBy"))
        body["createdBy"] = "user";
    if (!body.isMember("scheduleKind") && !body.isMember("eventDate"))
        body["scheduleKind"] = "weekly";

    // Front UI may omit title for insight-sourced tasks; use a safe placeholder when needed.
    if ((!body.isMember("title") || !body["title"].isString() || body["title"].asString().empty()) &&
        body.isMember("sourceInsightId") && !body["sourceInsightId"].isNull())
    {
        body["title"] = "인사이트 일정";
    }

    internal::ScheduleTasksInternalStore store(AppState::get().db());
    std::string error;
    std::string field;
    const auto created = store.create(body, error, field);
    if (!created)
    {
        respondError(callback, 400, "INVALID_REQUEST", error, field);
        return;
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(withoutUserId(*created));
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void ScheduleTasksController::updateTask(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string taskId)
{
    const auto user_id = requireActiveUser(req, callback);
    if (!user_id)
        return;

    const auto id = parseInt64(taskId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "일정을 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    internal::ScheduleTasksInternalStore store(AppState::get().db());
    std::string error;
    std::string field;
    const auto updated = store.update(*user_id, *id, *json, error, field);
    if (!updated)
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        respondError(callback, status, status == 404 ? "NOT_FOUND" : "INVALID_REQUEST", error, field);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(withoutUserId(*updated)));
}

void ScheduleTasksController::deleteTask(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string taskId)
{
    const auto user_id = requireActiveUser(req, callback);
    if (!user_id)
        return;

    const auto id = parseInt64(taskId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "일정을 찾을 수 없습니다.");
        return;
    }

    internal::ScheduleTasksInternalStore store(AppState::get().db());
    std::string error;
    const auto removed = store.remove(*user_id, *id, error);
    if (!removed)
    {
        respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*removed));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
