#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class PushController :
    public drogon::HttpController<PushController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PushController::vapidPublicKey, "/api/v1/push/vapid-public-key", drogon::Get);
    ADD_METHOD_TO(PushController::saveSubscription, "/api/v1/push/subscription", drogon::Put);
    ADD_METHOD_TO(PushController::deleteSubscription, "/api/v1/push/subscription", drogon::Delete);
    METHOD_LIST_END

    void vapidPublicKey(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void saveSubscription(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void deleteSubscription(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
