#include "iot_controller.h"

#include <thread>
#include <vector>

#include "../../../app/app_state.h"
#include "../../../core/logger.h"
#include "../../../service/go2rtc_service.h"
#include "iot_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    bool requireDevices(
        const std::function<void(const drogon::HttpResponsePtr&)>& callback,
        IotStore& store)
    {
        if (!store.devicesAvailable())
        {
            respondError(callback, 503, "DEVICES_UNAVAILABLE", "장치 관리자를 사용할 수 없습니다.");
            return false;
        }
        return true;
    }
}

void IotController::getSummary(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;
    callback(drogon::HttpResponse::newHttpJsonResponse(store.getSummary()));
}

void IotController::listDevices(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listDevices()));
}

void IotController::getDeviceState(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string code;
    const auto body = store.queryDevice(deviceId, "status", code);
    if (code == "NOT_FOUND")
        respondError(callback, 404, code, "기기를 찾을 수 없습니다.");
    else if (code == "DEVICE_OFFLINE")
        respondError(callback, 409, code, "장치가 오프라인 상태입니다.");
    else if (code == "DEVICE_INITIALIZING")
        respondError(callback, 409, code, "장치를 초기화하는 중입니다.");
    else if (!code.empty())
        respondError(callback, 500, code, "상태 조회에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::queryDevice(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId,
    std::string queryName)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string code;
    const auto body = store.queryDevice(deviceId, queryName, code);
    if (code == "NOT_FOUND")
        respondError(callback, 404, code, "기기를 찾을 수 없습니다.");
    else if (code == "DEVICE_OFFLINE")
        respondError(callback, 409, code, "장치가 오프라인 상태입니다.");
    else if (code == "DEVICE_INITIALIZING")
        respondError(callback, 409, code, "장치를 초기화하는 중입니다.");
    else if (!code.empty())
        respondError(callback, 500, code, "조회에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::invokeDevice(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId,
    std::string actionName)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    Json::Value params(Json::objectValue);
    if (json && json->isObject())
        params = *json;

    std::string code;
    const auto body = store.invokeDevice(deviceId, actionName, params, code);
    if (code == "NOT_FOUND")
        respondError(callback, 404, code, "기기를 찾을 수 없습니다.");
    else if (code == "DEVICE_OFFLINE")
        respondError(callback, 409, code, "장치가 오프라인 상태입니다.");
    else if (code == "DEVICE_INITIALIZING")
        respondError(callback, 409, code, "장치를 초기화하는 중입니다.");
    else if (code == "ACTION_NOT_FOUND")
        respondError(callback, 404, code, "동작을 찾을 수 없습니다.");
    else if (!code.empty())
        respondError(callback, 500, code, "제어에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::reconnectDevice(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string error;
    std::string code;
    if (!store.reconnectDevice(deviceId, error, code))
    {
        const auto status = code == "NOT_FOUND" ? 404 : 409;
        respondError(callback, status, code, error);
        return;
    }

    Json::Value body;
    body["ok"] = true;
    body["id"] = deviceId;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::listEvents(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    const auto device_id = req->getParameter("deviceId");
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listEvents(device_id)));
}

namespace
{
    int mapCameraErrorStatus(const std::string& code)
    {
        if (code == "NOT_FOUND" || code == "UNSUPPORTED_DEVICE")
            return 404;
        if (code == "DEVICE_OFFLINE" || code == "DEVICE_INITIALIZING" || code == "STREAM_UNAVAILABLE")
            return 409;
        if (code == "INVALID_BODY")
            return 400;
        return 500;
    }
}

void IotController::getCameraStream(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string code;
    const auto body = store.getCameraStream(deviceId, code);
    if (!code.empty())
        respondError(callback, mapCameraErrorStatus(code), code, "스트림 정보를 가져올 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::setCameraStream(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("streaming"))
    {
        respondError(callback, 400, "INVALID_BODY", "streaming 필드가 필요합니다.");
        return;
    }

    std::string code;
    const auto body = store.setCameraStream(deviceId, (*json)["streaming"].asBool(), code);
    if (!code.empty())
        respondError(callback, mapCameraErrorStatus(code), code, "스트림을 시작할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::exchangeCameraWebRtc(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string offer_sdp;
    const auto json = req->getJsonObject();
    if (json && json->isMember("sdp"))
        offer_sdp = (*json)["sdp"].asString();
    else
        offer_sdp = std::string(req->body());

    if (offer_sdp.empty())
    {
        respondError(callback, 400, "INVALID_BODY", "SDP offer가 필요합니다.");
        return;
    }

    std::string answer_sdp;
    std::string code;
    if (!store.exchangeCameraWebRtc(deviceId, offer_sdp, answer_sdp, code))
        respondError(callback, mapCameraErrorStatus(code), code, "WebRTC 연결에 실패했습니다.");
    else
    {
        Json::Value body;
        body["type"] = "answer";
        body["sdp"] = answer_sdp;
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    }
}

void IotController::streamMp4(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string stream_name;
    std::string code;
    if (!store.openCameraMp4Stream(deviceId, stream_name, code))
    {
        respondError(callback, mapCameraErrorStatus(code), code, "스트림을 열 수 없습니다.");
        return;
    }

    auto stream = std::make_shared<service::Go2RtcService::LiveMp4Stream>();
    if (!stream->open(stream_name))
    {
        respondError(callback, 409, "STREAM_UNAVAILABLE", "스트림을 열 수 없습니다.");
        return;
    }

    // Read go2rtc in a background thread so blocking recv() does not stall drogon workers.
    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [stream](drogon::ResponseStreamPtr response_stream)
        {
            std::thread([stream, response = std::shared_ptr<drogon::ResponseStream>{
                             std::move(response_stream)}]() mutable
            {
                char buffer[16384];
                while (response)
                {
                    const ssize_t n = stream->read(buffer, sizeof(buffer));
                    if (n <= 0)
                        break;
                    if (!response->send(std::string(buffer, static_cast<size_t>(n))))
                        break;
                }
                if (response)
                    response->close();
                stream->close();
            }).detach();
        },
        true);
    resp->setContentTypeCode(drogon::CT_VIDEO_MP4);
    resp->addHeader("Cache-Control", "no-cache, no-store");
    resp->addHeader("Pragma", "no-cache");
    callback(resp);
}

void IotController::getPtzCapabilities(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string code;
    const auto body = store.getPtzCapabilities(deviceId, code);
    if (!code.empty())
        respondError(callback, mapCameraErrorStatus(code), code, "PTZ 기능을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::movePtz(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    Json::Value vector(Json::objectValue);
    if (json && json->isObject())
        vector = *json;

    std::string code;
    if (!store.moveCameraPtz(deviceId, vector, code))
        respondError(callback, mapCameraErrorStatus(code), code, "PTZ 이동에 실패했습니다.");
    else
    {
        Json::Value body;
        body["ok"] = true;
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    }
}

void IotController::stopPtz(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string code;
    if (!store.stopCameraPtz(deviceId, code))
        respondError(callback, mapCameraErrorStatus(code), code, "PTZ 정지에 실패했습니다.");
    else
    {
        Json::Value body;
        body["ok"] = true;
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    }
}

void IotController::zoomPtz(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    const int delta = json && json->isMember("delta") ? (*json)["delta"].asInt() : 0;

    std::string code;
    const auto body = store.zoomCameraPtz(deviceId, delta, code);
    if (!code.empty())
        respondError(callback, mapCameraErrorStatus(code), code, "줌 조정에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::captureSnapshot(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::thread([deviceId, callback = std::move(callback), &state]() mutable
    {
        IotStore worker(state.deviceManager);
        std::vector<uint8_t> jpeg;
        std::string occurred_at;
        std::string code;
        if (!worker.captureCameraSnapshot(deviceId, jpeg, occurred_at, code))
        {
            respondError(callback, mapCameraErrorStatus(code), code, "스냅샷 캡처에 실패했습니다.");
            return;
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::ContentType::CT_IMAGE_JPG);
        resp->setBody(std::string(jpeg.begin(), jpeg.end()));
        resp->addHeader("X-Snapshot-At", occurred_at);
        callback(resp);
    }).detach();
}

void IotController::sendTts(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("text"))
    {
        respondError(callback, 400, "INVALID_BODY", "text 필드가 필요합니다.");
        return;
    }

    const std::string text = (*json)["text"].asString();
    const int speaker_id = json->isMember("speakerId") ? (*json)["speakerId"].asInt() : 0;
    const float speed = json->isMember("speed") ? static_cast<float>((*json)["speed"].asDouble()) : 1.0f;

    if (text.empty())
    {
        respondError(callback, 400, "INVALID_BODY", "text 필드가 필요합니다.");
        return;
    }

    std::string code;
    (void)store.getCameraStream(deviceId, code);
    if (!code.empty())
    {
        respondError(callback, mapCameraErrorStatus(code), code, "TTS를 재생할 수 없습니다.");
        return;
    }

    if (!isTtsServiceReady())
    {
        respondError(callback, 503, "TTS_UNAVAILABLE", "TTS 엔진을 사용할 수 없습니다.");
        return;
    }

    queueDeviceTts(deviceId, text, speaker_id, speed);

    Json::Value body;
    body["ok"] = true;
    body["queued"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
