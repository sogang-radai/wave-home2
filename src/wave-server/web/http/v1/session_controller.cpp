#include "session_controller.h"

#include "../../../app/app_state.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

void SessionController::getSession(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SessionStore store(state.db());
    const auto session = store.resolveSession(req);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.sessionJson(session)));
}

void SessionController::patchActiveAccount(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("accountId"))
    {
        respondError(callback, 400, "INVALID_BODY", "accountId가 필요합니다.");
        return;
    }

    const auto account_id = (*json)["accountId"].asInt64();
    SessionStore store(state.db());
    const auto session = store.resolveSession(req);

    std::string error;
    if (!store.setActiveAccount(session.session_id, account_id, error))
    {
        respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    const auto updated = store.resolveSession(req);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.sessionJson(updated)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
