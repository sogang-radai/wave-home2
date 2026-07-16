#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

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

    void vapidPublicKey(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void saveSubscription(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void deleteSubscription(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
