#pragma once

#include <optional>
#include <string>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

void ensureDemoSessionSeeded(const std::string& runtime_id, const drogon::orm::DbClientPtr& client);

Json::Value demoListAlarms(
    const std::string& runtime_id,
    int64_t user_id,
    const drogon::orm::DbClientPtr& client,
    const std::optional<bool>& enabled_filter = std::nullopt);

Json::Value demoCreateAlarm(
    const std::string& runtime_id,
    const Json::Value& body,
    const drogon::orm::DbClientPtr& client,
    std::string& error,
    std::string& field);

Json::Value demoUpdateAlarm(
    const std::string& runtime_id,
    int64_t alarm_id,
    const Json::Value& body,
    std::string& error,
    std::string& field);

bool demoDeleteAlarm(const std::string& runtime_id, int64_t alarm_id);

Json::Value demoListScheduleTasks(
    const std::string& runtime_id,
    int64_t user_id,
    const drogon::orm::DbClientPtr& client = nullptr);

Json::Value demoCreateScheduleTask(
    const std::string& runtime_id,
    const Json::Value& body,
    std::string& error,
    std::string& field);

Json::Value demoUpdateScheduleTask(
    const std::string& runtime_id,
    int64_t task_id,
    const Json::Value& body,
    std::string& error,
    std::string& field);

bool demoDeleteScheduleTask(const std::string& runtime_id, int64_t task_id);

Json::Value demoCreateRule(const std::string& runtime_id, const Json::Value& body, std::string& code);
Json::Value demoListRules(const std::string& runtime_id, int64_t user_id);
Json::Value demoUpdateRule(
    const std::string& runtime_id,
    const std::string& rule_id,
    const Json::Value& body,
    std::string& code);
bool demoDeleteRule(const std::string& runtime_id, const std::string& rule_id);

Json::Value demoListChatSummaries(
    const std::string& runtime_id,
    int64_t user_id,
    const drogon::orm::DbClientPtr& client);
std::optional<Json::Value> demoGetChatConversation(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t conversation_id,
    const drogon::orm::DbClientPtr& client);
std::optional<Json::Value> demoCreateChatConversation(
    const std::string& runtime_id,
    int64_t user_id,
    const std::string& title,
    const drogon::orm::DbClientPtr& client);
std::optional<Json::Value> demoRenameChatConversation(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t conversation_id,
    const std::string& title,
    std::string& error,
    const drogon::orm::DbClientPtr& client);
bool demoDeleteChatConversation(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t conversation_id,
    const drogon::orm::DbClientPtr& client);
std::optional<Json::Value> demoAppendChatUserMessage(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t conversation_id,
    const std::string& text,
    std::string& error,
    const drogon::orm::DbClientPtr& client);
std::optional<Json::Value> demoCreateChatWithUserMessage(
    const std::string& runtime_id,
    int64_t user_id,
    const std::string& text,
    std::string& error,
    const drogon::orm::DbClientPtr& client);
bool demoAppendChatAssistantMessage(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t conversation_id,
    int64_t message_id,
    const std::string& text,
    const Json::Value& tool_events = Json::Value(Json::arrayValue),
    const std::string& reasoning = {});
int64_t demoNextChatMessageId(const Json::Value& messages);

WAVE_NAMESPACE_END
