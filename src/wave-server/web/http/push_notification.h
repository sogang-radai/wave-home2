#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpClient.h>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

class PushNotificationController :
    public drogon::HttpController<PushNotificationController>
{
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(PushNotificationController::send, "/api/push/send", drogon::Post);
    METHOD_LIST_END

    void send(
        const drogon::HttpRequestPtr& req,
        std::function<void (const drogon::HttpResponsePtr&)>&& callback);
};

WEB_NAMESPACE_END
WAVE_NAMESPACE_END