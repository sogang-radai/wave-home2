#include "goal_coaching_generator.h"

#include <optional>
#include <sstream>

#include "../core/json.h"
#include "../core/logger.h"
#include "../core/time_util.h"
#include "agent_client.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

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

    Json::Value recommendationRowToJson(const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
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
}

std::optional<Json::Value> readCachedGoalCoaching(
    const drogon::orm::DbClientPtr& client,
    int64_t goal_id,
    const std::string& date)
{
    const auto report_rows = client->execSqlSync(
        "SELECT past_summary_text, projection_text, projected_metrics_json"
        " FROM goal_coaching_report WHERE goal_id = ? AND period_start = ?",
        goal_id,
        date);
    if (report_rows.empty())
        return std::nullopt;

    Json::Value out;
    out["periodStart"] = date;
    out["pastSummary"] = report_rows[0]["past_summary_text"].as<std::string>();
    out["projection"] = report_rows[0]["projection_text"].as<std::string>();
    out["projectedMetrics"] = parseJsonColumn(report_rows[0]["projected_metrics_json"]);

    Json::Value recommendations(Json::arrayValue);
    const auto rec_rows = client->execSqlSync(
        R"SQL(
SELECT id, kind, title, text, actionable, action_type, approved, rule_json, schedule_task_json
FROM goal_recommendation WHERE goal_id = ? AND date = ? ORDER BY id
)SQL",
        goal_id,
        date);
    for (const auto& row : rec_rows)
        recommendations.append(recommendationRowToJson(row));
    out["recommendations"] = recommendations;

    return out;
}

std::optional<Json::Value> generateGoalCoaching(
    const drogon::orm::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    int64_t goal_id,
    const std::string& goal_title,
    const std::string& category,
    const std::string& date,
    std::string& out_error)
{
    json body;
    body["userId"] = user_id;
    body["goalId"] = goal_id;
    body["goalTitle"] = goal_title;
    body["category"] = category;
    body["periodStart"] = date;
    body["embed"] = false;

    AgentGoalCoachingJobResult agent_result;
    if (runGoalCoachingJobSync(agent_base_url, body, agent_result, out_error) != AgentClientResult::success)
    {
        LOG_WARN("goal coaching generation failed (goal {}): {}", goal_id, out_error);
        return std::nullopt;
    }

    const json& content = agent_result.content;
    const std::string past_summary = content.value("pastSummary", std::string());
    const std::string projection = content.value("projection", std::string());
    const json projected_metrics = content.value("projectedMetrics", json::object());
    const json items = content.value("items", json::array());

    try
    {
        client->execSqlSync(
            R"SQL(
INSERT INTO goal_coaching_report (
    goal_id, user_id, period_start, past_summary_text, projection_text, projected_metrics_json, created_at
) VALUES (?, ?, ?, ?, ?, ?, ?)
ON CONFLICT DO UPDATE SET
    past_summary_text = excluded.past_summary_text,
    projection_text = excluded.projection_text,
    projected_metrics_json = excluded.projected_metrics_json,
    created_at = excluded.created_at
)SQL",
            goal_id,
            user_id,
            date,
            past_summary,
            projection,
            projected_metrics.dump(),
            formatTimestamp());

        // 문서 규칙(insight_generator.cpp와 동일한 관례): 동일 goal_id+date 기존 행은 삭제 후 insert.
        client->execSqlSync("DELETE FROM goal_recommendation WHERE goal_id = ? AND date = ?", goal_id, date);

        const auto id_rows = client->execSqlSync("SELECT COALESCE(MAX(id), 0) AS max_id FROM goal_recommendation");
        int64_t next_id = (id_rows.empty() ? 0 : id_rows[0]["max_id"].as<int64_t>()) + 1;

        for (const auto& item : items)
        {
            const std::string kind = item.value("kind", std::string("tip"));
            const bool actionable = item.value("actionable", false);

            const std::optional<std::string> action_type =
                item.contains("actionType") && item["actionType"].is_string()
                    ? std::optional<std::string>(item["actionType"].get<std::string>())
                    : std::nullopt;
            const std::optional<std::string> rule_json =
                item.contains("ruleJson") && !item["ruleJson"].is_null()
                    ? std::optional<std::string>(item["ruleJson"].dump())
                    : std::nullopt;
            const std::optional<std::string> schedule_task_json =
                item.contains("scheduleTaskJson") && !item["scheduleTaskJson"].is_null()
                    ? std::optional<std::string>(item["scheduleTaskJson"].dump())
                    : std::nullopt;

            client->execSqlSync(
                R"SQL(
INSERT INTO goal_recommendation (
    id, goal_id, user_id, date, kind, title, text,
    actionable, action_type, approved, rule_json, schedule_task_json, created_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?)
)SQL",
                next_id,
                goal_id,
                user_id,
                date,
                kind,
                item.value("title", std::string()),
                item.value("text", std::string()),
                actionable ? 1 : 0,
                action_type,
                rule_json,
                schedule_task_json,
                formatTimestamp());
            ++next_id;
        }

        LOG_INFO("goal coaching persisted (goal {}, {} recommendation(s))", goal_id, items.size());
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        LOG_WARN("goal_coaching persist failed (goal {}): {}", goal_id, out_error);
        return std::nullopt;
    }

    return readCachedGoalCoaching(client, goal_id, date);
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
