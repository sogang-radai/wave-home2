#include "sleep_controller.h"
#include "../../../db/database.h"

#include "../../../app/app_state.h"
#include "session_store.h"
#include "settings_store.h"
#include "sleep_store.h"

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

void SleepController::todaySummary(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getTodaySummary(*user_id)));
}

void SleepController::todayPlan(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getTodayPlan(*user_id)));
}

void SleepController::todayPhoneUsage(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    (void)req;
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getTodayPhoneUsage()));
}

void SleepController::todayAutomationSummary(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getTodayAutomationSummary(*user_id)));
}

void SleepController::dailySessions(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

    const auto date = req->getParameter("date");
    if (date.empty())
    {
        respondError(callback, 400, "INVALID_DATE", "date는 YYYY-MM-DD 형식이어야 합니다.", "date");
        return;
    }

    SleepStore store(client);
    const auto body = store.getDailySessions(*user_id, date);
    if (body.isNull() || body.empty())
    {
        respondError(callback, 404, "NOT_FOUND", "해당 날짜의 수면 기록이 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void SleepController::dailyReport(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

    const auto date = req->getParameter("date");
    if (date.empty())
    {
        respondError(callback, 400, "INVALID_DATE", "date는 YYYY-MM-DD 형식이어야 합니다.", "date");
        return;
    }

    const auto session_id = req->getParameter("sessionId");

    SleepStore store(client);
    const auto body = store.getDailyReport(*user_id, date, session_id);
    if (body.isNull() || body.empty())
    {
        respondError(callback, 404, "NOT_FOUND", "해당 날짜의 수면 기록이 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void SleepController::weeklyReport(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

    auto week_start = req->getParameter("weekStart");
    if (week_start.empty())
    {
        const auto& state = AppState::get();
        std::string ref_date;
        if (state.demo_mode && !state.anchor_date.empty())
        {
            ref_date = state.anchor_date;
        }
        else
        {
            auto rows = client->execSqlSync(
                "SELECT night_date FROM sleep_session WHERE user_id = ? ORDER BY night_date DESC LIMIT 1",
                *user_id);
            if (!rows.empty())
                ref_date = rows[0]["night_date"].as<std::string>();
        }

        if (!ref_date.empty())
        {
            auto start_rows = client->execSqlSync(
                "SELECT date(?, '-6 day') AS week_start",
                ref_date);
            if (!start_rows.empty())
                week_start = start_rows[0]["week_start"].as<std::string>();
        }
    }

    if (week_start.empty())
    {
        respondError(callback, 400, "INVALID_WEEK_START", "weekStart는 YYYY-MM-DD 형식이어야 합니다.", "weekStart");
        return;
    }

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getWeeklyReport(*user_id, week_start)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
