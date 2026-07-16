#include "chat_controller.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include <json/json.h>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../core/logger.h"
#include "util/time_util.h"
#include "../../../service/agent_client.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../demo/demo_session_writes.h"
#include "chat_store.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    std::optional<int64_t> parse_conversation_id(const std::string& raw)
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

    std::optional<int64_t> resolve_user_id(
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

    std::string extract_text(const std::shared_ptr<const Json::Value>& json, std::string& error, std::string& field)
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

    bool send_sse_event(const std::shared_ptr<drogon::ResponseStream>& stream, const Json::Value& event)
    {
        if (!stream)
            return false;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        const std::string payload = Json::writeString(builder, event);
        return stream->send("data: " + payload + "\n\n");
    }

    std::string tool_label(const std::string& name, bool running, bool failed = false)
    {
        static const std::unordered_map<std::string, std::string> labels = {
            {"query_db", "DB 조회"},
            {"rag_search", "메모리 검색"},
            {"list_devices", "기기 목록 조회"},
            {"get_device_classes", "기기 종류 조회"},
            {"get_device_capabilities", "기기 기능 조회"},
            {"query_device", "기기 조회"},
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

    std::string summarize_tool_result(const json& result)
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
            if (result.contains("deviceName") && result["deviceName"].is_string()
                && result.contains("action") && result["action"].is_string())
            {
                return result["deviceName"].get<std::string>() + " · " + result["action"].get<std::string>();
            }
            if (result.contains("ok") && result["ok"].is_boolean())
                return result["ok"].get<bool>() ? "성공" : "실패";
        }
        return {};
    }

    Json::Value nlohmann_to_json_cpp(const json& value)
    {
        if (value.is_null())
            return Json::nullValue;
        Json::Value parsed;
        Json::CharReaderBuilder builder;
        std::string errors;
        const auto text = value.dump();
        std::istringstream stream(text);
        if (Json::parseFromStream(builder, stream, &parsed, &errors))
            return parsed;
        return Json::nullValue;
    }

    Json::Value make_tool_event(
        const std::string& name,
        bool running,
        const std::string& result_summary = {},
        bool failed = false,
        const Json::Value& args = Json::nullValue,
        const Json::Value& result = Json::nullValue,
        const std::string& id = {})
    {
        Json::Value event;
        if (!id.empty())
            event["id"] = id;
        event["name"] = name;
        event["status"] = running ? "running" : (failed ? "failed" : "done");
        event["label"] = tool_label(name, running, failed);
        if (!result_summary.empty())
            event["resultSummary"] = result_summary;
        if (!args.isNull())
            event["args"] = args;
        if (!result.isNull())
            event["result"] = result;
        return event;
    }

    // Prefer agent run id; fall back to first running row with the same name
    // so parallel same-name tools (e.g. many query_device) do not overwrite.
    std::optional<Json::ArrayIndex> find_tool_event_index(
        const Json::Value& tool_events,
        const std::unordered_map<std::string, Json::ArrayIndex>& tool_index,
        const std::string& id,
        const std::string& name,
        bool prefer_running)
    {
        if (!id.empty())
        {
            const auto it = tool_index.find(id);
            if (it != tool_index.end())
                return it->second;
        }
        if (prefer_running)
        {
            for (Json::ArrayIndex i = 0; i < tool_events.size(); ++i)
            {
                const auto& te = tool_events[i];
                if (te.isMember("name") && te["name"].asString() == name
                    && te.isMember("status") && te["status"].asString() == "running")
                    return i;
            }
        }
        return std::nullopt;
    }

    std::string build_agent_now()
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

    void set_agent_turn_now(service::AgentChatTurnRequest& request)
    {
        request.now = build_agent_now();
    }

    service::AgentClientResult call_agent_sync(
        int64_t chat_history_id,
        int64_t user_id,
        const std::vector<service::AgentChatMessage>& messages,
        std::string& out_content,
        std::string& out_model,
        std::string& out_error,
        const std::string& demo_runtime_id = {})
    {
        service::AgentChatTurnRequest request;
        request.chat_history_id = chat_history_id;
        request.user_id = user_id;
        request.messages = messages;
        set_agent_turn_now(request);
        request.demo_runtime_id = demo_runtime_id;
        request.stream = false;

        return service::runChatTurnSync(
            AppState::get().config.agent.base_url,
            request,
            out_content,
            out_model,
            out_error);
    }

    std::vector<service::AgentChatMessage> build_agent_messages_from_json(const Json::Value& messages)
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

    std::string trim_personal_prompt(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, end - start + 1);
    }

    std::string resolve_personal_prompt(int64_t user_id, const std::string& demo_runtime_id)
    {
        if (!demo_runtime_id.empty())
            return demoResolvePersonalPrompt(demo_runtime_id, user_id, AppState::get().db());

        SettingsStore settings(AppState::get().db());
        const auto agent = settings.getAiAgentSettings(user_id);
        if (!agent.isMember("personalPrompt") || !agent["personalPrompt"].isString())
            return {};
        return trim_personal_prompt(agent["personalPrompt"].asString());
    }

    void prepend_personal_prompt(
        std::vector<service::AgentChatMessage>& messages,
        int64_t user_id,
        const std::string& demo_runtime_id)
    {
        // Drop any prior system rows so an updated personal prompt replaces
        // rather than stacking on top of an older injection.
        messages.erase(
            std::remove_if(
                messages.begin(),
                messages.end(),
                [](const service::AgentChatMessage& message) { return message.role == "system"; }),
            messages.end());

        const auto prompt = resolve_personal_prompt(user_id, demo_runtime_id);
        if (prompt.empty())
            return;
        messages.insert(messages.begin(), {"system", prompt});
    }

    std::optional<int64_t> parse_required_int64(
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

    bool run_agent_stream_core(
        const std::shared_ptr<drogon::ResponseStream>& stream,
        int64_t user_id,
        int64_t conversation_id,
        int64_t assistant_id,
        const std::vector<service::AgentChatMessage>& agent_messages,
        std::string& accumulated_text,
        std::string& accumulated_reasoning,
        Json::Value& tool_events,
        const std::string& demo_runtime_id = {})
    {
        service::AgentChatTurnRequest agent_request;
        agent_request.chat_history_id = conversation_id;
        agent_request.user_id = user_id;
        agent_request.messages = agent_messages;
        set_agent_turn_now(agent_request);
        agent_request.demo_runtime_id = demo_runtime_id;
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
                    const std::string id = event.value("id", "");
                    const Json::Value args = event.contains("args")
                        ? nlohmann_to_json_cpp(event["args"])
                        : Json::Value(Json::nullValue);
                    Json::Value stored = make_tool_event(name, true, {}, false, args, Json::nullValue, id);
                    Json::Value payload;
                    payload["type"] = "tool_start";
                    payload["conversationId"] = static_cast<Json::Int64>(conversation_id);
                    payload["messageId"] = static_cast<Json::Int64>(assistant_id);
                    payload["toolEvent"] = stored;
                    if (!send_sse_event(stream, payload))
                        return false;

                    // Never overwrite a completed same-name tool — that caused UI flicker
                    // when many query_device calls shared one name-keyed slot.
                    if (!id.empty())
                    {
                        const auto it = tool_index.find(id);
                        if (it != tool_index.end())
                            tool_events[it->second] = stored;
                        else
                        {
                            tool_index[id] = tool_events.size();
                            tool_events.append(stored);
                        }
                    }
                    else
                    {
                        tool_events.append(stored);
                    }
                    return true;
                }

                if (type == "tool.end")
                {
                    const std::string name = event.value("name", "");
                    const std::string id = event.value("id", "");
                    const bool failed = event.contains("ok") && event["ok"].is_boolean() && !event["ok"].get<bool>();
                    const json& raw_result = event.contains("result") ? event["result"] : json();
                    const std::string result_summary = summarize_tool_result(raw_result);
                    const Json::Value result = raw_result.is_object() || raw_result.is_array()
                        ? nlohmann_to_json_cpp(raw_result)
                        : Json::Value(Json::nullValue);
                    Json::Value prior_args = Json::nullValue;
                    const auto existing = find_tool_event_index(tool_events, tool_index, id, name, true);
                    if (existing && tool_events[*existing].isMember("args"))
                        prior_args = tool_events[*existing]["args"];
                    const std::string stored_id = !id.empty()
                        ? id
                        : (existing && tool_events[*existing].isMember("id")
                            ? tool_events[*existing]["id"].asString()
                            : std::string{});
                    Json::Value stored = make_tool_event(
                        name, false, result_summary, failed, prior_args, result, stored_id);
                    Json::Value payload;
                    payload["type"] = "tool_end";
                    payload["conversationId"] = static_cast<Json::Int64>(conversation_id);
                    payload["messageId"] = static_cast<Json::Int64>(assistant_id);
                    payload["toolEvent"] = stored;
                    if (!send_sse_event(stream, payload))
                        return false;

                    if (existing)
                    {
                        tool_events[*existing] = stored;
                        if (!stored_id.empty())
                            tool_index[stored_id] = *existing;
                    }
                    else
                    {
                        const auto idx = tool_events.size();
                        tool_events.append(stored);
                        if (!stored_id.empty())
                            tool_index[stored_id] = idx;
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
                    if (!send_sse_event(stream, payload))
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
                            if (!send_sse_event(stream, payload))
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
                            if (!send_sse_event(stream, payload))
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

    void run_streaming_turn(
        std::shared_ptr<drogon::ResponseStream> stream,
        int64_t user_id,
        int64_t conversation_id,
        const Json::Value& user_message,
        Json::Value conversation_for_event,
        bool is_new_conversation,
        const std::string& demo_runtime_id)
    {
        ChatStore store(AppState::get().db());
        const auto messages = conversation_for_event["messages"];
        const int64_t assistant_id = demo_runtime_id.empty()
            ? store.nextMessageId(messages)
            : demoNextChatMessageId(messages);
        const std::string assistant_created_at = formatTimestamp();
        const Json::Value assistant_shell = store.makeAssistantShell(assistant_id, assistant_created_at);

        Json::Value user_added;
        user_added["type"] = "user_added";
        user_added["conversationId"] = static_cast<Json::Int64>(conversation_id);
        user_added["message"] = user_message;
        if (is_new_conversation)
            user_added["conversation"] = conversation_for_event;
        if (!send_sse_event(stream, user_added))
            return;

        Json::Value assistant_start;
        assistant_start["type"] = "assistant_start";
        assistant_start["conversationId"] = static_cast<Json::Int64>(conversation_id);
        assistant_start["message"] = assistant_shell;
        if (!send_sse_event(stream, assistant_start))
            return;

        std::string accumulated_text;
        std::string accumulated_reasoning;
        Json::Value tool_events(Json::arrayValue);

        auto agent_messages = store.buildAgentMessages(messages);
        prepend_personal_prompt(agent_messages, user_id, demo_runtime_id);

        run_agent_stream_core(
            stream,
            user_id,
            conversation_id,
            assistant_id,
            agent_messages,
            accumulated_text,
            accumulated_reasoning,
            tool_events,
            demo_runtime_id);

        if (!demo_runtime_id.empty())
        {
            demoAppendChatAssistantMessage(
                demo_runtime_id,
                user_id,
                conversation_id,
                assistant_id,
                accumulated_text,
                tool_events,
                accumulated_reasoning);
        }
        else
        {
            store.appendAssistantMessage(
                user_id,
                conversation_id,
                assistant_id,
                accumulated_text,
                tool_events,
                accumulated_reasoning);
        }

        Json::Value message_done;
        message_done["type"] = "message_done";
        message_done["conversationId"] = static_cast<Json::Int64>(conversation_id);
        message_done["messageId"] = static_cast<Json::Int64>(assistant_id);
        message_done["text"] = accumulated_text;
        send_sse_event(stream, message_done);

        if (stream)
            stream->close();
    }

    void start_stream_response(
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        const drogon::HttpRequestPtr& req,
        int64_t user_id,
        std::optional<int64_t> conversation_id,
        const std::string& text)
    {
        const auto demo_runtime_id = demoVirtualDevicesEnabled()
            ? resolveDemoRuntimeId(req, nullptr)
            : std::string{};

        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
            [user_id, conversation_id, text, demo_runtime_id](drogon::ResponseStreamPtr response_stream)
            {
                std::thread([user_id, conversation_id, text, demo_runtime_id, response = std::shared_ptr<drogon::ResponseStream>{
                                 std::move(response_stream)}]() mutable
                {
                    ChatStore store(AppState::get().db());
                    std::string error;
                    std::string field;
                    const auto client = AppState::get().db();

                    if (conversation_id)
                    {
                        const auto appended = demo_runtime_id.empty()
                            ? store.appendUserMessage(user_id, *conversation_id, text, error, field)
                            : demoAppendChatUserMessage(
                                  demo_runtime_id, user_id, *conversation_id, text, error, client);
                        if (!appended)
                        {
                            if (response)
                                response->close();
                            return;
                        }

                        auto conversation = demo_runtime_id.empty()
                            ? store.getConversation(user_id, *conversation_id)
                            : demoGetChatConversation(demo_runtime_id, user_id, *conversation_id, client);
                        if (!conversation)
                        {
                            if (response)
                                response->close();
                            return;
                        }

                        run_streaming_turn(
                            response,
                            user_id,
                            *conversation_id,
                            (*appended)["userMessage"],
                            *conversation,
                            false,
                            demo_runtime_id);
                        return;
                    }

                    const auto created = demo_runtime_id.empty()
                        ? store.createConversationWithUserMessage(text, user_id, error, field)
                        : demoCreateChatWithUserMessage(demo_runtime_id, user_id, text, error, client);
                    if (!created)
                    {
                        if (response)
                            response->close();
                        return;
                    }

                    run_streaming_turn(
                        response,
                        user_id,
                        (*created)["id"].asInt64(),
                        (*created)["userMessage"],
                        *created,
                        true,
                        demo_runtime_id);
                }).detach();
            },
            true);

        resp->setContentTypeString("text/event-stream");
        resp->addHeader("Cache-Control", "no-cache, no-store");
        resp->addHeader("Connection", "keep-alive");
        if (!demo_runtime_id.empty())
            attachDemoRuntimeCookieIfNeeded(req, resp, demo_runtime_id);
        callback(resp);
    }

    void start_ephemeral_stream_response(
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        const drogon::HttpRequestPtr& req,
        int64_t user_id,
        int64_t conversation_id,
        int64_t assistant_message_id,
        const Json::Value& messages_json)
    {
        auto agent_messages = build_agent_messages_from_json(messages_json);
        if (agent_messages.empty())
        {
            respondError(callback, 400, "INVALID_MESSAGE", "메시지 기록이 비어 있습니다.", "messages");
            return;
        }

        const auto demo_runtime_id = demoVirtualDevicesEnabled()
            ? resolveDemoRuntimeId(req, nullptr)
            : std::string{};
        prepend_personal_prompt(agent_messages, user_id, demo_runtime_id);

        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
            [user_id, conversation_id, assistant_message_id, agent_messages, demo_runtime_id](
                drogon::ResponseStreamPtr response_stream)
            {
                std::thread([user_id, conversation_id, assistant_message_id, agent_messages, demo_runtime_id, response = std::shared_ptr<drogon::ResponseStream>{
                                 std::move(response_stream)}]() mutable
                {
                    std::string accumulated_text;
                    std::string accumulated_reasoning;
                    Json::Value tool_events(Json::arrayValue);

                    run_agent_stream_core(
                        response,
                        user_id,
                        conversation_id,
                        assistant_message_id,
                        agent_messages,
                        accumulated_text,
                        accumulated_reasoning,
                        tool_events,
                        demo_runtime_id);

                    Json::Value message_done;
                    message_done["type"] = "message_done";
                    message_done["conversationId"] = static_cast<Json::Int64>(conversation_id);
                    message_done["messageId"] = static_cast<Json::Int64>(assistant_message_id);
                    message_done["text"] = accumulated_text;
                    send_sse_event(response, message_done);

                    if (response)
                        response->close();
                }).detach();
            },
            true);

        resp->setContentTypeString("text/event-stream");
        resp->addHeader("Cache-Control", "no-cache, no-store");
        resp->addHeader("Connection", "keep-alive");
        if (!demo_runtime_id.empty())
            attachDemoRuntimeCookieIfNeeded(req, resp, demo_runtime_id);
        callback(resp);
    }

    void handle_stream_ephemeral(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback)
    {
        const auto user_id = resolve_user_id(req, callback);
        if (!user_id)
            return;

        const auto json = req->getJsonObject();
        if (!json)
        {
            respondError(callback, 400, "INVALID_REQUEST", "요청 본문이 필요합니다.");
            return;
        }

        std::string error;
        const auto conversation_id = parse_required_int64(*json, "conversationId", error);
        if (!conversation_id)
        {
            respondError(callback, 400, "INVALID_REQUEST", error);
            return;
        }

        const auto assistant_message_id = parse_required_int64(*json, "assistantMessageId", error);
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

        start_ephemeral_stream_response(
            std::move(callback),
            req,
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
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            demoListChatSummaries(runtime_id, *user_id, AppState::get().db()));
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    ChatStore store(AppState::get().db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listSummaries(*user_id)));
}

void ChatController::createConversation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    const auto json = req->getJsonObject();
    ChatStore store(AppState::get().db());
    const bool demo = demoVirtualDevicesEnabled();
    const auto runtime_id = demo ? resolveDemoRuntimeId(req, nullptr) : std::string{};
    const auto client = AppState::get().db();

    if (json && json->isMember("initialMessage"))
    {
        std::string error;
        std::string field;
        const auto text = extract_text(json, error, field);
        if (text.empty())
        {
            respondError(callback, 400, "INVALID_MESSAGE", error, field);
            return;
        }

        const auto created = demo
            ? demoCreateChatWithUserMessage(runtime_id, *user_id, text, error, client)
            : store.createConversationWithUserMessage(text, *user_id, error, field);
        if (!created)
        {
            respondError(callback, 400, "INVALID_MESSAGE", error, field);
            return;
        }

        const int64_t conversation_id = (*created)["id"].asInt64();
        std::string assistant_text;
        std::string model;
        std::string agent_error;
        auto agent_messages = store.buildAgentMessages((*created)["messages"]);
        prepend_personal_prompt(agent_messages, *user_id, runtime_id);
        const auto agent_result = call_agent_sync(
            conversation_id, *user_id, agent_messages, assistant_text, model, agent_error, runtime_id);
        if (agent_result != service::AgentClientResult::success)
        {
            respondError(callback, 502, "AI_PROVIDER_ERROR", "AI 응답을 생성하지 못했습니다. 잠시 후 다시 시도해주세요.");
            return;
        }

        const int64_t assistant_id = demo
            ? demoNextChatMessageId((*created)["messages"])
            : store.nextMessageId((*created)["messages"]);
        if (demo)
        {
            demoAppendChatAssistantMessage(
                runtime_id, *user_id, conversation_id, assistant_id, assistant_text);
        }
        else
        {
            store.appendAssistantMessage(*user_id, conversation_id, assistant_id, assistant_text);
        }
        const auto conversation = demo
            ? demoGetChatConversation(runtime_id, *user_id, conversation_id, client)
            : store.getConversation(*user_id, conversation_id);
        if (!conversation)
        {
            respondError(callback, 500, "INTERNAL_ERROR", "대화를 저장하지 못했습니다.");
            return;
        }

        auto resp = drogon::HttpResponse::newHttpJsonResponse(*conversation);
        resp->setStatusCode(drogon::k201Created);
        if (demo)
            attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
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

    const auto conversation = demo
        ? demoCreateChatConversation(runtime_id, *user_id, title, client)
        : store.createConversation(*user_id, title);
    if (!conversation)
    {
        respondError(callback, 400, "INVALID_TITLE", "대화 제목 또는 첫 메시지를 입력해주세요.", "title");
        return;
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(*conversation);
    resp->setStatusCode(drogon::k201Created);
    if (demo)
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void ChatController::getConversation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string conversationId)
{
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    const auto id = parse_conversation_id(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        const auto conversation =
            demoGetChatConversation(runtime_id, *user_id, *id, AppState::get().db());
        if (!conversation)
        {
            respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(*conversation);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
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
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    const auto id = parse_conversation_id(conversationId);
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

    std::string error;
    std::string field;
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        const auto summary = demoRenameChatConversation(
            runtime_id, *user_id, *id, (*json)["title"].asString(), error, AppState::get().db());
        if (!summary)
        {
            const int status = error.find("찾을") != std::string::npos ? 404 : 400;
            const std::string code = status == 404 ? "NOT_FOUND" : "INVALID_TITLE";
            respondError(callback, status, code, error, field);
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(*summary);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    ChatStore store(AppState::get().db());
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
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    const auto id = parse_conversation_id(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        if (!demoDeleteChatConversation(runtime_id, *user_id, *id, AppState::get().db()))
        {
            respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
            return;
        }
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k204NoContent);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
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
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    const auto id = parse_conversation_id(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    std::string error;
    std::string field;
    const auto text = extract_text(json, error, field);
    if (text.empty())
    {
        respondError(callback, 400, "INVALID_MESSAGE", error, field);
        return;
    }

    ChatStore store(AppState::get().db());
    const bool demo = demoVirtualDevicesEnabled();
    const auto runtime_id = demo ? resolveDemoRuntimeId(req, nullptr) : std::string{};
    const auto client = AppState::get().db();

    const auto appended = demo
        ? demoAppendChatUserMessage(runtime_id, *user_id, *id, text, error, client)
        : store.appendUserMessage(*user_id, *id, text, error, field);
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
    auto agent_messages = store.buildAgentMessages((*appended)["messages"]);
    prepend_personal_prompt(agent_messages, *user_id, runtime_id);
    const auto agent_result = call_agent_sync(
        *id, *user_id, agent_messages, assistant_text, model, agent_error, runtime_id);
    if (agent_result != service::AgentClientResult::success)
    {
        respondError(callback, 502, "AI_PROVIDER_ERROR", "AI 응답을 생성하지 못했습니다. 잠시 후 다시 시도해주세요.");
        return;
    }

    const int64_t assistant_id = demo
        ? demoNextChatMessageId((*appended)["messages"])
        : store.nextMessageId((*appended)["messages"]);
    if (demo)
    {
        demoAppendChatAssistantMessage(
            runtime_id, *user_id, *id, assistant_id, assistant_text);
    }
    else
    {
        store.appendAssistantMessage(*user_id, *id, assistant_id, assistant_text);
    }

    Json::Value user_message = (*appended)["userMessage"];
    Json::Value assistant_message;
    assistant_message["id"] = static_cast<Json::Int64>(assistant_id);
    assistant_message["role"] = "assistant";
    assistant_message["text"] = assistant_text;
    assistant_message["createdAt"] = ChatStore::to_created_at_iso(formatTimestamp());

    Json::Value body;
    body["conversationId"] = static_cast<Json::Int64>(*id);
    Json::Value appended_messages(Json::arrayValue);
    appended_messages.append(user_message);
    appended_messages.append(assistant_message);
    body["appendedMessages"] = appended_messages;

    Json::Value conversation;
    conversation["id"] = static_cast<Json::Int64>(*id);
    const auto current = demo
        ? demoGetChatConversation(runtime_id, *user_id, *id, client)
        : store.getConversation(*user_id, *id);
    conversation["title"] = current ? (*current)["title"] : "";
    conversation["updatedAt"] = (*appended)["updatedAt"];
    body["conversation"] = conversation;

    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    if (demo)
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void ChatController::streamMessage(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string conversationId)
{
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    const auto id = parse_conversation_id(conversationId);
    if (!id)
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    std::string error;
    std::string field;
    const auto text = extract_text(json, error, field);
    if (text.empty())
    {
        respondError(callback, 400, "INVALID_MESSAGE", error, field);
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        if (!demoGetChatConversation(runtime_id, *user_id, *id, AppState::get().db()))
        {
            respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
            return;
        }
        start_stream_response(std::move(callback), req, *user_id, id, text);
        return;
    }

    ChatStore store(AppState::get().db());
    if (!store.getConversation(*user_id, *id))
    {
        respondError(callback, 404, "NOT_FOUND", "대화를 찾을 수 없습니다.");
        return;
    }

    start_stream_response(std::move(callback), req, *user_id, id, text);
}

void ChatController::streamNewConversation(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    const auto json = req->getJsonObject();
    std::string error;
    std::string field;
    const auto text = extract_text(json, error, field);
    if (text.empty())
    {
        respondError(callback, 400, "INVALID_MESSAGE", error, field);
        return;
    }

    start_stream_response(std::move(callback), req, *user_id, std::nullopt, text);
}

void ChatController::streamEphemeral(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    handle_stream_ephemeral(req, std::move(callback));
}

void ChatController::getSuggestions(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    ChatStore store(AppState::get().db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.defaultSuggestions()));
}

void ChatController::askInsight(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto user_id = resolve_user_id(req, callback);
    if (!user_id)
        return;

    const auto json = req->getJsonObject();
    std::string error;
    std::string field;
    const auto text = extract_text(json, error, field);
    if (text.empty())
    {
        respondError(callback, 400, "INVALID_MESSAGE", error, field);
        return;
    }

    const auto demo_runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string{};

    std::vector<service::AgentChatMessage> messages;
    messages.push_back({"user", text});
    prepend_personal_prompt(messages, *user_id, demo_runtime_id);

    std::string reply;
    std::string model;
    std::string agent_error;
    const auto agent_result = call_agent_sync(
        0, *user_id, messages, reply, model, agent_error, demo_runtime_id);
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
