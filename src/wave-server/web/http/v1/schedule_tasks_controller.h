#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class ScheduleTasksController :
    public drogon::HttpController<ScheduleTasksController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ScheduleTasksController::listTasks, "/api/v1/schedule-tasks", drogon::Get);
    ADD_METHOD_TO(ScheduleTasksController::createTask, "/api/v1/schedule-tasks", drogon::Post);
    ADD_METHOD_TO(
        ScheduleTasksController::updateTask,
        "/api/v1/schedule-tasks/{taskId}",
        drogon::Patch);
    ADD_METHOD_TO(
        ScheduleTasksController::deleteTask,
        "/api/v1/schedule-tasks/{taskId}",
        drogon::Delete);
    METHOD_LIST_END

    void listTasks(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void createTask(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void updateTask(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string taskId);
    void deleteTask(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string taskId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
