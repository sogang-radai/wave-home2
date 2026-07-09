#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class IotController :
    public drogon::HttpController<IotController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(IotController::getSummary, "/api/v1/iot/summary", drogon::Get);
    ADD_METHOD_TO(IotController::listDevices, "/api/v1/iot/devices", drogon::Get);
    ADD_METHOD_TO(IotController::getDeviceState, "/api/v1/iot/devices/{deviceId}/state", drogon::Get);
    ADD_METHOD_TO(IotController::queryDevice, "/api/v1/iot/devices/{deviceId}/query/{queryName}", drogon::Get);
    ADD_METHOD_TO(IotController::invokeDevice, "/api/v1/iot/devices/{deviceId}/actions/{actionName}", drogon::Post);
    ADD_METHOD_TO(IotController::reconnectDevice, "/api/v1/iot/devices/{deviceId}/reconnect", drogon::Post);
    ADD_METHOD_TO(IotController::listEvents, "/api/v1/iot/events", drogon::Get);
    ADD_METHOD_TO(IotController::getCameraStream, "/api/v1/iot/devices/{deviceId}/stream", drogon::Get);
    ADD_METHOD_TO(IotController::setCameraStream, "/api/v1/iot/devices/{deviceId}/stream", drogon::Put);
    ADD_METHOD_TO(IotController::exchangeCameraWebRtc, "/api/v1/iot/devices/{deviceId}/stream/webrtc", drogon::Post);
    ADD_METHOD_TO(IotController::streamMp4, "/api/v1/iot/devices/{deviceId}/stream/mp4", drogon::Get);
    ADD_METHOD_TO(IotController::streamMjpeg, "/api/v1/iot/devices/{deviceId}/stream/mjpeg", drogon::Get);
    ADD_METHOD_TO(IotController::getPtzCapabilities, "/api/v1/iot/devices/{deviceId}/ptz/capabilities", drogon::Get);
    ADD_METHOD_TO(IotController::movePtz, "/api/v1/iot/devices/{deviceId}/ptz/move", drogon::Post);
    ADD_METHOD_TO(IotController::stopPtz, "/api/v1/iot/devices/{deviceId}/ptz/stop", drogon::Post);
    ADD_METHOD_TO(IotController::zoomPtz, "/api/v1/iot/devices/{deviceId}/ptz/zoom", drogon::Post);
    ADD_METHOD_TO(IotController::captureSnapshot, "/api/v1/iot/devices/{deviceId}/snapshot", drogon::Post);
    ADD_METHOD_TO(IotController::sendTts, "/api/v1/iot/devices/{deviceId}/tts", drogon::Post);
    ADD_METHOD_TO(IotController::streamWaveStationTelemetry, "/api/v1/iot/devices/{deviceId}/telemetry/stream", drogon::Get);
    ADD_METHOD_TO(IotController::listIrCommands, "/api/v1/iot/ir-commands", drogon::Get);
    ADD_METHOD_TO(IotController::saveIrCommand, "/api/v1/iot/ir-commands", drogon::Put);
    ADD_METHOD_TO(IotController::deleteIrCommand, "/api/v1/iot/ir-commands/{commandId}", drogon::Delete);
    ADD_METHOD_TO(IotController::learnIrCommand, "/api/v1/iot/ir-commands/learn", drogon::Post);
    ADD_METHOD_TO(IotController::listGestureSets, "/api/v1/iot/gesture-sets", drogon::Get);
    ADD_METHOD_TO(IotController::getGestureSetDefinition, "/api/v1/iot/gesture-sets/{gestureSetId}", drogon::Get);
    ADD_METHOD_TO(IotController::getRadarGestureSet, "/api/v1/iot/devices/{deviceId}/gesture-set", drogon::Get);
    ADD_METHOD_TO(IotController::setRadarGestureSet, "/api/v1/iot/devices/{deviceId}/gesture-set", drogon::Put);
    METHOD_LIST_END

    void getSummary(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void listDevices(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getDeviceState(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void queryDevice(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId,
        std::string queryName);

    void invokeDevice(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId,
        std::string actionName);

    void reconnectDevice(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void listEvents(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getCameraStream(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void setCameraStream(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void exchangeCameraWebRtc(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void streamMp4(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void streamMjpeg(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void getPtzCapabilities(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void movePtz(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void stopPtz(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void zoomPtz(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void captureSnapshot(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void sendTts(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void streamWaveStationTelemetry(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void listIrCommands(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void saveIrCommand(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void deleteIrCommand(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string commandId);

    void learnIrCommand(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void listGestureSets(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getGestureSetDefinition(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string gestureSetId);

    void getRadarGestureSet(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void setRadarGestureSet(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
