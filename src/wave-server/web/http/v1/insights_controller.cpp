#include "insights_controller.h"

#include "../../../app/app_state.h"
#include "insights_store.h"
#include "session_store.h"
#include "settings_store.h"

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

    std::optional<bool> parseOptionalBool(const std::string& value)
    {
        if (value.empty())
            return std::nullopt;
        if (value == "true" || value == "1")
            return true;
        if (value == "false" || value == "0")
            return false;
        return std::nullopt;
    }
}

void InsightsController::listInsights(
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

    const auto surface = req->getParameter("surface");
    const auto date = req->getParameter("date");
    const auto kind = req->getParameter("kind");
    const auto approved = parseOptionalBool(req->getParameter("approved"));
    const auto actionable = parseOptionalBool(req->getParameter("actionable"));

    InsightsStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.list(
        *user_id,
        surface.empty() ? std::nullopt : std::optional<std::string>(surface),
        date.empty() ? std::nullopt : std::optional<std::string>(date),
        kind.empty() ? std::nullopt : std::optional<std::string>(kind),
        approved,
        actionable)));
}

void InsightsController::getInsight(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t insightId)
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

    InsightsStore store(client);
    const auto body = store.getById(*user_id, insightId);
    if (body.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "인사이트를 찾을 수 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
