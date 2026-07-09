#include "iot_controller.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <json/writer.h>
#include <thread>
#include <vector>

#include "../../../app/app_state.h"
#include "../../../core/logger.h"
#include "../../../device/platform/droid_cam.h"
#include "../../../device/platform/radai_ws.h"
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

    class PhoneMjpegStream
    {
    public:
        void setStopFlag(std::shared_ptr<std::atomic<bool>> flag)
        {
            m_stop = std::move(flag);
        }

        bool open(const std::string& host, uint16_t port, const std::string& path)
        {
            if (m_stop && m_stop->load(std::memory_order_acquire))
                return false;

            close();

            m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (m_fd < 0)
                return false;

            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            {
                close();
                return false;
            }

            const int flags = ::fcntl(m_fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);

            const int connect_rc = ::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (connect_rc < 0 && errno != EINPROGRESS)
            {
                close();
                return false;
            }

            if (connect_rc != 0)
            {
                pollfd pfd {};
                pfd.fd = m_fd;
                pfd.events = POLLOUT;
                if (::poll(&pfd, 1, 3000) <= 0)
                {
                    close();
                    return false;
                }

                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);
                if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 || socket_error != 0)
                {
                    close();
                    return false;
                }
            }

            if (flags >= 0)
                ::fcntl(m_fd, F_SETFL, flags);

            timeval tv {};
            tv.tv_sec = 5;
            ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            std::string request;
            request += "GET " + path + " HTTP/1.1\r\n";
            request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
            request += "Connection: keep-alive\r\n";
            request += "\r\n";

            size_t sent = 0;
            while (sent < request.size())
            {
                const ssize_t n = ::send(m_fd, request.data() + sent, request.size() - sent, 0);
                if (n <= 0)
                {
                    close();
                    return false;
                }
                sent += static_cast<size_t>(n);
            }

            std::string raw;
            while (true)
            {
                char buffer[4096];
                const ssize_t n = ::recv(m_fd, buffer, sizeof(buffer), 0);
                if (n <= 0)
                {
                    close();
                    return false;
                }
                raw.append(buffer, static_cast<size_t>(n));
                if (raw.find("\r\n\r\n") != std::string::npos)
                    break;
            }

            const auto header_end = raw.find("\r\n\r\n");
            m_pending = raw.substr(header_end + 4);
            return true;
        }

        ssize_t read(char* buffer, size_t capacity)
        {
            if (m_fd < 0 || capacity == 0)
                return 0;

            if (m_stop && m_stop->load(std::memory_order_acquire))
                return 0;

            if (m_pending.empty())
            {
                const ssize_t n = ::recv(m_fd, buffer, capacity, 0);
                return n > 0 ? n : 0;
            }

            const size_t n = std::min(capacity, m_pending.size());
            std::memcpy(buffer, m_pending.data(), n);
            m_pending.erase(0, n);
            return static_cast<ssize_t>(n);
        }

        void close()
        {
            if (m_fd >= 0)
            {
                ::close(m_fd);
                m_fd = -1;
            }
            m_pending.clear();
        }

    private:
        int m_fd = -1;
        std::string m_pending;
        std::shared_ptr<std::atomic<bool>> m_stop;
    };
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
    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [stream, stream_name](drogon::ResponseStreamPtr response_stream)
        {
            std::thread([stream, stream_name, response = std::shared_ptr<drogon::ResponseStream>{
                             std::move(response_stream)}]() mutable
            {
                if (!stream->open(stream_name))
                {
                    if (response)
                        response->close();
                    stream->close();
                    return;
                }

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

void IotController::streamMjpeg(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string code;
    std::string host;
    uint16_t port = 0;
    std::string path;
    if (!store.openCameraMjpegStream(deviceId, host, port, path, code))
    {
        respondError(callback, mapCameraErrorStatus(code), code, "스트림을 열 수 없습니다.");
        return;
    }

    auto stream = std::make_shared<PhoneMjpegStream>();
    const auto stop_flag = AppState::get().iot.beginDroidMjpegProxy(deviceId);
    stream->setStopFlag(stop_flag);
    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [stream, deviceId, stop_flag, host = std::move(host), port, path = std::move(path)](
            drogon::ResponseStreamPtr response_stream)
        {
            std::thread([stream, deviceId, stop_flag, host, port, path, response = std::shared_ptr<drogon::ResponseStream>{
                             std::move(response_stream)}]() mutable
            {
                bool phone_lost = false;
                if (!stream->open(host, port, path))
                    phone_lost = !stop_flag->load(std::memory_order_acquire);
                else
                {
                    char buffer[16384];
                    while (response)
                    {
                        if (stop_flag->load(std::memory_order_acquire))
                            break;

                        const ssize_t n = stream->read(buffer, sizeof(buffer));
                        if (n <= 0)
                        {
                            phone_lost = !stop_flag->load(std::memory_order_acquire);
                            break;
                        }
                        if (!response->send(std::string(buffer, static_cast<size_t>(n))))
                            break;
                    }
                }
                if (response)
                    response->close();
                stream->close();
                AppState::get().iot.finishDroidMjpegProxy(deviceId, phone_lost);
            }).detach();
        },
        true);
    resp->setContentTypeString("multipart/x-mixed-replace; boundary=--dcmjpeg");
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
    const auto* device = store.findDevice(deviceId);
    if (!device)
    {
        respondError(callback, 404, "NOT_FOUND", "장치를 찾을 수 없습니다.");
        return;
    }

    const auto class_name = std::string(device->getClass());
    if (class_name != dev::RadaiWs::kClass && class_name != "reolink_e1_pro")
    {
        respondError(callback, 400, "UNSUPPORTED_DEVICE", "TTS를 재생할 수 없는 장치입니다.");
        return;
    }

    if (!state.tts.isReady())
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

void IotController::streamWaveStationTelemetry(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    std::string code;
    (void)store.snapshotWaveStationTelemetry(deviceId, code);
    if (!code.empty())
    {
        const int status = code == "NOT_FOUND" ? 404
            : (code == "DEVICE_OFFLINE" || code == "DEVICE_INITIALIZING") ? 409
            : code == "UNSUPPORTED_DEVICE" ? 400
            : 500;
        respondError(callback, status, code, "텔레메트리를 구독할 수 없습니다.");
        return;
    }

    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [deviceId](drogon::ResponseStreamPtr response_stream)
        {
            std::thread([deviceId, response = std::shared_ptr<drogon::ResponseStream>{
                             std::move(response_stream)}]() mutable
            {
                Json::StreamWriterBuilder builder;
                builder["indentation"] = "";
                while (response)
                {
                    auto& app = AppState::get();
                    IotStore worker(app.deviceManager);
                    std::string worker_code;
                    const auto snapshot = worker.snapshotWaveStationTelemetry(deviceId, worker_code);
                    if (!worker_code.empty())
                        break;

                    const std::string payload = Json::writeString(builder, snapshot);
                    if (!response->send("data: " + payload + "\n\n"))
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
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

void IotController::listIrCommands(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.hasIrStore())
    {
        respondError(callback, 503, "IR_STORE_UNAVAILABLE", "IR 저장소를 사용할 수 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(state.irStore().listCommands()));
}

void IotController::saveIrCommand(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.hasIrStore())
    {
        respondError(callback, 503, "IR_STORE_UNAVAILABLE", "IR 저장소를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json)
    {
        respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    std::string code;
    const auto body = state.irStore().saveCommand(*json, code);
    if (code == "INVALID_BODY" || code == "INVALID_TIMINGS")
        respondError(callback, 400, code, "IR 커맨드를 저장할 수 없습니다.");
    else if (code == "NOT_FOUND")
        respondError(callback, 404, code, "IR 커맨드를 찾을 수 없습니다.");
    else if (!code.empty())
        respondError(callback, 500, code, "IR 커맨드를 저장할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::deleteIrCommand(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string commandId)
{
    auto& state = AppState::get();
    if (!state.hasIrStore())
    {
        respondError(callback, 503, "IR_STORE_UNAVAILABLE", "IR 저장소를 사용할 수 없습니다.");
        return;
    }

    std::string code;
    const auto body = state.irStore().deleteCommand(commandId, code);
    if (code == "NOT_FOUND")
        respondError(callback, 404, code, "IR 커맨드를 찾을 수 없습니다.");
    else if (!code.empty())
        respondError(callback, 500, code, "IR 커맨드를 삭제할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::learnIrCommand(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.hasIrStore())
    {
        respondError(callback, 503, "IR_STORE_UNAVAILABLE", "IR 저장소를 사용할 수 없습니다.");
        return;
    }

    IotStore store(state.deviceManager);
    if (!requireDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("deviceId"))
    {
        respondError(callback, 400, "INVALID_BODY", "deviceId 필드가 필요합니다.");
        return;
    }

    const std::string device_id = (*json)["deviceId"].asString();
    const uint32_t timeout_ms = json->isMember("timeoutMs") ? (*json)["timeoutMs"].asUInt() : 10000;

    std::string code;
    const auto body = store.learnIr(device_id, timeout_ms, code);
    if (code == "IR_LEARN_TIMEOUT")
        respondError(callback, 408, code, "리모컨 신호를 받지 못했습니다. 다시 시도해주세요.");
    else if (code == "NOT_FOUND")
        respondError(callback, 404, code, "Wave Station을 찾을 수 없습니다.");
    else if (code == "DEVICE_OFFLINE" || code == "DEVICE_INITIALIZING")
        respondError(callback, 409, code, "Wave Station이 오프라인 상태입니다.");
    else if (code == "UNSUPPORTED_DEVICE")
        respondError(callback, 400, code, "Wave Station 장치가 아닙니다.");
    else if (!code.empty())
        respondError(callback, 500, code, "IR 학습에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::listGestureSets(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.hasGestureStore())
    {
        respondError(callback, 503, "AUTOMATION_UNAVAILABLE", "제스처 저장소를 사용할 수 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(state.gestureStore().listGestureSets()));
}

void IotController::getGestureSetDefinition(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string gestureSetId)
{
    auto& state = AppState::get();
    if (!state.hasGestureStore())
    {
        respondError(callback, 503, "AUTOMATION_UNAVAILABLE", "제스처 저장소를 사용할 수 없습니다.");
        return;
    }

    std::string code;
    const auto body = state.gestureStore().getGestureSetDefinition(gestureSetId, code);
    if (code == "NOT_FOUND")
        respondError(callback, 404, code, "제스처 셋을 찾을 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::getRadarGestureSet(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    if (!state.hasGestureStore())
    {
        respondError(callback, 503, "AUTOMATION_UNAVAILABLE", "제스처 저장소를 사용할 수 없습니다.");
        return;
    }

    std::string code;
    const auto body = state.gestureStore().getRadarGestureSet(deviceId, code);
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void IotController::setRadarGestureSet(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    if (!state.hasGestureStore())
    {
        respondError(callback, 503, "AUTOMATION_UNAVAILABLE", "제스처 저장소를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    std::string gesture_set_id;
    if (json && json->isMember("gestureSetId") && !(*json)["gestureSetId"].isNull())
    {
        if (!(*json)["gestureSetId"].isString())
        {
            respondError(callback, 400, "INVALID_REQUEST", "gestureSetId 형식이 올바르지 않습니다.");
            return;
        }
        gesture_set_id = (*json)["gestureSetId"].asString();
    }

    std::string code;
    const auto body = state.gestureStore().setRadarGestureSet(deviceId, gesture_set_id, code);
    if (code == "NOT_FOUND")
        respondError(callback, 404, code, "제스처 셋을 찾을 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
