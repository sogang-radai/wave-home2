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

Json::Value demoListNotifications(
    const std::string& runtime_id,
    int64_t user_id,
    const drogon::orm::DbClientPtr& client,
    int limit = 0,
    int64_t before_id = 0);

Json::Value demoMarkAllNotificationsRead(
    const std::string& runtime_id,
    int64_t user_id,
    const drogon::orm::DbClientPtr& client);

Json::Value demoMarkNotificationRead(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t notification_id,
    const drogon::orm::DbClientPtr& client);

Json::Value demoAppendNotification(
    const std::string& runtime_id,
    int64_t user_id,
    const std::string& type,
    const std::string& message);

Json::Value demoListSpeechOverlays(const std::string& runtime_id);

void demoSetSpeechOverlay(
    const std::string& runtime_id,
    const std::string& device_id,
    const Json::Value& overlay);

void demoClearSpeechOverlay(const std::string& runtime_id, const std::string& device_id);

void demoRefreshSpeechOverlays(const std::string& runtime_id, int64_t now_ms);

void demoFireAlarm(
    const std::string& runtime_id,
    const Json::Value& alarm,
    const drogon::orm::DbClientPtr& client);

void demoDisableAlarm(const std::string& runtime_id, int64_t alarm_id);

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
/** cron/relativeMinutes 등 레거시 schedule 을 agent RuleSchedule 형식으로 정규화. 실패 시 null. */
Json::Value demoNormalizeRuleSchedule(const Json::Value& schedule);
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

Json::Value demoGetAiAgentSettings(
    const std::string& runtime_id,
    int64_t user_id,
    const drogon::orm::DbClientPtr& client);

Json::Value demoPutAiAgentSettings(
    const std::string& runtime_id,
    int64_t user_id,
    const Json::Value& body,
    const drogon::orm::DbClientPtr& client,
    std::string& error,
    std::string& field);

std::string demoResolvePersonalPrompt(
    const std::string& runtime_id,
    int64_t user_id,
    const drogon::orm::DbClientPtr& client);

WAVE_NAMESPACE_END
