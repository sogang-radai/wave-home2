#include "notifications_controller.h"

#include "../../../app/app_state.h"
#include "notifications_store.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

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

    NotificationsStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listForUser(*user_id)));
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

    NotificationsStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.markAllRead(*user_id)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
