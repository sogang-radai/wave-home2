#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class ChatSttController :
    public drogon::HttpController<ChatSttController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ChatSttController::createSession, "/api/v1/chat/stt/sessions", drogon::Post);
    ADD_METHOD_TO(ChatSttController::streamEvents, "/api/v1/chat/stt/sessions/{sessionId}/events", drogon::Get);
    ADD_METHOD_TO(ChatSttController::pushAudio, "/api/v1/chat/stt/sessions/{sessionId}/audio", drogon::Post);
    ADD_METHOD_TO(ChatSttController::endSession, "/api/v1/chat/stt/sessions/{sessionId}/end", drogon::Post);
    ADD_METHOD_TO(ChatSttController::abortSession, "/api/v1/chat/stt/sessions/{sessionId}", drogon::Delete);
    METHOD_LIST_END

    void createSession(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void streamEvents(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string sessionId);
    void pushAudio(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string sessionId);
    void endSession(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string sessionId);
    void abortSession(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string sessionId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
