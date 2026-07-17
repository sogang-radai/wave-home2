#include "demo_schedule_tasks_facade.h"

#include "demo_session_writes.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    Json::Value filter_demo_tasks(const Json::Value& items, const facade::ScheduleTaskListFilter& filter)
    {
        Json::Value filtered(Json::arrayValue);
        for (const auto& item : items)
        {
            if (!item.isObject())
                continue;
            if (filter.day_of_week && item.get("dayOfWeek", "").asString() != *filter.day_of_week)
                continue;
            if (filter.event_date && item.get("eventDate", "").asString() != *filter.event_date)
                continue;
            if (filter.schedule_kind && item.get("scheduleKind", "").asString() != *filter.schedule_kind)
                continue;
            if (filter.from && item.get("eventDate", "").asString() < *filter.from)
                continue;
            if (filter.to && item.get("eventDate", "").asString() >= *filter.to)
                continue;
            if (filter.done && item.get("done", false).asBool() != *filter.done)
                continue;
            filtered.append(item);
        }
        return filtered;
    }
}

Json::Value DemoScheduleTasksFacade::list(
    const facade::ScheduleTaskListFilter& filter,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    return filter_demo_tasks(demoListScheduleTasks(runtime_id, filter.user_id, client), filter);
}

std::optional<Json::Value> DemoScheduleTasksFacade::create(
    const Json::Value& body,
    const std::string& runtime_id,
    const db::DbClientPtr& /*client*/,
    std::string& error,
    std::string& field)
{
    const auto created = demoCreateScheduleTask(runtime_id, body, error, field);
    if (created.isNull())
        return std::nullopt;
    return created;
}

std::optional<Json::Value> DemoScheduleTasksFacade::update(
    int64_t /*user_id*/,
    int64_t task_id,
    const Json::Value& body,
    const std::string& runtime_id,
    const db::DbClientPtr& /*client*/,
    std::string& error,
    std::string& field)
{
    const auto updated = demoUpdateScheduleTask(runtime_id, task_id, body, error, field);
    if (updated.isNull())
        return std::nullopt;
    return updated;
}

std::optional<Json::Value> DemoScheduleTasksFacade::remove(
    int64_t /*user_id*/,
    int64_t task_id,
    const std::string& runtime_id,
    const db::DbClientPtr& /*client*/,
    std::string& error)
{
    if (!demoDeleteScheduleTask(runtime_id, task_id))
    {
        error = "일정을 찾을 수 없습니다.";
        return std::nullopt;
    }
    Json::Value removed;
    removed["id"] = static_cast<Json::Int64>(task_id);
    return removed;
}

WAVE_NAMESPACE_END
