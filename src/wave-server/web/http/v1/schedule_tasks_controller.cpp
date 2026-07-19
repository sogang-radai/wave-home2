#include "schedule_tasks_controller.h"

#include <cstdint>
#include <optional>

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../facade/schedule_tasks_facade.h"
#include "../internal/schedule_tasks_internal_store.h"
#include "action_log_store.h"
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

    std::optional<bool> parse_bool(const std::string& raw)
    {
        if (raw == "true" || raw == "1")
            return true;
        if (raw == "false" || raw == "0")
            return false;
        return std::nullopt;
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

void ScheduleTasksController::listTasks(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto user_id = require_active_user(req, callback);
    if (!user_id)
        return;

    facade::ScheduleTaskListFilter filter;
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
    if (const auto done = parse_bool(req->getParameter("done")))
        filter.done = *done;

    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string();
    const auto items = AppState::get().runtime().scheduleTasks().list(
        filter, runtime_id, AppState::get().db());
    callback(json_response(req, without_user_ids(items), runtime_id));
}

void ScheduleTasksController::createTask(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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
    if (!body.isMember("createdBy"))
        body["createdBy"] = "user";
    if (!body.isMember("scheduleKind") && !body.isMember("eventDate"))
        body["scheduleKind"] = "weekly";

    if ((!body.isMember("title") || !body["title"].isString() || body["title"].asString().empty()) &&
        body.isMember("sourceInsightId") && !body["sourceInsightId"].isNull())
    {
        body["title"] = "인사이트 일정";
    }

    std::string error;
    std::string field;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, &body)
        : std::string();
    const auto created = AppState::get().runtime().scheduleTasks().create(
        body, runtime_id, AppState::get().db(), error, field);
    if (!created)
    {
        respondError(callback, 400, "INVALID_REQUEST", error, field);
        return;
    }

    if (runtime_id.empty())
    {
        ActionLogStore(AppState::get().db())
            .record(
                *user_id,
                "schedule_task_created",
                "schedule_task",
                (*created)["id"].asInt64(),
                (*created)["category"].asString());
    }

    callback(json_response(req, without_user_id(*created), runtime_id, drogon::k201Created));
}

void ScheduleTasksController::updateTask(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string taskId)
{
    const auto user_id = require_active_user(req, callback);
    if (!user_id)
        return;

    const auto id = parse_int64(taskId);
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

    std::string error;
    std::string field;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, json.get())
        : std::string();

    std::optional<bool> done_before;
    if (runtime_id.empty() && json->isMember("done"))
    {
        internal::ScheduleTasksInternalStore store(AppState::get().db());
        if (const auto existing = store.getById(*user_id, *id))
            done_before = (*existing)["done"].asBool();
    }

    const auto updated = AppState::get().runtime().scheduleTasks().update(
        *user_id, *id, *json, runtime_id, AppState::get().db(), error, field);
    if (!updated)
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        respondError(callback, status, status == 404 ? "NOT_FOUND" : "INVALID_REQUEST", error, field);
        return;
    }

    if (runtime_id.empty() && done_before)
    {
        const bool done_after = (*updated)["done"].asBool();
        const std::string category = (*updated)["category"].asString();
        if (!*done_before && done_after)
        {
            ActionLogStore(AppState::get().db())
                .record(*user_id, "schedule_task_completed", "schedule_task", *id, category);
        }
        else if (*done_before && !done_after)
        {
            ActionLogStore(AppState::get().db())
                .record(*user_id, "schedule_task_uncompleted", "schedule_task", *id, category);
        }
    }

    callback(json_response(req, without_user_id(*updated), runtime_id));
}

void ScheduleTasksController::deleteTask(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string taskId)
{
    const auto user_id = require_active_user(req, callback);
    if (!user_id)
        return;

    const auto id = parse_int64(taskId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "일정을 찾을 수 없습니다.");
        return;
    }

    std::string error;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string();
    const auto removed = AppState::get().runtime().scheduleTasks().remove(
        *user_id, *id, runtime_id, AppState::get().db(), error);
    if (!removed)
    {
        respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    callback(json_response(req, *removed, runtime_id));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
