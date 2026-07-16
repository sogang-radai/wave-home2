#include "dashboard_controller.h"
#include "../../../db/database.h"

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../demo/demo_session_writes.h"
#include "dashboard_store.h"
#include "insights_store.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace
{
    std::optional<int64_t> resolve_user_id(const HttpRequestPtr& req, db::DbClientPtr client)
    {
        SessionStore sessions(client);
        SettingsStore settings(client);
        return settings.resolveActiveUserId(sessions, req);
    }

    bool require_db(
        const HttpResponseCallback& callback,
        db::DbClientPtr& client_out)
    {
        client_out = AppState::get().db();
        if (!client_out)
        {
            respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
            return false;
        }
        return true;
    }

    std::optional<int64_t> require_active_user(
        const HttpRequestPtr& req,
        const HttpResponseCallback& callback,
        db::DbClientPtr client)
    {
        const auto user_id = resolve_user_id(req, client);
        if (!user_id)
        {
            respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
            return std::nullopt;
        }
        return user_id;
    }
}

void DashboardController::dailyMessage(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    db::DbClientPtr client;
    if (!require_db(callback, client))
        return;

    const auto user_id = require_active_user(req, callback, client);
    if (!user_id)
        return;

    InsightsStore store(client);
    const auto body = store.dashboardDailyMessage(*user_id);
    if (body.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "대시보드 메시지가 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void DashboardController::currentState(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    db::DbClientPtr client;
    if (!require_db(callback, client))
        return;

    if (!require_active_user(req, callback, client))
        return;

    DashboardStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.currentState()));
}

void DashboardController::upcomingAlarms(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    db::DbClientPtr client;
    if (!require_db(callback, client))
        return;

    const auto user_id = require_active_user(req, callback, client);
    if (!user_id)
        return;

    DashboardStore store(client);
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        const auto alarms = demoListAlarms(runtime_id, *user_id, client, true);
        // Reuse store helper by synthesizing upcoming from session alarms via store API shape.
        // Keep response compatible: filter enabled alarms already applied.
        Json::Value upcoming = store.upcomingAlarmsFromItems(alarms);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(upcoming);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(store.upcomingAlarms(*user_id)));
}

void DashboardController::activeGestureRules(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    db::DbClientPtr client;
    if (!require_db(callback, client))
        return;

    const auto user_id = require_active_user(req, callback, client);
    if (!user_id)
        return;

    DashboardStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.activeGestureRules(*user_id)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
