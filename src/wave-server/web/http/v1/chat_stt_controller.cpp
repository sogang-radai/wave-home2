#include "chat_stt_controller.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

#include "../../../app/app_state.h"
#include "../../../core/logger.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    std::optional<int64_t> resolve_user_id(
        const HttpRequestPtr& req,
        const HttpResponseCallback& callback)
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

    int status_for_stt_code(const std::string& code)
    {
        if (code == "STT_BUSY")
            return 503;
        if (code == "STT_UNAVAILABLE")
            return 503;
        if (code == "NOT_FOUND")
            return 404;
        if (code == "INVALID_AUDIO" || code == "INVALID_LOCALE" || code == "INVALID_BODY")
            return 400;
        return 500;
    }

    std::string message_for_stt_code(const std::string& code)
    {
        if (code == "STT_BUSY")
            return "다른 음성 인식 세션이 진행 중입니다.";
        if (code == "STT_UNAVAILABLE")
            return "음성 인식을 사용할 수 없습니다.";
        if (code == "NOT_FOUND")
            return "음성 인식 세션을 찾을 수 없습니다.";
        if (code == "INVALID_AUDIO")
            return "오디오 데이터가 올바르지 않습니다.";
        if (code == "INVALID_LOCALE")
            return "지원하지 않는 로케일입니다.";
        return "음성 인식 요청을 처리하지 못했습니다.";
    }
}

void ChatSttController::createSession(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    if (!resolve_user_id(req, callback))
        return;

#ifndef WAVE_BUILD_TTS
    respondError(callback, 503, "STT_UNAVAILABLE", "음성 인식을 사용할 수 없습니다.");
#else
    std::string locale = "ko-KR";
    if (const auto json = req->getJsonObject())
    {
        if (json->isMember("locale") && (*json)["locale"].isString())
        {
            locale = (*json)["locale"].asString();
            if (locale.empty())
                locale = "ko-KR";
        }
    }

    std::string session_id;
    std::string code;
    if (!AppState::get().stt.createSession(locale, session_id, code))
    {
        respondError(callback, status_for_stt_code(code), code, message_for_stt_code(code));
        return;
    }

    Json::Value body(Json::objectValue);
    body["sessionId"] = session_id;
    body["sampleRate"] = 16000;
    body["locale"] = locale;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
#endif
}

void ChatSttController::streamEvents(
    const HttpRequestPtr& req,
    HttpResponseCallback&& callback,
    std::string sessionId)
{
    if (!resolve_user_id(req, callback))
        return;

#ifndef WAVE_BUILD_TTS
    respondError(callback, 503, "STT_UNAVAILABLE", "음성 인식을 사용할 수 없습니다.");
#else
    if (sessionId.empty())
    {
        respondError(callback, 404, "NOT_FOUND", "음성 인식 세션을 찾을 수 없습니다.");
        return;
    }

    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [sessionId](drogon::ResponseStreamPtr response_stream)
        {
            std::thread([sessionId, response = std::shared_ptr<drogon::ResponseStream>{
                             std::move(response_stream)}]() mutable
            {
                Json::StreamWriterBuilder builder;
                builder["indentation"] = "";
                while (response)
                {
                    Json::Value event;
                    bool closed = false;
                    std::string code;
                    if (!AppState::get().stt.popEvent(
                            sessionId,
                            event,
                            closed,
                            std::chrono::milliseconds(500),
                            code))
                    {
                        break;
                    }

                    if (!event.isNull() && event.isObject() && event.isMember("type"))
                    {
                        const std::string payload = Json::writeString(builder, event);
                        if (!response->send("data: " + payload + "\n\n"))
                            break;
                        if (event["type"].asString() == "done" || event["type"].asString() == "error")
                            break;
                    }
                    else if (closed)
                    {
                        break;
                    }
                }
                if (response)
                    response->close();
            }).detach();
        },
        true);

    resp->setContentTypeString("text/event-stream");
    resp->addHeader("Cache-Control", "no-cache, no-store");
    resp->addHeader("Connection", "keep-alive");
    resp->addHeader("X-Accel-Buffering", "no");
    callback(resp);
#endif
}

void ChatSttController::pushAudio(
    const HttpRequestPtr& req,
    HttpResponseCallback&& callback,
    std::string sessionId)
{
    if (!resolve_user_id(req, callback))
        return;

#ifndef WAVE_BUILD_TTS
    respondError(callback, 503, "STT_UNAVAILABLE", "음성 인식을 사용할 수 없습니다.");
#else
    const auto body = req->getBody();
    if (body.empty() || (body.size() % sizeof(float)) != 0)
    {
        respondError(callback, 400, "INVALID_AUDIO", "오디오 데이터가 올바르지 않습니다.");
        return;
    }

    const auto sample_count = body.size() / sizeof(float);
    std::vector<float> samples(sample_count);
    std::memcpy(samples.data(), body.data(), body.size());

    std::string code;
    if (!AppState::get().stt.pushAudio(sessionId, samples.data(), samples.size(), 16000, code))
    {
        respondError(callback, status_for_stt_code(code), code, message_for_stt_code(code));
        return;
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
#endif
}

void ChatSttController::endSession(
    const HttpRequestPtr& req,
    HttpResponseCallback&& callback,
    std::string sessionId)
{
    if (!resolve_user_id(req, callback))
        return;

#ifndef WAVE_BUILD_TTS
    respondError(callback, 503, "STT_UNAVAILABLE", "음성 인식을 사용할 수 없습니다.");
#else
    std::string code;
    if (!AppState::get().stt.endSession(sessionId, code))
    {
        respondError(callback, status_for_stt_code(code), code, message_for_stt_code(code));
        return;
    }

    Json::Value body(Json::objectValue);
    body["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
#endif
}

void ChatSttController::abortSession(
    const HttpRequestPtr& req,
    HttpResponseCallback&& callback,
    std::string sessionId)
{
    if (!resolve_user_id(req, callback))
        return;

#ifndef WAVE_BUILD_TTS
    respondError(callback, 503, "STT_UNAVAILABLE", "음성 인식을 사용할 수 없습니다.");
#else
    std::string code;
    if (!AppState::get().stt.abortSession(sessionId, code))
    {
        // Idempotent abort: missing session is fine.
        if (code != "NOT_FOUND")
        {
            respondError(callback, status_for_stt_code(code), code, message_for_stt_code(code));
            return;
        }
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
#endif
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
