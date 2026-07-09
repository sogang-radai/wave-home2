#include "chat_controller.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <json/json.h>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../core/logger.h"
#include "../../../core/time_util.h"
#include "../../../service/agent_client.h"
#include "chat_store.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    std::optional<int64_t> parseConversationId(const std::string& raw)
    {
        try
        {
            return std::stoll(raw);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    std::optional<int64_t> resolveUserId(
        const drogon::HttpRequestPtr& req,
        const std::function<void(const drogon::HttpResponsePtr&)>& callback)
    {
        auto& state = AppState::get();
        if (!state.db())
        {
            respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
            return std::nullopt;
        }

        SessionStore sessions(state.db());
        SettingsStore settings(state.db());
        const auto user_id = settings.resolveActiveUserId(sessions, req);
        if (!user_id)
        {
            respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
            return std::nullopt;
        }
        return user_id;
    }

    std::string extractText(const std::shared_ptr<const Json::Value>& json, std::string& error, std::string& field)
    {
        if (!json || !json->isMember("text") || !(*json)["text"].isString())
        {
            error = "메시지를 입력해주세요.";
            field = "text";
            return {};
        }

        std::string text = (*json)["text"].asString();
        const auto start = text.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
        {
            error = "메시지를 입력해주세요.";
            field = "text";
            return {};
        }
        const auto end = text.find_last_not_of(" \t\n\r");
        text = text.substr(start, end - start + 1);

        if (text.size() > 2000)
        {
            error = "메시지는 2000자 이하로 입력해주세요.";
            field = "text";
            return {};
        }
        return text;
    }

    bool sendSseEvent(const std::shared_ptr<drogon::ResponseStream>& stream, const Json::Value& event)
    {
        if (!stream)
            return false;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        const std::string payload = Json::writeString(builder, event);
        return stream->send("data: " + payload + "\n\n");
    }

    std::string toolLabel(const std::string& name, bool running, bool failed = false)
    {
        static const std::unordered_map<std::string, std::string> labels = {
            {"query_db", "DB 조회"},
            {"rag_search", "메모리 검색"},
            {"list_devices", "기기 목록 조회"},
            {"control_device", "기기 제어"},
            {"get_schedule_tasks", "일정 조회"},
            {"update_schedule_task", "일정 수정"},
        };

        const auto it = labels.find(name);
        const std::string base = it != labels.end() ? it->second : name;
        if (running)
            return base + " 중";
        if (failed)
            return base + " 실패";
        return base + " 완료";
    }

    std::string summarizeToolResult(const json& result)
    {
        if (result.is_null())
            return {};
        if (result.is_string())
            return result.get<std::string>();
        if (result.is_number_integer())
            return std::to_string(result.get<int>());
        if (result.is_number())
            return std::to_string(result.get<double>());
        if (result.is_object())
        {
            if (result.contains("count") && result["count"].is_number())
                return std::to_string(result["count"].get<int>()) + "건";
            if (result.contains("raw") && result["raw"].is_string())
                return result["raw"].get<std::string>();
        }
        return {};
    }

    Json::Value makeToolEvent(
        const std::string& name,
        bool running,
        const std::string& result_summary = {},
        bool failed = false)
    {
        Json::Value event;
        event["name"] = name;
        event["status"] = running ? "running" : (failed ? "failed" : "done");
        event["label"] = toolLabel(name, running, failed);
        if (!result_summary.empty())
            event["resultSummary"] = result_summary;
        return event;
    }

    std::string buildAgentNow()
    {
        const auto& state = AppState::get();
        if (state.demo_mode && !state.anchor_date.empty())
        {
            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm local_tm {};
#if defined(_WIN32)
            localtime_s(&local_tm, &t);
#else
            localtime_r(&t, &local_tm);
#endif
            char time_buf[16];
            std::snprintf(
                time_buf,
                sizeof(time_buf),
                "%02d:%02d:%02d",
                local_tm.tm_hour,
                local_tm.tm_min,
                local_tm.tm_sec);
            return state.anchor_date + " " + time_buf;
        }
        return formatTimestamp();
    }

    void setAgentTurnNow(service::AgentChatTurnRequest& request)
    {
        request.now = buildAgentNow();
    }

    service::AgentClientResult callAgentSync(
        int64_t chat_history_id,
        int64_t user_id,
        const std::vector<service::AgentChatMessage>& messages,
        std::string& out_content,
        std::string& out_model,
        std::string& out_error)
    {
        service::AgentChatTurnRequest request;
        request.chat_history_id = chat_history_id;
        request.user_id = user_id;
        request.messages = messages;
        setAgentTurnNow(request);
        request.stream = false;

        return service::runChatTurnSync(
            AppState::get().config.agent.base_url,
            request,
            out_content,
            out_model,
            out_error);
    }

    std::vector<service::AgentChatMessage> buildAgentMessagesFromJson(const Json::Value& messages)
    {
        std::vector<service::AgentChatMessage> out;
        if (!messages.isArray())
            return out;

        for (const auto& message : messages)
        {
            if (!message.isMember("role") || !message["role"].isString())
                continue;
            if (!message.isMember("text") || !message["text"].isString())
                continue;

            const std::string role = message["role"].asString();
            if (role != "user" && role != "assistant")
                continue;

            const std::string text = message["text"].asString();
            if (text.empty())
                continue;

            out.push_back({role, text});
        }
        return out;
    }

    std::optional<int64_t> parseRequiredInt64(
        const Json::Value& json,
        const char* field,
        std::string& error)
    {
        if (!json.isMember(field) || !json[field].isInt64())
        {
            error = std::string("필수 필드가 없습니다: ") + field;
            return std::nullopt;
        }
        return json[field].asInt64();
    }

    bool runAgentStreamCore(
        const std::shared_ptr<drogon::ResponseStream>& stream,
        int64_t user_id,
        int64_t conversation_id,
        int64_t assistant_id,
        const std::vector<service::AgentChatMessage>& agent_messages,
        std::string& accumulated_text,
        std::string& accumulated_reasoning,
        Json::Value& tool_events)
    {
        service::AgentChatTurnRequest agent_request;
        agent_request.chat_history_id = conversation_id;
        agent_request.user_id = user_id;
        agent_request.messages = agent_messages;
        setAgentTurnNow(agent_request);
        agent_request.stream = true;

        std::unordered_map<std::string, Json::ArrayIndex> tool_index;

        std::string agent_error;
        const auto agent_result = service::streamChatTurn(
            AppState::get().config.agent.base_url,
            agent_request,
            [&](const json& event) -> bool {
                if (!event.contains("type") || !event["type"].is_string())
                    return true;

                const std::string type = event["type"].get<std::string>();

                if (type == "tool.start")
                {
                    const std::string name = event.value("name", "");
                    Json::Value payload;
                    payload["type"] = "tool_start";
                    payload["conversationId"] = static_cast<Json::Int64>(conversation_id);
                    payload["messageId"] = static_cast<Json::Int64>(assistant_id);
                    payload["toolEvent"] = makeToolEvent(name, true);
                    if (!sendSseEvent(stream, payload))
                        return false;

                    Json::Value stored = makeToolEvent(name, true);
                    if (tool_index.count(name) > 0)
                        tool_events[tool_index[name]] = stored;
                    else
                    {
                        tool_index[name] = tool_events.size();
                        tool_events.append(stored);
                    }
                    return true;
                }

                if (type == "tool.end")
                {
                    const std::string name = event.value("name", "");
                    const bool failed = event.contains("ok") && event["ok"].is_boolean() && !event["ok"].get<bool>();
                    const std::string result_summary = event.contains("result")
                        ? summarizeToolResult(event["result"])
                        : std::string();
                    Json::Value payload;
                    payload["type"] = "tool_end";
                    payload["conversationId"] = static_cast<Json::Int64>(conversation_id);
                    payload["messageId"] = static_cast<Json::Int64>(assistant_id);
                    payload["toolEvent"] = makeToolEvent(name, false, result_summary, failed);
                    if (!sendSseEvent(stream, payload))
                        return false;

                    Json::Value stored = makeToolEvent(name, false, result_summary, failed);
                    if (tool_index.count(name) > 0)
                        tool_events[tool_index[name]] = stored;
                    else
                    {
                        tool_index[name] = tool_events.size();
                        tool_events.append(stored);
                    }
                    return true;
                }

                if (type == "status" && event.contains("phase") && event["phase"].is_string())
                {
                    Json::Value payload;
                    payload["type"] = "assistant_status";
                    payload["conversationId"] = static_cast<Json::Int64>(conversation_id);
                    payload["messageId"] = static_cast<Json::Int64>(assistant_id);
                    payload["phase"] = event["phase"].get<std::string>();
                    if (!sendSseEvent(stream, payload))
                        return false;
                    return true;
                }

                if (type == "message.delta")
                {
                    if (event.contains("reasoning") && event["reasoning"].is_string())
                    {
                        const std::string delta = event["reasoning"].get<std::string>();
                        if (!delta.empty())
                        {
                            accumulated_reasoning += delta;
                            Json::Value payload;
                            payload["type"] = "reasoning_delta";
                            payload["conversationId"] = static_cast<Json::Int64>(conversation_id);
                            payload["messageId"] = static_cast<Json::Int64>(assistant_id);
                            payload["reasoning"] = accumulated_reasoning;
                            if (!sendSseEvent(stream, payload))
                                return false;
                        }
                    }

                    if (event.contains("content") && event["content"].is_string())
                    {
                        const std::string delta = event["content"].get<std::string>();
                        if (!delta.empty())
                        {
                            accumulated_text += delta;
                            Json::Value payload;
                            payload["type"] = "content_delta";
                            payload["conversationId"] = static_cast<Json::Int64>(conversation_id);
                            payload["messageId"] = static_cast<Json::Int64>(assistant_id);
                            payload["text"] = accumulated_text;
                            if (!sendSseEvent(stream, payload))
                                return false;
                        }
                    }
                    return true;
                }

                if (type == "message.completed")
                {
                    if (event.contains("content") && event["content"].is_string())
                        accumulated_text = event["content"].get<std::string>();
                    return true;
                }

                if (type == "error")
                {
                    if (event.contains("error") && event["error"].is_object())
                    {
                        const auto& err = event["error"];
                        if (err.contains("message") && err["message"].is_string())
                            accumulated_text = err["message"].get<std::string>();
                    }
                    if (accumulated_text.empty())
                        accumulated_text = "AI 응답을 생성하지 못했습니다. 잠시 후 다시 시도해주세요.";
                    return true;
                }

                return true;
            },
            agent_error);

        if (agent_result != service::AgentClientResult::success &&
            agent_result != service::AgentClientResult::cancelled)
        {
            LOG_ERROR("Agent stream failed: {}", agent_error);
            if (accumulated_text.empty())
                accumulated_text = "AI 응답을 생성하지 못했습니다. 잠시 후 다시 시도해주세요.";
        }
        else if (agent_result == service::AgentClientResult::cancelled && accumulated_text.empty())
        {
            accumulated_text = "AI 응답을 생성하지 못했습니다. 잠시 후 다시 시도해주세요.";
        }

        return true;
    }

    void runStreamingTurn(
        std::shared_ptr<drogon::ResponseStream> stream,
        int64_t user_id,
        int64_t conversation_id,
        const Json::Value& user_message,
        Json::Value conversation_for_event,
        bool is_new_conversation)
    {
        ChatStore store(AppState::get().db());
        const auto messages = conversation_for_event["messages"];
        const int64_t assistant_id = store.nextMessageId(messages);
        const std::string assistant_created_at = formatTimestamp();
        const Json::Value assistant_shell = store.makeAssistantShell(assistant_id, assistant_created_at);

        Json::Value user_added;
        user_added["type"] = "user_added";
        user_added["conversationId"] = static_cast<Json::Int64>(conversation_id);
        user_added["message"] = user_message;
        if (is_new_conversation)
            user_added["conversation"] = conversation_for_event;
        if (!sendSseEvent(stream, user_added))
            return;

        Json::Value assistant_start;
        assistant_start["type"] = "assistant_start";
        assistant_start["conversationId"] = static_cast<Json::Int64>(conversation_id);
        assistant_start["message"] = assistant_shell;
        if (!sendSseEvent(stream, assistant_start))
            return;

        std::string accumulated_text;
        std::string accumulated_reasoning;
        Json::Value tool_events(Json::arrayValue);

        runAgentStreamCore(
            stream,
            user_id,
            conversation_id,
            assistant_id,
            store.buildAgentMessages(messages),
            accumulated_text,
            accumulated_reasoning,
            tool_events);

        store.appendAssistantMessage(
            user_id,
            conversation_id,
            assistant_id,
            accumulated_text,
            tool_events,
            accumulated_reasoning);

        Json::Value message_done;
        message_done["type"] = "message_done";
        message_done["conversationId"] = static_cast<Json::Int64>(conversation_id);
        message_done["messageId"] = static_cast<Json::Int64>(assistant_id);
        message_done["text"] = accumulated_text;
        sendSseEvent(stream, message_done);

        if (stream)
            stream->close();
    }

    void startStreamResponse(
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t user_id,
        std::optional<int64_t> conversation_id,
        const std::string& text)
    {
        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
            [user_id, conversation_id, text](drogon::ResponseStreamPtr response_stream)
            {
                std::thread([user_id, conversation_id, text, response = std::shared_ptr<drogon::ResponseStream>{
                                 std::move(response_stream)}]() mutable
                {
                    ChatStore store(AppState::get().db());
                    std::string error;
                    std::string field;

                    if (conversation_id)
                    {
                        const auto appended = store.appendUserMessage(user_id, *conversation_id, text, error, field);
                        if (!appended)
                        {
                            if (response)
                                response->close();
                            return;
                        }

                        auto conversation = store.getConversation(user_id, *conversation_id);
                        if (!conversation)
                        {
                            if (response)
                                response->close();
                            return;
                        }

                        runStreamingTurn(
                            response,
                            user_id,
                            *conversation_id,
                            (*appended)["userMessage"],
                            *conversation,
                            false);
                        return;
                    }

                    const auto created = store.createConversationWithUserMessage(text, user_id, error, field);
                    if (!created)
                    {
                        if (response)
                            response->close();
                        return;
                    }

                    runStreamingTurn(
                        response,
                        user_id,
                        (*created)["id"].asInt64(),
                        (*created)["userMessage"],
                        *created,
                        true);
                }).detach();
            },
            true);

        resp->setContentTypeString("text/event-stream");
        resp->addHeader("Cache-Control", "no-cache, no-store");
        resp->addHeader("Connection", "keep-alive");
        callback(resp);
    }

    void startEphemeralStreamResponse(
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t user_id,
        int64_t conversation_id,
        int64_t assistant_message_id,
        const Json::Value& messages_json)
    {
        const auto agent_messages = buildAgentMessagesFromJson(messages_json);
        if (agent_messages.empty())
        {
            respondError(callback, 400, "INVALID_MESSAGE", "메시지 기록이 비어 있습니다.", "messages");
            return;
        }

        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
            [user_id, conversation_id, assistant_message_id, agent_messages](drogon::ResponseStreamPtr response_stream)
            {
                std::thread([user_id, conversation_id, assistant_message_id, agent_messages, response = std::shared_ptr<drogon::ResponseStream>{
                                 std::move(response_stream)}]() mutable
                {
                    std::string accumulated_text;
                    std::string accumulated_reasoning;
                    Json::Value tool_events(Json::arrayValue);

                    runAgentStreamCore(
                        response,
                        user_id,
                        conversation_id,
                        assistant_message_id,
                        agent_messages,
                        accumulated_text,
                        accumulated_reasoning,
                        tool_events);

                    Json::Value message_done;
                    message_done["type"] = "message_done";
                    message_done["conversationId"] = static_cast<Json::Int64>(conversation_id);
                    message_done["messageId"] = static_cast<Json::Int64>(assistant_message_id);
                    message_done["text"] = accumulated_text;
                    sendSseEvent(response, message_done);

                    if (response)
                        response->close();
                }).detach();
            },
            true);

        resp->setContentTypeString("text/event-stream");
        resp->addHeader("Cache-Control", "no-cache, no-store");
        resp->addHeader("Connection", "keep-alive");
        callback(resp);
    }

    void handleStreamEphemeral(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback)
    {
        const auto user_id = resolveUserId(req, callback);
        if (!user_id)
            return;

        const auto json = req->getJsonObject();
        if (!json)
        {
            respondError(callback, 400, "INVALID_REQUEST", "요청 본문이 필요합니다.");
            return;
        }

        std::string error;
        const auto conversation_id = parseRequiredInt64(*json, "conversationId", error);
        if (!conversation_id)
        {
            respondError(callback, 400, "INVALID_REQUEST", error);
            return;
        }

        const auto assistant_message_id = parseRequiredInt64(*json, "assistantMessageId", error);
        if (!assistant_message_id)
        {
            respondError(callback, 400, "INVALID_REQUEST", error);
            return;
        }

        if (!json->isMember("messages") || !(*json)["messages"].isArray())
        {
            respondError(callback, 400, "INVALID_REQUEST", "messages 배열이 필요합니다.", "messages");
            return;
        }

        startEphemeralStreamResponse(
            std::move(callback),
            *user_id,
            *conversation_id,
            *assistant_message_id,
            (*json)["messages"]);
    }
}

void ChatController::listConversations(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    ChatStore store(AppState::get().db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listSummaries(*user_id)));
}

void ChatController::createConversation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    const auto json = req->getJsonObject();
    ChatStore store(AppState::get().db());

    if (json && json->isMember("initialMessage"))
    {
        std::string error;
        std::string field;
        const auto text = extractText(json, error, field);
        if (text.empty())
        {
            respondError(callback, 400, "INVALID_MESSAGE", error, field);
            return;
        }

        const auto created = store.createConversationWithUserMessage(text, *user_id, error, field);
        if (!created)
        {
            respondError(callback, 400, "INVALID_MESSAGE", error, field);
            return;
        }

        const int64_t conversation_id = (*created)["id"].asInt64();
        std::string assistant_text;
        std::string model;
        std::string agent_error;
        const auto agent_messages = store.buildAgentMessages((*created)["messages"]);
        const auto agent_result = callAgentSync(conversation_id, *user_id, agent_messages, assistant_text, model, agent_error);
        if (agent_result != service::AgentClientResult::success)
        {
            respondError(callback, 502, "AI_PROVIDER_ERROR", "AI 응답을 생성하지 못했습니다. 잠시 후 다시 시도해주세요.");
            return;
        }

        const int64_t assistant_id = store.nextMessageId((*created)["messages"]);
        store.appendAssistantMessage(*user_id, conversation_id, assistant_id, assistant_text);
        const auto conversation = store.getConversation(*user_id, conversation_id);
        if (!conversation)
        {
            respondError(callback, 500, "INTERNAL_ERROR", "대화를 저장하지 못했습니다.");
            return;
        }

        auto resp = drogon::HttpResponse::newHttpJsonResponse(*conversation);
        resp->setStatusCode(drogon::k201Created);
        callback(resp);
        return;
    }

    std::string title;
    if (json && json->isMember("title") && (*json)["title"].isString())
        title = (*json)["title"].asString();

    if (title.empty())
    {
        respondError(callback, 400, "INVALID_TITLE", "대화 제목 또는 첫 메시지를 입력해주세요.", "title");
        return;
    }

    const auto conversation = store.createConversation(*user_id, title);
    if (!conversation)
    {
        respondError(callback, 400, "INVALID_TITLE", "대화 제목 또는 첫 메시지를 입력해주세요.", "title");
        return;
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(*conversation);
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void ChatController::getConversation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string conversationId)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    const auto id = parseConversationId(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    ChatStore store(AppState::get().db());
    const auto conversation = store.getConversation(*user_id, *id);
    if (!conversation)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*conversation));
}

void ChatController::renameConversation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string conversationId)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    const auto id = parseConversationId(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("title") || !(*json)["title"].isString())
    {
        respondError(callback, 400, "INVALID_TITLE", "대화 제목을 입력해주세요.", "title");
        return;
    }

    ChatStore store(AppState::get().db());
    std::string error;
    std::string field;
    const auto summary = store.renameConversation(*user_id, *id, (*json)["title"].asString(), error, field);
    if (!summary)
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        const std::string code = status == 404 ? "NOT_FOUND" : "INVALID_TITLE";
        respondError(callback, status, code, error, field);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*summary));
}

void ChatController::deleteConversation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string conversationId)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    const auto id = parseConversationId(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    ChatStore store(AppState::get().db());
    if (!store.deleteConversation(*user_id, *id))
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
}

void ChatController::appendMessage(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string conversationId)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    const auto id = parseConversationId(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    std::string error;
    std::string field;
    const auto text = extractText(json, error, field);
    if (text.empty())
    {
        respondError(callback, 400, "INVALID_MESSAGE", error, field);
        return;
    }

    ChatStore store(AppState::get().db());
    const auto appended = store.appendUserMessage(*user_id, *id, text, error, field);
    if (!appended)
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        const std::string code = status == 404 ? "NOT_FOUND" : "INVALID_MESSAGE";
        respondError(callback, status, code, error, field);
        return;
    }

    std::string assistant_text;
    std::string model;
    std::string agent_error;
    const auto agent_messages = store.buildAgentMessages((*appended)["messages"]);
    const auto agent_result = callAgentSync(*id, *user_id, agent_messages, assistant_text, model, agent_error);
    if (agent_result != service::AgentClientResult::success)
    {
        respondError(callback, 502, "AI_PROVIDER_ERROR", "AI 응답을 생성하지 못했습니다. 잠시 후 다시 시도해주세요.");
        return;
    }

    const int64_t assistant_id = store.nextMessageId((*appended)["messages"]);
    store.appendAssistantMessage(*user_id, *id, assistant_id, assistant_text);

    Json::Value user_message = (*appended)["userMessage"];
    Json::Value assistant_message;
    assistant_message["id"] = static_cast<Json::Int64>(assistant_id);
    assistant_message["role"] = "assistant";
    assistant_message["text"] = assistant_text;
    assistant_message["createdAt"] = ChatStore::toCreatedAtIso(formatTimestamp());

    Json::Value body;
    body["conversationId"] = static_cast<Json::Int64>(*id);
    Json::Value appended_messages(Json::arrayValue);
    appended_messages.append(user_message);
    appended_messages.append(assistant_message);
    body["appendedMessages"] = appended_messages;

    Json::Value conversation;
    conversation["id"] = static_cast<Json::Int64>(*id);
    const auto current = store.getConversation(*user_id, *id);
    conversation["title"] = current ? (*current)["title"] : "";
    conversation["updatedAt"] = (*appended)["updatedAt"];
    body["conversation"] = conversation;

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void ChatController::streamMessage(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string conversationId)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    const auto id = parseConversationId(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    std::string error;
    std::string field;
    const auto text = extractText(json, error, field);
    if (text.empty())
    {
        respondError(callback, 400, "INVALID_MESSAGE", error, field);
        return;
    }

    ChatStore store(AppState::get().db());
    if (!store.getConversation(*user_id, *id))
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    startStreamResponse(std::move(callback), *user_id, id, text);
}

void ChatController::streamNewConversation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    const auto json = req->getJsonObject();
    std::string error;
    std::string field;
    const auto text = extractText(json, error, field);
    if (text.empty())
    {
        respondError(callback, 400, "INVALID_MESSAGE", error, field);
        return;
    }

    startStreamResponse(std::move(callback), *user_id, std::nullopt, text);
}

void ChatController::streamEphemeral(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    handleStreamEphemeral(req, std::move(callback));
}

void ChatController::getSuggestions(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    ChatStore store(AppState::get().db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.defaultSuggestions()));
}

void ChatController::askInsight(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolveUserId(req, callback);
    if (!user_id)
        return;

    const auto json = req->getJsonObject();
    std::string error;
    std::string field;
    const auto text = extractText(json, error, field);
    if (text.empty())
    {
        respondError(callback, 400, "INVALID_MESSAGE", error, field);
        return;
    }

    std::vector<service::AgentChatMessage> messages;
    messages.push_back({"user", text});

    std::string reply;
    std::string model;
    std::string agent_error;
    const auto agent_result = callAgentSync(0, *user_id, messages, reply, model, agent_error);
    if (agent_result != service::AgentClientResult::success)
    {
        respondError(callback, 502, "AI_PROVIDER_ERROR", "AI 응답을 생성하지 못했습니다. 잠시 후 다시 시도해주세요.");
        return;
    }

    Json::Value body;
    body["reply"] = reply;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
