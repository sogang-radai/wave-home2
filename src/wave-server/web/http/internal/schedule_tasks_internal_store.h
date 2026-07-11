#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {

struct ScheduleTaskListFilter
{
    int64_t user_id = 0;
    std::optional<std::string> day_of_week;
    std::optional<std::string> event_date;
    std::optional<std::string> schedule_kind;
    std::optional<std::string> from;
    std::optional<std::string> to;
    std::optional<bool> done;
};

class ScheduleTasksInternalStore
{
public:
    explicit ScheduleTasksInternalStore(drogon::orm::DbClientPtr client);

    Json::Value list(const ScheduleTaskListFilter& filter) const;
    std::optional<Json::Value> getById(int64_t user_id, int64_t task_id) const;
    std::optional<Json::Value> create(const Json::Value& body, std::string& error, std::string& field) const;
    std::optional<Json::Value> update(
        int64_t user_id,
        int64_t task_id,
        const Json::Value& body,
        std::string& error,
        std::string& field) const;
    std::optional<Json::Value> remove(int64_t user_id, int64_t task_id, std::string& error) const;
    /** insight 취소(apply 되돌리기) 시 파생된 schedule_task 를 정리한다. */
    void removeBySourceInsight(int64_t user_id, int64_t insight_id) const;

private:
    drogon::orm::DbClientPtr m_client;

    Json::Value rowToJson(const drogon::orm::Row& row) const;
    int64_t nextId() const;
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
