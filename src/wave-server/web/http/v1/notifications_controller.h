#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class NotificationsController :
    public drogon::HttpController<NotificationsController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(NotificationsController::listNotifications, "/api/v1/notifications", drogon::Get);
    ADD_METHOD_TO(NotificationsController::markAllRead, "/api/v1/notifications/read-all", drogon::Patch);
    ADD_METHOD_TO(NotificationsController::markRead, "/api/v1/notifications/{1}/read", drogon::Patch);
    METHOD_LIST_END

    void listNotifications(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void markAllRead(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void markRead(const HttpRequestPtr& req, HttpResponseCallback&& callback, const std::string& notification_id);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
