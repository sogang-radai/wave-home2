#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

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
    ADD_METHOD_TO(IotController::listSpeechOverlays, "/api/v1/iot/speech-overlays", drogon::Get);
    METHOD_LIST_END

    void getSummary(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void listDevices(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void getDeviceState(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void queryDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback,
        std::string deviceId,
        std::string queryName);
    void invokeDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback,
        std::string deviceId,
        std::string actionName);
    void reconnectDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void listEvents(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void getCameraStream(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void setCameraStream(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void exchangeCameraWebRtc(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void streamMp4(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void streamMjpeg(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);

    void getPtzCapabilities(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void movePtz(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void stopPtz(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void zoomPtz(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);

    void captureSnapshot(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void sendTts(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void streamWaveStationTelemetry(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);

    void listIrCommands(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void saveIrCommand(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void deleteIrCommand(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string commandId);
    void learnIrCommand(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void listGestureSets(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void getGestureSetDefinition(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string gestureSetId);
    void getRadarGestureSet(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void setRadarGestureSet(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void listSpeechOverlays(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
