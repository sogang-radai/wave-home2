#include "demo_policy.h"

#include <string_view>

#include <drogon/drogon.h>

#include "../app/app_state.h"
#include "http/v1/session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

namespace
{
    bool isReadMethod(drogon::HttpMethod method)
    {
        return method == drogon::Get || method == drogon::Head || method == drogon::Options;
    }

    bool isApiPath(std::string_view path)
    {
        return path.starts_with("/api/v1/") || path.starts_with("/internal/v1/");
    }
}

void registerDemoPolicy()
{
    drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr
    {
        if (!AppState::get().demo_mode)
            return nullptr;

        if (!isApiPath(req->path()))
            return nullptr;

        if (isReadMethod(req->method()))
            return nullptr;

        auto resp = drogon::HttpResponse::newHttpJsonResponse(v1::makeError(
            "DEMO_READ_ONLY",
            "시연 모드에서는 변경할 수 없습니다.",
            403,
            ""));
        resp->setStatusCode(drogon::k403Forbidden);
        return resp;
    });
}

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
