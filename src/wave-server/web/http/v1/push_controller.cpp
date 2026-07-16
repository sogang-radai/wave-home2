#include "push_controller.h"

#include <json/json.h>

#include "../../../app/app_state.h"
#include "../../push/push_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

void PushController::vapidPublicKey(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback)
{
    auto& state = AppState::get();
    if (state.test_mode || state.config.push.vapid_public_key.empty())
    {
        respondError(callback, 503, "PUSH_UNAVAILABLE", "푸시 알림을 사용할 수 없습니다.");
        return;
    }

    Json::Value body;
    body["publicKey"] = state.config.push.vapid_public_key;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void PushController::saveSubscription(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    auto& state = AppState::get();
    if (state.test_mode || !state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("endpoint") || !json->isMember("keys"))
    {
        respondError(callback, 400, "INVALID_BODY", "endpoint와 keys가 필요합니다.");
        return;
    }

    const auto& keys = (*json)["keys"];
    if (!keys.isMember("p256dh") || !keys.isMember("auth"))
    {
        respondError(callback, 400, "INVALID_BODY", "keys.p256dh와 keys.auth가 필요합니다.");
        return;
    }

    SessionStore store(state.db());
    const auto session = store.resolveSession(req);

    push::Subscription subscription;
    subscription.endpoint = (*json)["endpoint"].asString();
    subscription.p256dh = keys["p256dh"].asString();
    subscription.auth = keys["auth"].asString();

    if (!push::upsertSubscription(state.db(), session.session_id, subscription))
    {
        respondError(callback, 500, "SAVE_FAILED", "푸시 구독 저장에 실패했습니다.");
        return;
    }

    Json::Value body;
    body["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void PushController::deleteSubscription(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    auto& state = AppState::get();
    if (state.test_mode || !state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SessionStore store(state.db());
    const auto session = store.resolveSession(req);

    if (!push::deleteSubscriptions(state.db(), session.session_id))
    {
        respondError(callback, 500, "DELETE_FAILED", "푸시 구독 삭제에 실패했습니다.");
        return;
    }

    Json::Value body;
    body["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
