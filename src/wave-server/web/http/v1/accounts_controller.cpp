#include "accounts_controller.h"

#include <json/json.h>

#include "../../../app/app_state.h"
#include "accounts_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    Json::Value accountJson(const AccountView& account)
    {
        Json::Value body;
        body["id"] = static_cast<Json::Int64>(account.id);
        body["name"] = account.name;
        return body;
    }
}

void AccountsController::listAccounts(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SessionStore store(state.db());
    Json::Value body(Json::arrayValue);
    for (const auto& account : store.listAccounts())
        body.append(accountJson(account));
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void AccountsController::createAccount(
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
    if (!json || !json->isMember("name") || !(*json)["name"].isString())
    {
        respondError(callback, 400, "INVALID_NAME", "이름을 입력해주세요.", "name");
        return;
    }

    AccountsStore store(state.db());
    std::string error;
    std::string field;
    const auto account = store.createAccount((*json)["name"].asString(), error, field);
    if (!account)
    {
        respondError(callback, 400, "INVALID_NAME", error, field);
        return;
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(accountJson(*account));
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void AccountsController::updateAccount(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t accountId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("name") || !(*json)["name"].isString())
    {
        respondError(callback, 400, "INVALID_NAME", "이름을 입력해주세요.", "name");
        return;
    }

    AccountsStore store(state.db());
    std::string error;
    std::string field;
    const auto account = store.updateAccount(accountId, (*json)["name"].asString(), error, field);
    if (!account)
    {
        const auto status = field.empty() ? 404 : 400;
        const auto code = field.empty() ? "NOT_FOUND" : "INVALID_NAME";
        respondError(callback, status, code, error, field);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(accountJson(*account)));
}

void AccountsController::deleteAccount(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t accountId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SessionStore sessions(state.db());
    AccountsStore store(state.db());
    std::string error;
    std::string code;
    if (!store.deleteAccount(accountId, sessions.resolveSession(req).session_id, error, code))
    {
        const auto status = code == "NOT_FOUND" ? 404 : 409;
        respondError(callback, status, code, error);
        return;
    }

    Json::Value body;
    body["id"] = static_cast<Json::Int64>(accountId);
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
