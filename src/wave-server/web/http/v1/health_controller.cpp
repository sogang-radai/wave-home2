#include "health_controller.h"

#include <json/json.h>

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

void HealthController::health(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback)
{
    Json::Value body;
    body["status"] = "ok";
    body["api"] = "v1";
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
