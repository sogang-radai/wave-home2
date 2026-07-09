#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

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
    ADD_METHOD_TO(
        ChatController::streamMessage,
        "/api/v1/chat/conversations/{conversationId}/messages/stream",
        drogon::Post);
    ADD_METHOD_TO(ChatController::streamNewConversation, "/api/v1/chat/conversations/stream", drogon::Post);
    ADD_METHOD_TO(ChatController::streamEphemeral, "/api/v1/chat/ephemeral/stream", drogon::Post);
    ADD_METHOD_TO(ChatController::getSuggestions, "/api/v1/chat/suggestions", drogon::Get);
    ADD_METHOD_TO(ChatController::askInsight, "/api/v1/chat/insight-queries", drogon::Post);
    METHOD_LIST_END

    void listConversations(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void createConversation(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getConversation(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string conversationId);

    void renameConversation(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string conversationId);

    void deleteConversation(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string conversationId);

    void appendMessage(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string conversationId);

    void streamMessage(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string conversationId);

    void streamNewConversation(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void streamEphemeral(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getSuggestions(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void askInsight(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
