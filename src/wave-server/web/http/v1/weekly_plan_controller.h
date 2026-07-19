#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class WeeklyPlanController :
    public drogon::HttpController<WeeklyPlanController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(WeeklyPlanController::weeklyReport, "/api/v1/weekly-plan/report", drogon::Get);
    ADD_METHOD_TO(WeeklyPlanController::recommendations, "/api/v1/weekly-plan/recommendations", drogon::Get);
    METHOD_LIST_END

    void weeklyReport(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void recommendations(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
