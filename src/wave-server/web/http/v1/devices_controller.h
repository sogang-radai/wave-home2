#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class DevicesController :
    public drogon::HttpController<DevicesController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DevicesController::listDevices, "/api/v1/devices", drogon::Get);
    ADD_METHOD_TO(DevicesController::createDevice, "/api/v1/devices", drogon::Post);
    ADD_METHOD_TO(DevicesController::updateDevice, "/api/v1/devices/{deviceId}", drogon::Patch);
    ADD_METHOD_TO(DevicesController::deleteDevice, "/api/v1/devices/{deviceId}", drogon::Delete);
    ADD_METHOD_TO(DevicesController::assignRoom, "/api/v1/devices/{deviceId}/room", drogon::Put);
    ADD_METHOD_TO(DevicesController::unassignRoom, "/api/v1/devices/{deviceId}/room", drogon::Delete);
    METHOD_LIST_END

    void listDevices(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void createDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void updateDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void deleteDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);

    void assignRoom(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void unassignRoom(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
