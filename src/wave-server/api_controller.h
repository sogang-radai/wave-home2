#pragma once

#include "whdefs.h"

#include <drogon/HttpController.h>

WH_NAMESPACE_BEGIN

class ApiController : public drogon::HttpController<ApiController, false>
{
public:
    METHOD_LIST_BEGIN
    METHOD_ADD(ApiController::status, "/api/status", drogon::Get);
    METHOD_ADD(ApiController::apps, "/api/apps", drogon::Get);
    METHOD_LIST_END

    void status(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void apps(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

WH_NAMESPACE_END
