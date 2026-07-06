#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

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
    METHOD_LIST_END

    void listNotifications(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void markAllRead(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
