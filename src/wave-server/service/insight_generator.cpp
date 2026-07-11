#include "insight_generator.h"

#include <optional>

#include "../core/json.h"
#include "../core/logger.h"
#include "../core/time_util.h"
#include "agent_client.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

bool generateAndPersistInsights(
    const drogon::orm::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& surface,
    const std::string& date,
    std::string& out_error)
{
    json body;
    body["userId"] = user_id;
    body["surface"] = surface;
    body["date"] = date;
    body["embed"] = false;

    AgentInsightJobResult result;
    if (runInsightJobSync(agent_base_url, body, result, out_error) != AgentClientResult::success)
    {
        LOG_WARN(
            "insight generation job failed (user {}, surface {}, date {}): {}",
            user_id,
            surface,
            date,
            out_error);
        return false;
    }

    try
    {
        // 문서 규칙: 동일 userId+surface+date 기존 행은 삭제 후 insert.
        client->execSqlSync(
            "DELETE FROM insight WHERE user_id = ? AND surface = ? AND date = ?",
            user_id,
            surface,
            date);

        const auto id_rows = client->execSqlSync("SELECT COALESCE(MAX(id), 0) AS max_id FROM insight");
        int64_t next_id = (id_rows.empty() ? 0 : id_rows[0]["max_id"].as<int64_t>()) + 1;

        const auto now = formatTimestamp();
        for (const auto& item : result.items)
        {
            const std::string kind = item.value("kind", std::string("tip"));
            const bool actionable = item.value("actionable", false);

            const std::optional<std::string> label = item.contains("label") && item["label"].is_string()
                ? std::optional<std::string>(item["label"].get<std::string>())
                : std::nullopt;
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
INSERT INTO insight (
    id, user_id, surface, kind, date, label, title, text,
    actionable, action_type, approved, rule_json, schedule_task_json, created_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?)
)SQL",
                next_id,
                user_id,
                surface,
                kind,
                item.value("date", date),
                label,
                item.value("title", std::string()),
                item.value("text", std::string()),
                actionable ? 1 : 0,
                action_type,
                rule_json,
                schedule_task_json,
                now);
            ++next_id;
        }

        LOG_INFO(
            "insight generation persisted {} item(s) (user {}, surface {}, date {})",
            result.items.size(),
            user_id,
            surface,
            date);
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        LOG_WARN(
            "insight persist failed (user {}, surface {}, date {}): {}",
            user_id,
            surface,
            date,
            out_error);
        return false;
    }

    return true;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
