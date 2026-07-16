#include "weekly_plan_controller.h"
#include "../../../db/database.h"

#include "../../../app/app_state.h"
#include "insights_store.h"
#include "session_store.h"
#include "settings_store.h"
#include "weekly_plan_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace
{
    std::optional<int64_t> resolve_user_id(const drogon::HttpRequestPtr& req, db::DbClientPtr client)
    {
        SessionStore sessions(client);
        SettingsStore settings(client);
        return settings.resolveActiveUserId(sessions, req);
    }
}

void WeeklyPlanController::weeklyReport(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolve_user_id(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    WeeklyPlanStore store(client);
    const auto period_start = req->getParameter("periodStart");
    const auto body = store.weeklyReport(*user_id, period_start);
    if (body.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "주간 계획 리포트가 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void WeeklyPlanController::recommendations(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolve_user_id(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    const auto ref_date = InsightsStore::reference_date(client);
    InsightsStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(
        store.list(*user_id, std::string("weekly_plan"), ref_date, std::nullopt, std::nullopt, std::nullopt)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
