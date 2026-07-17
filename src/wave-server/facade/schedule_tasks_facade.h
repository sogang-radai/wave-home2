#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <json/json.h>

#include "../core/coredefs.h"
#include "../db/database.h"
#include "../web/http/internal/schedule_tasks_internal_store.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

using ScheduleTaskListFilter = web::internal::ScheduleTaskListFilter;

class IScheduleTasksFacade
{
public:
    virtual ~IScheduleTasksFacade() = default;

    virtual Json::Value list(
        const ScheduleTaskListFilter& filter,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual std::optional<Json::Value> create(
        const Json::Value& body,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) = 0;

    virtual std::optional<Json::Value> update(
        int64_t user_id,
        int64_t task_id,
        const Json::Value& body,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) = 0;

    virtual std::optional<Json::Value> remove(
        int64_t user_id,
        int64_t task_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error) = 0;
};

} // namespace facade
WAVE_NAMESPACE_END
