#include "schedule_tasks_controller.h"

#include <cstdint>
#include <optional>

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../demo/demo_session_writes.h"
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

    drogon::HttpResponsePtr demo_response(
        const HttpRequestPtr& req,
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

void ScheduleTasksController::listTasks(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto user_id = require_active_user(req, callback);
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
    if (const auto done = parse_bool(req->getParameter("done")))
        filter.done = *done;

    internal::ScheduleTasksInternalStore store(AppState::get().db());
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        auto items = demoListScheduleTasks(runtime_id, *user_id, AppState::get().db());
        // Optional filters applied client-side for session copy.
        Json::Value filtered(Json::arrayValue);
        for (const auto& item : items)
        {
            if (!item.isObject())
                continue;
            if (filter.day_of_week && item.get("dayOfWeek", "").asString() != *filter.day_of_week)
                continue;
            if (filter.event_date && item.get("eventDate", "").asString() != *filter.event_date)
                continue;
            if (filter.schedule_kind && item.get("scheduleKind", "").asString() != *filter.schedule_kind)
                continue;
            if (filter.done && item.get("done", false).asBool() != *filter.done)
                continue;
            filtered.append(item);
        }
        callback(demo_response(req, without_user_ids(filtered), runtime_id));
        return;
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(without_user_ids(store.list(filter))));
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

    // Front UI may omit title for insight-sourced tasks; use a safe placeholder when needed.
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
    Json::Value created;
    if (demoVirtualDevicesEnabled())
    {
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        created = demoCreateScheduleTask(runtime_id, body, error, field);
    }
    else if (const auto persisted = internal::ScheduleTasksInternalStore(AppState::get().db()).create(body, error, field))
        created = *persisted;
    if (created.isNull())
    {
        respondError(callback, 400, "INVALID_REQUEST", error, field);
        return;
    }

    if (!demoVirtualDevicesEnabled())
    {
        ActionLogStore(AppState::get().db())
            .record(*user_id, "schedule_task_created", "schedule_task", created["id"].asInt64(), created["category"].asString());
    }

    if (demoVirtualDevicesEnabled())
    {
        callback(demo_response(req, without_user_id(created), runtime_id, drogon::k201Created));
        return;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(without_user_id(created));
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
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
    if (!demoVirtualDevicesEnabled() && json->isMember("done"))
    {
        internal::ScheduleTasksInternalStore store(AppState::get().db());
        if (const auto existing = store.getById(*user_id, *id))
            done_before = (*existing)["done"].asBool();
    }

    Json::Value updated;
    if (demoVirtualDevicesEnabled())
        updated = demoUpdateScheduleTask(runtime_id, *id, *json, error, field);
    else if (const auto persisted =
        internal::ScheduleTasksInternalStore(AppState::get().db()).update(*user_id, *id, *json, error, field))
        updated = *persisted;
    if (updated.isNull())
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        respondError(callback, status, status == 404 ? "NOT_FOUND" : "INVALID_REQUEST", error, field);
        return;
    }

    if (!demoVirtualDevicesEnabled() && done_before)
    {
        const bool done_after = updated["done"].asBool();
        const std::string category = updated["category"].asString();
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

    if (demoVirtualDevicesEnabled())
    {
        callback(demo_response(req, without_user_id(updated), runtime_id));
        return;
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(without_user_id(updated)));
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
    Json::Value removed;
    if (demoVirtualDevicesEnabled())
    {
        if (demoDeleteScheduleTask(runtime_id, *id))
            removed["id"] = static_cast<Json::Int64>(*id);
        else
            error = "세션 일정을 찾을 수 없습니다.";
    }
    else if (const auto persisted =
        internal::ScheduleTasksInternalStore(AppState::get().db()).remove(*user_id, *id, error))
    {
        removed = *persisted;
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
    callback(drogon::HttpResponse::newHttpJsonResponse(removed));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
