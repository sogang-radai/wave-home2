#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class PowerController :
    public drogon::HttpController<PowerController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PowerController::listPlugs, "/api/v1/power/plugs", drogon::Get);
    ADD_METHOD_TO(PowerController::comboTrend, "/api/v1/power/trend/combo", drogon::Get);
    ADD_METHOD_TO(PowerController::periodTrend, "/api/v1/power/trend/period", drogon::Get);
    ADD_METHOD_TO(PowerController::powerReport, "/api/v1/power/reports", drogon::Get);
    METHOD_LIST_END

    void listPlugs(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void comboTrend(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void periodTrend(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void powerReport(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
