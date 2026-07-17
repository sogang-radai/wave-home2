#include "real_schedule_tasks_facade.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

Json::Value RealScheduleTasksFacade::list(
    const ScheduleTaskListFilter& filter,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    return web::internal::ScheduleTasksInternalStore(client).list(filter);
}

std::optional<Json::Value> RealScheduleTasksFacade::create(
    const Json::Value& body,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    return web::internal::ScheduleTasksInternalStore(client).create(body, error, field);
}

std::optional<Json::Value> RealScheduleTasksFacade::update(
    int64_t user_id,
    int64_t task_id,
    const Json::Value& body,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    return web::internal::ScheduleTasksInternalStore(client).update(user_id, task_id, body, error, field);
}

std::optional<Json::Value> RealScheduleTasksFacade::remove(
    int64_t user_id,
    int64_t task_id,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error)
{
    return web::internal::ScheduleTasksInternalStore(client).remove(user_id, task_id, error);
}

} // namespace facade
WAVE_NAMESPACE_END
