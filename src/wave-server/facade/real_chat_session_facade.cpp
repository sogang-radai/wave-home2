#include "real_chat_session_facade.h"

#include "../web/http/v1/chat_store.h"
#include "../web/http/v1/settings_store.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

Json::Value RealChatSessionFacade::listSummaries(
    int64_t user_id,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::v1::ChatStore(client).listSummaries(user_id);
}

std::optional<Json::Value> RealChatSessionFacade::getConversation(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::v1::ChatStore(client).getConversation(user_id, conversation_id);
}

std::optional<Json::Value> RealChatSessionFacade::createConversation(
    int64_t user_id,
    const std::string& title,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::v1::ChatStore(client).createConversation(user_id, title);
}

std::optional<Json::Value> RealChatSessionFacade::createWithUserMessage(
    int64_t user_id,
    const std::string& text,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    return web::v1::ChatStore(client).createConversationWithUserMessage(text, user_id, error, field);
}

std::optional<Json::Value> RealChatSessionFacade::rename(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& title,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    return web::v1::ChatStore(client).renameConversation(user_id, conversation_id, title, error, field);
}

bool RealChatSessionFacade::remove(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::v1::ChatStore(client).deleteConversation(user_id, conversation_id);
}

std::optional<Json::Value> RealChatSessionFacade::appendUserMessage(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& text,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    return web::v1::ChatStore(client).appendUserMessage(user_id, conversation_id, text, error, field);
}

bool RealChatSessionFacade::appendAssistantMessage(
    int64_t user_id,
    int64_t conversation_id,
    int64_t message_id,
    const std::string& text,
    const Json::Value& tool_events,
    const std::string& reasoning,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::v1::ChatStore(client).appendAssistantMessage(
        user_id, conversation_id, message_id, text, tool_events, reasoning);
}

int64_t RealChatSessionFacade::nextMessageId(const Json::Value& messages)
{
    return web::v1::ChatStore(nullptr).nextMessageId(messages);
}

std::string RealChatSessionFacade::resolvePersonalPrompt(
    int64_t user_id,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    const auto agent = web::v1::SettingsStore(client).getAiAgentSettings(user_id);
    if (!agent.isMember("personalPrompt") || !agent["personalPrompt"].isString())
        return {};
    return agent["personalPrompt"].asString();
}

} // namespace facade
WAVE_NAMESPACE_END
