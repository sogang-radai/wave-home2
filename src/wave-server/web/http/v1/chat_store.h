#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "../../../service/agent_client.h"
#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class ChatStore
{
public:
    explicit ChatStore(drogon::orm::DbClientPtr client);

    static std::string toCreatedAtIso(const std::string& db_time);
    static std::string titleFromText(const std::string& text);

    Json::Value listSummaries(int64_t user_id) const;
    std::optional<Json::Value> getConversation(int64_t user_id, int64_t conversation_id) const;
    std::optional<Json::Value> createConversation(int64_t user_id, const std::string& title) const;
    std::optional<Json::Value> renameConversation(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& title,
        std::string& error,
        std::string& field) const;
    bool deleteConversation(int64_t user_id, int64_t conversation_id) const;

    std::optional<Json::Value> appendUserMessage(
        int64_t user_id,
        int64_t conversation_id,
        const std::string& text,
        std::string& error,
        std::string& field) const;

    std::optional<Json::Value> createConversationWithUserMessage(
        const std::string& text,
        int64_t user_id,
        std::string& error,
        std::string& field) const;

    bool appendAssistantMessage(
        int64_t user_id,
        int64_t conversation_id,
        int64_t message_id,
        const std::string& text,
        const Json::Value& tool_events = Json::Value(Json::arrayValue),
        const std::string& reasoning = {}) const;

    std::vector<service::AgentChatMessage> buildAgentMessages(const Json::Value& messages) const;

    Json::Value defaultSuggestions() const;

    int64_t nextMessageId(const Json::Value& messages) const;
    Json::Value makeAssistantShell(int64_t message_id, const std::string& created_at) const;

    /** Accept ChatStore array or mock seed {"messages":[{role,content},...]} and
     *  normalize to [{id,role,text,createdAt},...] (system rows dropped). */
    static Json::Value normalizeMessagesJson(
        Json::Value raw,
        const std::string& fallback_db_time = {});

private:
    drogon::orm::DbClientPtr m_client;

    int64_t nextConversationId() const;
    Json::Value parseMessagesColumn(const std::string& raw) const;
    std::string serializeMessages(const Json::Value& messages) const;
    Json::Value makeUserMessage(int64_t message_id, const std::string& text, const std::string& created_at) const;
    Json::Value toSummary(
        int64_t id,
        const std::string& title,
        const Json::Value& messages,
        const std::string& created_at,
        const std::string& updated_at) const;
    std::optional<Json::Value> loadConversationRow(int64_t user_id, int64_t conversation_id) const;
    bool saveMessages(
        int64_t conversation_id,
        const Json::Value& messages,
        const std::string& updated_at) const;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
