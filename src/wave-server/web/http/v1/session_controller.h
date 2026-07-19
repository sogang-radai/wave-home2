#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class SessionController :
    public drogon::HttpController<SessionController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SessionController::getSession, "/api/v1/session", drogon::Get);
    ADD_METHOD_TO(SessionController::patchActiveAccount, "/api/v1/session/active-account", drogon::Patch);
    METHOD_LIST_END

    void getSession(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void patchActiveAccount(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
