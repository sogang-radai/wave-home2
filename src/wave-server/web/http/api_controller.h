#pragma once

#include "../../core/coredefs.h"

#include <drogon/HttpController.h>

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

class ApiController :
    public drogon::HttpController<ApiController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ApiController::status, "/api/status", drogon::Get);
    ADD_METHOD_TO(ApiController::apps, "/api/apps", drogon::Get);
    METHOD_LIST_END

    void status(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void apps(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
