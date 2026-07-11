#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../core/json.h"

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

AgentClientResult runChatTurnSync(
    const std::string& base_url,
    const AgentChatTurnRequest& request,
    std::string& out_content,
    std::string& out_model,
    std::string& out_error);

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

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
