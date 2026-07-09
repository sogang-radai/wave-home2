#include "sleep_controller.h"

#include "../../../app/app_state.h"
#include "session_store.h"
#include "settings_store.h"
#include "sleep_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace
{
    std::optional<int64_t> resolveUserId(const drogon::HttpRequestPtr& req, drogon::orm::DbClientPtr client)
    {
        SessionStore sessions(client);
        SettingsStore settings(client);
        return settings.resolveActiveUserId(sessions, req);
    }
}

void SleepController::todaySummary(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getTodaySummary(*user_id)));
}

void SleepController::todayPlan(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getTodayPlan(*user_id)));
}

void SleepController::todayPhoneUsage(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
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

void SleepController::todayAutomationSummary(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getTodayAutomationSummary(*user_id)));
}

void SleepController::dailySessions(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
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

void SleepController::dailyReport(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
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

void SleepController::weeklyReport(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    auto week_start = req->getParameter("weekStart");
    if (week_start.empty())
    {
        auto rows = client->execSqlSync(
            "SELECT night_date FROM sleep_session WHERE user_id = ? ORDER BY night_date DESC LIMIT 1",
            *user_id);
        if (!rows.empty())
        {
            const std::string night = rows[0]["night_date"].as<std::string>();
            auto monday_rows = client->execSqlSync(
                "SELECT date(?, '-' || ((strftime('%w', ?) + 6) % 7) || ' day') AS week_start",
                night,
                night);
            if (!monday_rows.empty())
                week_start = monday_rows[0]["week_start"].as<std::string>();
        }
    }

    if (week_start.empty())
    {
        respondError(callback, 400, "INVALID_WEEK_START", "weekStart는 해당 주의 월요일 날짜여야 합니다.", "weekStart");
        return;
    }

    SleepStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getWeeklyReport(*user_id, week_start)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
