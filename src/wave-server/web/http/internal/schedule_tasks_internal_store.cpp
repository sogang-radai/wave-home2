#include "schedule_tasks_internal_store.h"
#include "../../../db/database.h"

#include <algorithm>
#include <sstream>

#include "util/time_util.h"
#include "../v1/chat_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    std::string trim_copy(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\n\r");
        return value.substr(start, end - start + 1);
    }

    bool is_valid_day(const std::string& day)
    {
        static const char* kDays[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
        return std::any_of(std::begin(kDays), std::end(kDays), [&](const char* candidate) {
            return day == candidate;
        });
    }
}

ScheduleTasksInternalStore::ScheduleTasksInternalStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

int64_t ScheduleTasksInternalStore::nextId() const
{
    auto rows = m_client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM schedule_task");
    return rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();
}

Json::Value ScheduleTasksInternalStore::rowToJson(const drogon::orm::Row& row) const
{
    Json::Value item;
    item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
    item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
    item["title"] = row["title"].as<std::string>();
    if (row["created_at"].isNull())
        item["createdAt"] = Json::nullValue;
    else
        item["createdAt"] = v1::ChatStore::to_created_at_iso(row["created_at"].as<std::string>());
    item["createdBy"] = row["created_by"].as<std::string>();
    item["category"] = row["category"].as<std::string>();
    item["scheduleKind"] = row["schedule_kind"].as<std::string>();
    item["dayOfWeek"] = row["day_of_week"].as<std::string>();
    if (row["event_date"].isNull())
        item["eventDate"] = Json::nullValue;
    else
        item["eventDate"] = row["event_date"].as<std::string>();
    if (row["start_minute"].isNull())
        item["startMinute"] = Json::nullValue;
    else
        item["startMinute"] = row["start_minute"].as<int>();
    if (row["end_minute"].isNull())
        item["endMinute"] = Json::nullValue;
    else
        item["endMinute"] = row["end_minute"].as<int>();
    item["done"] = row["done"].as<int>() != 0;
    if (row["source_insight_id"].isNull())
        item["sourceInsightId"] = Json::nullValue;
    else
        item["sourceInsightId"] = static_cast<Json::Int64>(row["source_insight_id"].as<int64_t>());
    return item;
}

Json::Value ScheduleTasksInternalStore::list(const ScheduleTaskListFilter& filter) const
{
    std::ostringstream sql;
    sql << "SELECT id, user_id, title, created_at, created_by, category, schedule_kind, day_of_week,"
        << " event_date, start_minute, end_minute, done, source_insight_id"
        << " FROM schedule_task WHERE user_id = " << filter.user_id;

    if (filter.day_of_week)
        sql << " AND day_of_week = '" << *filter.day_of_week << "'";
    if (filter.event_date)
        sql << " AND event_date = '" << *filter.event_date << "'";
    if (filter.schedule_kind)
        sql << " AND schedule_kind = '" << *filter.schedule_kind << "'";
    if (filter.from)
        sql << " AND event_date >= '" << *filter.from << "'";
    if (filter.to)
        sql << " AND event_date < '" << *filter.to << "'";
    if (filter.done)
        sql << " AND done = " << (*filter.done ? 1 : 0);

    sql << " ORDER BY schedule_kind ASC, event_date ASC, day_of_week ASC, start_minute ASC";

    Json::Value items(Json::arrayValue);
    for (const auto& row : m_client->execSqlSync(sql.str()))
        items.append(rowToJson(row));
    return items;
}

std::optional<Json::Value> ScheduleTasksInternalStore::getById(int64_t user_id, int64_t task_id) const
{
    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT id, user_id, title, created_at, created_by, category, schedule_kind, day_of_week,
       event_date, start_minute, end_minute, done, source_insight_id
FROM schedule_task WHERE id = ? AND user_id = ?
)SQL",
        task_id,
        user_id);
    if (rows.empty())
        return std::nullopt;
    return rowToJson(rows[0]);
}

void ScheduleTasksInternalStore::removeBySourceInsight(int64_t user_id, int64_t insight_id) const
{
    m_client->execSqlSync(
        "DELETE FROM schedule_task WHERE user_id = ? AND source_insight_id = ?",
        user_id,
        insight_id);
}

std::optional<Json::Value> ScheduleTasksInternalStore::create(
    const Json::Value& body,
    std::string& error,
    std::string& field) const
{
    if (!body.isMember("userId"))
    {
        error = "userId 가 필요합니다.";
        field = "userId";
        return std::nullopt;
    }
    if (!body.isMember("title") || !body["title"].isString() || trim_copy(body["title"].asString()).empty())
    {
        error = "title 이 필요합니다.";
        field = "title";
        return std::nullopt;
    }
    if (!body.isMember("dayOfWeek") || !body["dayOfWeek"].isString() || !is_valid_day(body["dayOfWeek"].asString()))
    {
        error = "dayOfWeek 가 필요합니다.";
        field = "dayOfWeek";
        return std::nullopt;
    }

    const int64_t user_id = body["userId"].asInt64();
    const std::string title = trim_copy(body["title"].asString());
    const std::string day_of_week = body["dayOfWeek"].asString();
    const std::string schedule_kind = body.isMember("scheduleKind") && body["scheduleKind"].isString()
        ? body["scheduleKind"].asString()
        : "weekly";
    const std::string category = body.isMember("category") && body["category"].isString()
        ? body["category"].asString()
        : "mental";
    const std::string created_by = body.isMember("createdBy") && body["createdBy"].isString()
        ? body["createdBy"].asString()
        : "agent";

    std::optional<std::string> event_date;
    if (body.isMember("eventDate") && !body["eventDate"].isNull() && body["eventDate"].isString())
        event_date = body["eventDate"].asString();

    if (schedule_kind == "once" && !event_date)
    {
        error = "once 일정에는 eventDate 가 필요합니다.";
        field = "eventDate";
        return std::nullopt;
    }
    if (schedule_kind == "weekly" && event_date)
    {
        error = "weekly 일정에는 eventDate 를 넣을 수 없습니다.";
        field = "eventDate";
        return std::nullopt;
    }

    std::optional<int> start_minute;
    std::optional<int> end_minute;
    if (body.isMember("startMinute") && !body["startMinute"].isNull())
        start_minute = body["startMinute"].asInt();
    if (body.isMember("endMinute") && !body["endMinute"].isNull())
        end_minute = body["endMinute"].asInt();

    const auto id = nextId();
    const auto now = formatTimestamp();

    const std::optional<int64_t> source_insight_id =
        body.isMember("sourceInsightId") && !body["sourceInsightId"].isNull()
            ? std::optional<int64_t>(body["sourceInsightId"].asInt64())
            : std::nullopt;

    m_client->execSqlSync(
        R"SQL(
INSERT INTO schedule_task (
    id, user_id, title, created_at, created_by, category, schedule_kind,
    day_of_week, event_date, start_minute, end_minute, done, source_insight_id
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?)
)SQL",
        id,
        user_id,
        title,
        now,
        created_by,
        category,
        schedule_kind,
        day_of_week,
        event_date ? *event_date : std::optional<std::string>{},
        start_minute ? *start_minute : std::optional<int>{},
        end_minute ? *end_minute : std::optional<int>{},
        source_insight_id);

    auto rows = m_client->execSqlSync(
        "SELECT * FROM schedule_task WHERE id = ? AND user_id = ?",
        id,
        user_id);
    if (rows.empty())
    {
        error = "일정을 저장하지 못했습니다.";
        return std::nullopt;
    }
    return rowToJson(rows[0]);
}

std::optional<Json::Value> ScheduleTasksInternalStore::update(
    int64_t user_id,
    int64_t task_id,
    const Json::Value& body,
    std::string& error,
    std::string& field) const
{
    auto rows = m_client->execSqlSync(
        "SELECT id FROM schedule_task WHERE id = ? AND user_id = ?",
        task_id,
        user_id);
    if (rows.empty())
    {
        error = "일정을 찾을 수 없습니다.";
        return std::nullopt;
    }

    std::vector<std::string> sets;
    auto bindText = [&](const char* column, const std::string& value) {
        sets.push_back(std::string(column) + " = '" + value + "'");
    };

    if (body.isMember("title") && body["title"].isString())
        bindText("title", trim_copy(body["title"].asString()));
    if (body.isMember("category") && body["category"].isString())
        bindText("category", body["category"].asString());
    if (body.isMember("dayOfWeek") && body["dayOfWeek"].isString())
    {
        if (!is_valid_day(body["dayOfWeek"].asString()))
        {
            error = "dayOfWeek 값이 올바르지 않습니다.";
            field = "dayOfWeek";
            return std::nullopt;
        }
        bindText("day_of_week", body["dayOfWeek"].asString());
    }
    if (body.isMember("scheduleKind") && body["scheduleKind"].isString())
        bindText("schedule_kind", body["scheduleKind"].asString());
    if (body.isMember("eventDate"))
    {
        if (body["eventDate"].isNull())
            sets.push_back("event_date = NULL");
        else if (body["eventDate"].isString())
            bindText("event_date", body["eventDate"].asString());
    }
    if (body.isMember("startMinute"))
    {
        if (body["startMinute"].isNull())
            sets.push_back("start_minute = NULL");
        else
            sets.push_back("start_minute = " + std::to_string(body["startMinute"].asInt()));
    }
    if (body.isMember("endMinute"))
    {
        if (body["endMinute"].isNull())
            sets.push_back("end_minute = NULL");
        else
            sets.push_back("end_minute = " + std::to_string(body["endMinute"].asInt()));
    }
    if (body.isMember("done"))
        sets.push_back(std::string("done = ") + (body["done"].asBool() ? "1" : "0"));

    if (sets.empty())
    {
        error = "변경할 필드가 없습니다.";
        field = "body";
        return std::nullopt;
    }

    std::ostringstream sql;
    sql << "UPDATE schedule_task SET ";
    for (size_t i = 0; i < sets.size(); ++i)
    {
        if (i > 0)
            sql << ", ";
        sql << sets[i];
    }
    sql << " WHERE id = " << task_id << " AND user_id = " << user_id;

    m_client->execSqlSync(sql.str());

    auto updated = m_client->execSqlSync(
        R"SQL(
SELECT id, user_id, title, created_at, created_by, category, schedule_kind, day_of_week,
       event_date, start_minute, end_minute, done, source_insight_id
FROM schedule_task WHERE id = ? AND user_id = ?
)SQL",
        task_id,
        user_id);
    if (updated.empty())
    {
        error = "일정을 찾을 수 없습니다.";
        return std::nullopt;
    }
    return rowToJson(updated[0]);
}

std::optional<Json::Value> ScheduleTasksInternalStore::remove(
    int64_t user_id,
    int64_t task_id,
    std::string& error) const
{
    auto rows = m_client->execSqlSync(
        "DELETE FROM schedule_task WHERE id = ? AND user_id = ?",
        task_id,
        user_id);
    if (rows.affectedRows() == 0)
    {
        error = "일정을 찾을 수 없습니다.";
        return std::nullopt;
    }

    Json::Value body;
    body["id"] = static_cast<Json::Int64>(task_id);
    return body;
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
