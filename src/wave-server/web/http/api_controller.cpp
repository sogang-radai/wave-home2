#include "api_controller.h"
#include "core/coredefs.h"

#include <json/json.h>

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

void ApiController::status(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    Json::Value body;
    body["status"] = "ok";
    body["message"] = "Wave Home server is running";
    body["version"] = "0.1.0";

    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    callback(resp);
}

void ApiController::apps(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    Json::Value body;
    Json::Value apps(Json::arrayValue);

    {
        Json::Value app;
        app["id"] = "bed-net";
        app["name"] = "Bed Net";
        app["description"] = "침대 센서 기반 수면·움직임 분석";
        apps.append(app);
    }
    {
        Json::Value app;
        app["id"] = "ir-remote";
        app["name"] = "IR Remote";
        app["description"] = "적외선 리모컨 제어";
        apps.append(app);
    }
    {
        Json::Value app;
        app["id"] = "llm-agent";
        app["name"] = "LLM Agent";
        app["description"] = "로컬 LLM 기반 라이프스타일 에이전트";
        apps.append(app);
    }

    body["apps"] = apps;

    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    callback(resp);
}

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
