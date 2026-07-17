#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <json/json.h>

#include "../core/coredefs.h"
#include "../db/database.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

class IChatSessionFacade
{
public:
    virtual ~IChatSessionFacade() = default;

    virtual Json::Value listSummaries(
        int64_t user_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual std::optional<Json::Value> getConversation(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual std::optional<Json::Value> createConversation(
        int64_t user_id,
        const std::string& title,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual std::optional<Json::Value> createWithUserMessage(
        int64_t user_id,
        const std::string& text,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) = 0;

    virtual std::optional<Json::Value> rename(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& title,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) = 0;

    virtual bool remove(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual std::optional<Json::Value> appendUserMessage(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& text,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) = 0;

    virtual bool appendAssistantMessage(
        int64_t user_id,
        int64_t conversation_id,
        int64_t message_id,
        const std::string& text,
        const Json::Value& tool_events,
        const std::string& reasoning,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual int64_t nextMessageId(const Json::Value& messages) = 0;

    virtual std::string resolvePersonalPrompt(
        int64_t user_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;
};

} // namespace facade
WAVE_NAMESPACE_END
