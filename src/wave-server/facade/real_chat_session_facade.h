#pragma once

#include "chat_session_facade.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

class RealChatSessionFacade :
    public IChatSessionFacade
{
public:
    Json::Value listSummaries(
        int64_t user_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    std::optional<Json::Value> getConversation(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    std::optional<Json::Value> createConversation(
        int64_t user_id,
        const std::string& title,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    std::optional<Json::Value> createWithUserMessage(
        int64_t user_id,
        const std::string& text,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) override;

    std::optional<Json::Value> rename(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& title,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) override;

    bool remove(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    std::optional<Json::Value> appendUserMessage(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& text,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) override;

    bool appendAssistantMessage(
        int64_t user_id,
        int64_t conversation_id,
        int64_t message_id,
        const std::string& text,
        const Json::Value& tool_events,
        const std::string& reasoning,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    int64_t nextMessageId(const Json::Value& messages) override;

    std::string resolvePersonalPrompt(
        int64_t user_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;
};

} // namespace facade
WAVE_NAMESPACE_END
