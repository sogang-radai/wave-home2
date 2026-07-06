#include "push_notification.h"

#include <json/json.h>
#include <memory>

#include "../../app/app_state.h"
#include "../push/push_store.h"
#include "../push/web_push.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

void PushNotificationController::send(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (state.test_mode || !state.db() || state.config.push.vapid_public_key.empty())
    {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k503ServiceUnavailable);
        resp->setBody("Push notifications are unavailable");
        callback(resp);
        return;
    }

    push::Message message;
    message.title = "WaveAI 건강 브리핑";
    message.body = "수면 중 심한 코골이가 감지되어 가습기를 가동했습니다.";
    message.url = "/chat";

    int64_t session_id = 1;
    if (const auto json = req->getJsonObject())
    {
        if (json->isMember("title")) message.title = (*json)["title"].asString();
        if (json->isMember("body")) message.body = (*json)["body"].asString();
        if (json->isMember("url")) message.url = (*json)["url"].asString();
        if (json->isMember("sessionId")) session_id = (*json)["sessionId"].asInt64();
    }

    push::VapidConfig vapid;
    vapid.public_key = state.config.push.vapid_public_key;
    vapid.private_key = state.config.push.vapid_private_key;
    vapid.subject = state.config.push.subject;

    const auto subscriptions = push::listSubscriptions(state.db(), session_id);
    if (subscriptions.empty())
    {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k404NotFound);
        resp->setBody("No push subscriptions registered");
        callback(resp);
        return;
    }

    struct SendState
    {
        size_t total = 0;
        size_t success = 0;
        size_t done = 0;
        std::function<void(const drogon::HttpResponsePtr&)> callback;

        void finishIfDone()
        {
            if (done < total)
                return;

            auto res = drogon::HttpResponse::newHttpResponse();
            if (success > 0)
            {
                res->setStatusCode(drogon::k200OK);
                res->setBody(
                    "푸시 알림 발송 성공 ("
                    + std::to_string(success)
                    + "/"
                    + std::to_string(total)
                    + ")");
            }
            else
            {
                res->setStatusCode(drogon::k502BadGateway);
                res->setBody("푸시 알림 발송 실패");
            }
            callback(res);
        }
    };

    auto send_state = std::make_shared<SendState>();
    send_state->total = subscriptions.size();
    send_state->callback = std::move(callback);

    for (const auto& subscription : subscriptions)
    {
        const auto prepared = push::prepareRequest(subscription, vapid, message);
        if (!prepared)
        {
            send_state->done += 1;
            send_state->finishIfDone();
            continue;
        }

        auto client = drogon::HttpClient::newHttpClient(prepared->endpoint);
        auto push_req = drogon::HttpRequest::newHttpRequest();
        push_req->setMethod(drogon::Post);
        push_req->setBody(std::string(
            reinterpret_cast<const char*>(prepared->body.data()),
            prepared->body.size()));
        push_req->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);

        for (const auto& [key, value] : prepared->headers)
            push_req->addHeader(key, value);

        client->sendRequest(push_req, [send_state](drogon::ReqResult result, const drogon::HttpResponsePtr& resp)
        {
            if (result == drogon::ReqResult::Ok && resp && (resp->statusCode() == 201 || resp->statusCode() == 200))
                send_state->success += 1;

            send_state->done += 1;
            send_state->finishIfDone();
        });
    }
}

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
