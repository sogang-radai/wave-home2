#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../../core/json.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct AgentChatMessage
{
    std::string role;
    std::string content;
};

struct AgentChatTurnRequest
{
    int64_t chat_history_id = 0;
    int64_t user_id = 0;
    std::vector<AgentChatMessage> messages;
    std::string now;
    std::string demo_runtime_id;
    bool stream = true;
};

enum class AgentClientResult
{
    success,
    network_error,
    http_error,
    parse_error,
    cancelled,
};

using AgentStreamEventCallback = std::function<bool(const json& event)>;

AgentClientResult streamChatTurn(
    const std::string& base_url,
    const AgentChatTurnRequest& request,
    const AgentStreamEventCallback& on_event,
    std::string& out_error);

/** Runs a chat turn via the streaming agent API so tool.start/tool.end are available.
 *  out_tool_events (optional) receives UI-shaped tool event objects (array). */
AgentClientResult runChatTurnSync(
    const std::string& base_url,
    const AgentChatTurnRequest& request,
    std::string& out_content,
    std::string& out_model,
    std::string& out_error,
    json* out_tool_events = nullptr,
    std::string* out_reasoning = nullptr);

struct AgentSleepJobResult
{
    std::string text;
    std::vector<float> embedding;
    std::string model;
    std::string embeddingModel;
};

AgentClientResult runSleepJobSync(
    const std::string& base_url,
    const std::string& post_path,
    const json& body,
    AgentSleepJobResult& out_result,
    std::string& out_error);

/** POST /power/v1/reports then poll /power/v1/jobs/{id} until done (same result shape as sleep). */
AgentClientResult runPowerJobSync(
    const std::string& base_url,
    const json& body,
    AgentSleepJobResult& out_result,
    std::string& out_error);

struct AgentInsightJobResult
{
    json items = json::array();
};

/** POST /insight/v1/insights then poll /insight/v1/jobs/{id} until done. */
AgentClientResult runInsightJobSync(
    const std::string& base_url,
    const json& body,
    AgentInsightJobResult& out_result,
    std::string& out_error);

struct AgentGoalCoachingJobResult
{
    /** raw {periodStart, pastSummary, projection, projectedMetrics, items} — richer than the
     * plain text/embedding shape sleep/power reports use, so we keep it as a json blob rather
     * than a struct with named fields. */
    json content = json::object();
};

/** POST /goal-coaching/v1/reports then poll /goal-coaching/v1/jobs/{id} until done. */
AgentClientResult runGoalCoachingJobSync(
    const std::string& base_url,
    const json& body,
    AgentGoalCoachingJobResult& out_result,
    std::string& out_error);

struct AgentHabitJobResult
{
    json items = json::array();
};

/** POST /insight/v1/habits then poll /insight/v1/jobs/{id} until done (same
 *  job-store/poll endpoint as insight generation — habit discovery reuses
 *  it rather than standing up a separate job type on the agent side). Called
 *  directly from UserModelManager's nightly rollover, not via AgentJobQueue —
 *  a once-a-day batch that's already uniquely gated by its own day-rollover
 *  guard has no dedup/priority need. */
AgentClientResult runHabitJobSync(
    const std::string& base_url,
    const json& body,
    AgentHabitJobResult& out_result,
    std::string& out_error);

struct AgentBannerJobResult
{
    std::string headline;
    std::string body;
};

/** POST /insight/v1/habit-banner then poll /insight/v1/jobs/{id} until done.
 *  Same direct-call-from-a-background-rollover shape as runHabitJobSync — no
 *  AgentJobQueue involved. */
AgentClientResult runBannerJobSync(
    const std::string& base_url,
    const json& body,
    AgentBannerJobResult& out_result,
    std::string& out_error);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
