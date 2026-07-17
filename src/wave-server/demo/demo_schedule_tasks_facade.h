#pragma once

#include "../facade/schedule_tasks_facade.h"

WAVE_NAMESPACE_BEGIN

class DemoScheduleTasksFacade :
    public facade::IScheduleTasksFacade
{
public:
    Json::Value list(
        const facade::ScheduleTaskListFilter& filter,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    std::optional<Json::Value> create(
        const Json::Value& body,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) override;

    std::optional<Json::Value> update(
        int64_t user_id,
        int64_t task_id,
        const Json::Value& body,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) override;

    std::optional<Json::Value> remove(
        int64_t user_id,
        int64_t task_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error) override;
};

WAVE_NAMESPACE_END
