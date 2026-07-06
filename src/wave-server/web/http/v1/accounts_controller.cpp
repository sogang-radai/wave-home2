#include "accounts_controller.h"

#include <json/json.h>

#include "../../../app/app_state.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

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
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(account.id);
        item["name"] = account.name;
        body.append(item);
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
