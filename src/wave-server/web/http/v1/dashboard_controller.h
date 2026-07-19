#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class DashboardController :
    public drogon::HttpController<DashboardController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DashboardController::dailyMessage, "/api/v1/dashboard/daily-message", drogon::Get);
    ADD_METHOD_TO(DashboardController::currentState, "/api/v1/dashboard/current-state", drogon::Get);
    ADD_METHOD_TO(DashboardController::upcomingAlarms, "/api/v1/dashboard/alarms/upcoming", drogon::Get);
    ADD_METHOD_TO(DashboardController::activeGestureRules, "/api/v1/dashboard/gestures/active", drogon::Get);
    METHOD_LIST_END

    void dailyMessage(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void currentState(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void upcomingAlarms(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void activeGestureRules(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
