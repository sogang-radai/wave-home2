#include "notifications_controller.h"

#include <cstdlib>

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../demo/demo_session_writes.h"
#include "notifications_store.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    drogon::HttpResponsePtr demoResponse(
        const drogon::HttpRequestPtr& req,
        const Json::Value& body,
        const std::string& runtime_id)
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        return resp;
    }

    int parsePositiveInt(const std::string& raw, int fallback)
    {
        if (raw.empty())
            return fallback;
        char* end = nullptr;
        const long value = std::strtol(raw.c_str(), &end, 10);
        if (end == raw.c_str() || value <= 0 || value > 200)
            return fallback;
        return static_cast<int>(value);
    }

    int64_t parseInt64Param(const std::string& raw)
    {
        if (raw.empty())
            return 0;
        char* end = nullptr;
        const long long value = std::strtoll(raw.c_str(), &end, 10);
        if (end == raw.c_str() || value < 0)
            return 0;
        return static_cast<int64_t>(value);
    }
}

void NotificationsController::listNotifications(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SessionStore sessions(state.db());
    SettingsStore settings(state.db());
    const auto user_id = settings.resolveActiveUserId(sessions, req);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    const int limit = parsePositiveInt(req->getParameter("limit"), 20);
    const int64_t before_id = parseInt64Param(req->getParameter("beforeId"));

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        callback(demoResponse(
            req,
            demoListNotifications(runtime_id, *user_id, state.db(), limit, before_id),
            runtime_id));
        return;
    }

    NotificationsStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listForUser(*user_id, limit, before_id)));
}

void NotificationsController::markAllRead(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SessionStore sessions(state.db());
    SettingsStore settings(state.db());
    const auto user_id = settings.resolveActiveUserId(sessions, req);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        callback(demoResponse(
            req,
            demoMarkAllNotificationsRead(runtime_id, *user_id, state.db()),
            runtime_id));
        return;
    }

    NotificationsStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.markAllRead(*user_id)));
}

void NotificationsController::markRead(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    const std::string& notification_id)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SessionStore sessions(state.db());
    SettingsStore settings(state.db());
    const auto user_id = settings.resolveActiveUserId(sessions, req);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    const int64_t id = parseInt64Param(notification_id);
    if (id <= 0)
    {
        respondError(callback, 400, "INVALID_ID", "알림 id가 올바르지 않습니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        const auto item = demoMarkNotificationRead(runtime_id, *user_id, id, state.db());
        if (item.isNull() || !item.isObject())
        {
            respondError(callback, 404, "NOT_FOUND", "알림을 찾을 수 없습니다.");
            return;
        }
        callback(demoResponse(req, item, runtime_id));
        return;
    }

    NotificationsStore store(state.db());
    const auto item = store.markRead(*user_id, id);
    if (item.isNull() || !item.isObject())
    {
        respondError(callback, 404, "NOT_FOUND", "알림을 찾을 수 없습니다.");
        return;
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(item));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
