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
    std::optional<int64_t> resolve_user_id(const HttpRequestPtr& req, db::DbClientPtr client)
    {
        SessionStore sessions(client);
        SettingsStore settings(client);
        return settings.resolveActiveUserId(sessions, req);
    }
}

void WeeklyPlanController::weeklyReport(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

void WeeklyPlanController::recommendations(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

    // "자동화 규칙 적용" 은 별도로 새 콘텐츠를 생성하지 않는다 - 오늘의 sleep_report/
    // power 인사이트에서 이미 만들어진 실행 제안(kind=action, 각 surface 당 정확히
    // automation_rule 1개 + schedule_task 1개)을 그대로 모아서 보여주는 뷰(view)일
    // 뿐이다. 예전엔 별도 weekly_plan surface 를 새로 생성해서 보여줬으나, 그건
    // 실제로 있는 액션과 다른 내용을 지어내는 셈이라 요구사항에 맞지 않는다.
    const auto ref_date = InsightsStore::reference_date(client);
    InsightsStore store(client);
    Json::Value items(Json::arrayValue);
    for (const auto& surface : {"sleep_report", "power"})
    {
        const auto surface_items = store.list(
            *user_id, std::string(surface), ref_date, std::string("action"), std::nullopt, std::nullopt);
        for (const auto& item : surface_items)
            items.append(item);
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(items));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
