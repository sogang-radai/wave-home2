#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class AlarmsController : public drogon::HttpController<AlarmsController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AlarmsController::listAlarms, "/api/v1/alarms", drogon::Get);
    ADD_METHOD_TO(AlarmsController::createAlarm, "/api/v1/alarms", drogon::Post);
    ADD_METHOD_TO(AlarmsController::updateAlarm, "/api/v1/alarms/{alarmId}", drogon::Patch);
    ADD_METHOD_TO(AlarmsController::deleteAlarm, "/api/v1/alarms/{alarmId}", drogon::Delete);
    METHOD_LIST_END

    void listAlarms(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void createAlarm(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void updateAlarm(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string alarmId);

    void deleteAlarm(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string alarmId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
