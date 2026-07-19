#include "listener_policy.h"

#include <string_view>

#include <drogon/drogon.h>

#include "http/v1/session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

namespace
{
    bool is_internal_path(std::string_view path)
    {
        return path == "/internal" || path.starts_with("/internal/");
    }

    drogon::HttpResponsePtr reject(const char* code, const char* message)
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(v1::makeError(code, message, 403, ""));
        resp->setStatusCode(drogon::k403Forbidden);
        return resp;
    }
}

void registerListenerIsolation(uint16_t client_api_port, uint16_t agent_api_port)
{
    if (agent_api_port == 0 || agent_api_port == client_api_port)
        return;

    drogon::app().registerSyncAdvice(
        [client_api_port, agent_api_port](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr
        {
            const uint16_t local_port = req->localAddr().toPort();
            const bool internal = is_internal_path(req->path());

            if (local_port == client_api_port && internal)
            {
                return reject(
                    "INTERNAL_LISTENER_REQUIRED",
                    "This path is only available on the agent-api listener");
            }

            if (local_port == agent_api_port && !internal)
            {
                return reject(
                    "CLIENT_LISTENER_REQUIRED",
                    "This path is only available on the client-api listener");
            }

            return nullptr;
        });
}

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
