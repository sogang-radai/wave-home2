#include "goals_store.h"

#include <sstream>

#include "../../../core/time_util.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

namespace
{
    Json::Value parseJsonColumn(const drogon::orm::Field& field)
    {
        if (field.isNull())
            return Json::nullValue;
        Json::Value parsed;
        Json::CharReaderBuilder builder;
        std::string errors;
        const auto text = field.as<std::string>();
        std::istringstream stream(text);
        if (Json::parseFromStream(builder, stream, &parsed, &errors))
            return parsed;
        return Json::nullValue;
    }
}

GoalsStore::GoalsStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

int64_t GoalsStore::nextGoalId() const
{
    const auto rows = m_client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM goal");
    return rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();
}

Json::Value GoalsStore::rowToJson(const drogon::orm::Row& row) const
{
    Json::Value item;
    item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
    item["title"] = row["title"].as<std::string>();
    item["category"] = row["category"].as<std::string>();
    item["status"] = row["status"].as<std::string>();
    item["createdAt"] = row["created_at"].as<std::string>();
    item["updatedAt"] = row["updated_at"].as<std::string>();
    return item;
}

Json::Value GoalsStore::recommendationRowToJson(const drogon::orm::Row& row) const
{
    Json::Value item;
    item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
    item["goalId"] = static_cast<Json::Int64>(row["goal_id"].as<int64_t>());
    item["kind"] = row["kind"].as<std::string>();
    item["title"] = row["title"].as<std::string>();
    item["text"] = row["text"].as<std::string>();
    item["actionable"] = row["actionable"].as<int>() != 0;
    if (row["action_type"].isNull())
        item["actionType"] = Json::nullValue;
    else
        item["actionType"] = row["action_type"].as<std::string>();
    item["approved"] = row["approved"].as<int>() != 0;
    item["ruleJson"] = parseJsonColumn(row["rule_json"]);
    item["scheduleTaskJson"] = parseJsonColumn(row["schedule_task_json"]);
    return item;
}

Json::Value GoalsStore::list(int64_t user_id, const std::optional<std::string>& status) const
{
    std::ostringstream sql;
    sql << "SELECT id, title, category, status, created_at, updated_at FROM goal WHERE user_id = " << user_id;
    if (status)
        sql << " AND status = '" << *status << "'";
    sql << " ORDER BY created_at DESC, id DESC";

    Json::Value items(Json::arrayValue);
    for (const auto& row : m_client->execSqlSync(sql.str()))
        items.append(rowToJson(row));
    return items;
}

Json::Value GoalsStore::getById(int64_t user_id, int64_t goal_id) const
{
    const auto rows = m_client->execSqlSync(
        "SELECT id, title, category, status, created_at, updated_at FROM goal WHERE id = ? AND user_id = ? LIMIT 1",
        goal_id,
        user_id);
    if (rows.empty())
        return Json::nullValue;
    return rowToJson(rows[0]);
}

void GoalsStore::archiveActiveGoals(int64_t user_id) const
{
    m_client->execSqlSync(
        "UPDATE goal SET status = 'archived', updated_at = ? WHERE user_id = ? AND status = 'active'",
        formatTimestamp(),
        user_id);
}

std::optional<Json::Value> GoalsStore::create(
    int64_t user_id,
    const std::string& title,
    const std::string& category,
    std::string& error,
    std::string& field) const
{
    if (title.empty())
    {
        error = "title 이 필요합니다.";
        field = "title";
        return std::nullopt;
    }
    static const char* kCategories[] = {"sleep", "posture", "mental", "life", "diet"};
    bool valid_category = false;
    for (const char* c : kCategories)
    {
        if (category == c)
        {
            valid_category = true;
            break;
        }
    }
    if (!valid_category)
    {
        error = "category 값이 올바르지 않습니다.";
        field = "category";
        return std::nullopt;
    }

    const auto id = nextGoalId();
    const auto now = formatTimestamp();
    m_client->execSqlSync(
        R"SQL(
INSERT INTO goal (id, user_id, title, category, status, created_at, updated_at)
VALUES (?, ?, ?, ?, 'active', ?, ?)
)SQL",
        id,
        user_id,
        title,
        category,
        now,
        now);

    return getById(user_id, id);
}

std::optional<Json::Value> GoalsStore::updateStatus(
    int64_t user_id,
    int64_t goal_id,
    const std::string& status,
    std::string& error) const
{
    if (status != "active" && status != "archived" && status != "completed")
    {
        error = "status 값이 올바르지 않습니다.";
        return std::nullopt;
    }

    const auto rows = m_client->execSqlSync(
        "SELECT id FROM goal WHERE id = ? AND user_id = ?",
        goal_id,
        user_id);
    if (rows.empty())
    {
        error = "목표를 찾을 수 없습니다.";
        return std::nullopt;
    }

    m_client->execSqlSync(
        "UPDATE goal SET status = ?, updated_at = ? WHERE id = ? AND user_id = ?",
        status,
        formatTimestamp(),
        goal_id,
        user_id);

    return getById(user_id, goal_id);
}

Json::Value GoalsStore::getRecommendation(int64_t user_id, int64_t goal_id, int64_t recommendation_id) const
{
    const auto rows = m_client->execSqlSync(
        R"SQL(
SELECT id, goal_id, kind, title, text, actionable, action_type, approved, rule_json, schedule_task_json
FROM goal_recommendation WHERE id = ? AND goal_id = ? AND user_id = ? LIMIT 1
)SQL",
        recommendation_id,
        goal_id,
        user_id);
    if (rows.empty())
        return Json::nullValue;
    return recommendationRowToJson(rows[0]);
}

bool GoalsStore::markRecommendationApplied(
    int64_t recommendation_id,
    const std::optional<Json::Value>& rule_json_override,
    const std::optional<Json::Value>& schedule_task_json_override) const
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";

    // 실제로는 automation_rule 또는 schedule_task 둘 중 하나만 채워진다(한 추천이 두 액션
    // 타입을 동시에 갖지 않음) — 그래서 조합을 일반화하지 않고 세 경우만 분기한다.
    if (rule_json_override)
    {
        const auto serialized = Json::writeString(builder, *rule_json_override);
        const auto rows = m_client->execSqlSync(
            "UPDATE goal_recommendation SET approved = 1, rule_json = ? WHERE id = ?",
            serialized,
            recommendation_id);
        return rows.affectedRows() > 0;
    }
    if (schedule_task_json_override)
    {
        const auto serialized = Json::writeString(builder, *schedule_task_json_override);
        const auto rows = m_client->execSqlSync(
            "UPDATE goal_recommendation SET approved = 1, schedule_task_json = ? WHERE id = ?",
            serialized,
            recommendation_id);
        return rows.affectedRows() > 0;
    }

    const auto rows = m_client->execSqlSync(
        "UPDATE goal_recommendation SET approved = 1 WHERE id = ?",
        recommendation_id);
    return rows.affectedRows() > 0;
}

bool GoalsStore::markRecommendationCanceled(int64_t recommendation_id) const
{
    const auto rows = m_client->execSqlSync(
        "UPDATE goal_recommendation SET approved = 0 WHERE id = ?",
        recommendation_id);
    return rows.affectedRows() > 0;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
