#include "demo_chat_session_facade.h"

#include "demo_session_writes.h"

WAVE_NAMESPACE_BEGIN

Json::Value DemoChatSessionFacade::listSummaries(
    int64_t user_id,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return demoListChatSummaries(runtime_id, user_id, client);
}

std::optional<Json::Value> DemoChatSessionFacade::getConversation(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return demoGetChatConversation(runtime_id, user_id, conversation_id, client);
}

std::optional<Json::Value> DemoChatSessionFacade::createConversation(
    int64_t user_id,
    const std::string& title,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return demoCreateChatConversation(runtime_id, user_id, title, client);
}

std::optional<Json::Value> DemoChatSessionFacade::createWithUserMessage(
    int64_t user_id,
    const std::string& text,
    const std::string& runtime_id,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& /*field*/)
{
    return demoCreateChatWithUserMessage(runtime_id, user_id, text, error, client);
}

std::optional<Json::Value> DemoChatSessionFacade::rename(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& title,
    const std::string& runtime_id,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& /*field*/)
{
    return demoRenameChatConversation(runtime_id, user_id, conversation_id, title, error, client);
}

bool DemoChatSessionFacade::remove(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return demoDeleteChatConversation(runtime_id, user_id, conversation_id, client);
}

std::optional<Json::Value> DemoChatSessionFacade::appendUserMessage(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& text,
    const std::string& runtime_id,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& /*field*/)
{
    return demoAppendChatUserMessage(runtime_id, user_id, conversation_id, text, error, client);
}

bool DemoChatSessionFacade::appendAssistantMessage(
    int64_t user_id,
    int64_t conversation_id,
    int64_t message_id,
    const std::string& text,
    const Json::Value& tool_events,
    const std::string& reasoning,
    const std::string& runtime_id,
    const db::DbClientPtr& /*client*/)
{
    return demoAppendChatAssistantMessage(
        runtime_id, user_id, conversation_id, message_id, text, tool_events, reasoning);
}

int64_t DemoChatSessionFacade::nextMessageId(const Json::Value& messages)
{
    return demoNextChatMessageId(messages);
}

std::string DemoChatSessionFacade::resolvePersonalPrompt(
    int64_t user_id,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return demoResolvePersonalPrompt(runtime_id, user_id, client);
}

WAVE_NAMESPACE_END
