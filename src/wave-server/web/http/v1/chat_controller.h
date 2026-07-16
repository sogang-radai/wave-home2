#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class ChatController :
    public drogon::HttpController<ChatController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ChatController::listConversations, "/api/v1/chat/conversations", drogon::Get);
    ADD_METHOD_TO(ChatController::createConversation, "/api/v1/chat/conversations", drogon::Post);
    ADD_METHOD_TO(ChatController::getConversation, "/api/v1/chat/conversations/{conversationId}", drogon::Get);
    ADD_METHOD_TO(ChatController::renameConversation, "/api/v1/chat/conversations/{conversationId}", drogon::Patch);
    ADD_METHOD_TO(ChatController::deleteConversation, "/api/v1/chat/conversations/{conversationId}", drogon::Delete);
    ADD_METHOD_TO(ChatController::appendMessage, "/api/v1/chat/conversations/{conversationId}/messages", drogon::Post);
    ADD_METHOD_TO(ChatController::streamMessage, "/api/v1/chat/conversations/{conversationId}/messages/stream", drogon::Post);
    ADD_METHOD_TO(ChatController::streamNewConversation, "/api/v1/chat/conversations/stream", drogon::Post);
    ADD_METHOD_TO(ChatController::getSuggestions, "/api/v1/chat/suggestions", drogon::Get);
    ADD_METHOD_TO(ChatController::askInsight, "/api/v1/chat/insight-queries", drogon::Post);
    METHOD_LIST_END

    void listConversations(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void createConversation(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void getConversation(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string conversationId);
    void renameConversation(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string conversationId);
    void deleteConversation(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string conversationId);

    void appendMessage(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string conversationId);
    void streamMessage(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string conversationId);
    void streamNewConversation(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void getSuggestions(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void askInsight(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
