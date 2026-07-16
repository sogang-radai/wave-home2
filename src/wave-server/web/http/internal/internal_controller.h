#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace internal {

class InternalController :
    public drogon::HttpController<InternalController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(InternalController::queryDb, "/internal/v1/db/query", drogon::Post);
    ADD_METHOD_TO(InternalController::searchRag, "/internal/v1/rag/search", drogon::Post);
    ADD_METHOD_TO(InternalController::listDevices, "/internal/v1/devices", drogon::Get);
    ADD_METHOD_TO(InternalController::getDevice, "/internal/v1/devices/{deviceId}", drogon::Get);
    ADD_METHOD_TO(InternalController::listDeviceClasses, "/internal/v1/device-classes", drogon::Get);
    ADD_METHOD_TO(InternalController::getDeviceState, "/internal/v1/devices/{deviceId}/state", drogon::Get);
    ADD_METHOD_TO(InternalController::queryDevice, "/internal/v1/devices/{deviceId}/query/{queryName}", drogon::Post);
    ADD_METHOD_TO(InternalController::invokeDeviceAction, "/internal/v1/devices/{deviceId}/actions/{actionName}", drogon::Post);
    ADD_METHOD_TO(InternalController::getPtzCapabilities, "/internal/v1/devices/{deviceId}/ptz/capabilities", drogon::Get);
    ADD_METHOD_TO(InternalController::movePtz, "/internal/v1/devices/{deviceId}/ptz/move", drogon::Post);
    ADD_METHOD_TO(InternalController::stopPtz, "/internal/v1/devices/{deviceId}/ptz/stop", drogon::Post);
    ADD_METHOD_TO(InternalController::zoomPtz, "/internal/v1/devices/{deviceId}/ptz/zoom", drogon::Post);
    ADD_METHOD_TO(InternalController::getCameraStream, "/internal/v1/devices/{deviceId}/stream", drogon::Get);
    ADD_METHOD_TO(InternalController::setCameraStream, "/internal/v1/devices/{deviceId}/stream", drogon::Put);
    ADD_METHOD_TO(InternalController::captureSnapshot, "/internal/v1/devices/{deviceId}/snapshot", drogon::Post);
    ADD_METHOD_TO(InternalController::sendTts, "/internal/v1/devices/{deviceId}/tts", drogon::Post);
    ADD_METHOD_TO(InternalController::listRules, "/internal/v1/rules", drogon::Get);
    ADD_METHOD_TO(InternalController::getRule, "/internal/v1/rules/{ruleId}", drogon::Get);
    ADD_METHOD_TO(InternalController::createRule, "/internal/v1/rules", drogon::Post);
    ADD_METHOD_TO(InternalController::updateRule, "/internal/v1/rules/{ruleId}", drogon::Put);
    ADD_METHOD_TO(InternalController::deleteRule, "/internal/v1/rules/{ruleId}", drogon::Delete);
    ADD_METHOD_TO(InternalController::setRuleEnabled, "/internal/v1/rules/{ruleId}/enabled", drogon::Put);
    ADD_METHOD_TO(InternalController::executeRule, "/internal/v1/rules/{ruleId}/execute", drogon::Post);
    ADD_METHOD_TO(InternalController::listIrCommands, "/internal/v1/ir-commands", drogon::Get);
    ADD_METHOD_TO(InternalController::getIrCommand, "/internal/v1/ir-commands/{commandId}", drogon::Get);
    ADD_METHOD_TO(InternalController::listEvents, "/internal/v1/events", drogon::Get);
    ADD_METHOD_TO(InternalController::toolListDevices, "/internal/v1/tools/device.list", drogon::Post);
    ADD_METHOD_TO(InternalController::toolControlDevice, "/internal/v1/tools/device.control", drogon::Post);
    ADD_METHOD_TO(InternalController::toolQueryDevice, "/internal/v1/tools/device.query", drogon::Post);
    ADD_METHOD_TO(InternalController::toolSchedule, "/internal/v1/tools/device.schedule", drogon::Post);
    ADD_METHOD_TO(InternalController::toolScheduleList, "/internal/v1/tools/device.schedule.list", drogon::Post);
    ADD_METHOD_TO(InternalController::toolScheduleCancel, "/internal/v1/tools/device.schedule.cancel", drogon::Post);
    ADD_METHOD_TO(InternalController::listAlarms, "/internal/v1/alarms", drogon::Get);
    ADD_METHOD_TO(InternalController::createAlarm, "/internal/v1/alarms", drogon::Post);
    ADD_METHOD_TO(InternalController::updateAlarm, "/internal/v1/alarms/{id}", drogon::Patch);
    ADD_METHOD_TO(InternalController::deleteAlarm, "/internal/v1/alarms/{id}", drogon::Delete);
    ADD_METHOD_TO(InternalController::listScheduleTasks, "/internal/v1/schedule-tasks", drogon::Get);
    ADD_METHOD_TO(InternalController::createScheduleTask, "/internal/v1/schedule-tasks", drogon::Post);
    ADD_METHOD_TO(InternalController::updateScheduleTask, "/internal/v1/schedule-tasks/{taskId}", drogon::Patch);
    ADD_METHOD_TO(InternalController::deleteScheduleTask, "/internal/v1/schedule-tasks/{taskId}", drogon::Delete);
    METHOD_LIST_END

    void queryDb(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void searchRag(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void listDevices(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getDevice(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, std::string deviceId);

    void listDeviceClasses(
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

    void invokeDeviceAction(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId,
        std::string actionName);

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

    void getCameraStream(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string deviceId);

    void setCameraStream(
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

    void listRules(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);

    void createRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void updateRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);

    void deleteRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);

    void setRuleEnabled(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);

    void executeRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);

    void listIrCommands(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getIrCommand(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string commandId);

    void listEvents(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void toolListDevices(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void toolControlDevice(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void toolQueryDevice(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void toolSchedule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void toolScheduleList(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void toolScheduleCancel(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void listAlarms(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void createAlarm(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void updateAlarm(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string id);

    void deleteAlarm(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string id);

    void listScheduleTasks(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void createScheduleTask(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void updateScheduleTask(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string taskId);

    void deleteScheduleTask(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string taskId);
};

} // namespace internal
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
