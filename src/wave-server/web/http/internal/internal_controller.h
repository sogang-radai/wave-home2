#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

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
    // Debug/manual trigger — normally UserModelManager runs this once per calendar-day
    // rollover on its own background thread; this lets it be fired on demand for testing
    // instead of waiting for real midnight.
    ADD_METHOD_TO(InternalController::runUserModelRollover, "/internal/v1/user-model/rollover", drogon::Post);
    // Debug/manual trigger — weekly power reports are never auto-created on a
    // query_report() cache miss (unlike 24h/1h), and the nightly PowerManager job
    // that normally seeds them doesn't run in demo mode; this forces one for testing.
    ADD_METHOD_TO(InternalController::runWeeklyPowerReport, "/internal/v1/power/reports/weekly", drogon::Post);
    METHOD_LIST_END

    void queryDb(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void searchRag(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void listDevices(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void getDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void listDeviceClasses(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void getDeviceState(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void queryDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback,
        std::string deviceId,
        std::string queryName);
    void invokeDeviceAction(const HttpRequestPtr& req, HttpResponseCallback&& callback,
        std::string deviceId,
        std::string actionName);

    void getPtzCapabilities(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void movePtz(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void stopPtz(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void zoomPtz(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);

    void getCameraStream(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void setCameraStream(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void captureSnapshot(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);
    void sendTts(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId);

    void listRules(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void getRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId);
    void createRule(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void updateRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId);
    void deleteRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId);
    void setRuleEnabled(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId);
    void executeRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId);

    void listIrCommands(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void getIrCommand(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string commandId);

    void listEvents(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void runUserModelRollover(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void runWeeklyPowerReport(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void toolListDevices(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void toolControlDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void toolQueryDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void toolSchedule(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void toolScheduleList(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void toolScheduleCancel(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void listAlarms(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void createAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void updateAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string id);
    void deleteAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string id);

    void listScheduleTasks(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void createScheduleTask(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void updateScheduleTask(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string taskId);
    void deleteScheduleTask(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string taskId);
};

} // namespace internal
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
