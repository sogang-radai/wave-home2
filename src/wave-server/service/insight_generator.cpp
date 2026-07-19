#include "insight_generator.h"
#include "../db/database.h"

#include <optional>
#include <vector>

#include "../core/json.h"
#include "../core/logger.h"
#include "util/time_util.h"
#include "agent_client.h"
#include "insight_vec_store.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    std::vector<float> parse_embedding(const json& item)
    {
        std::vector<float> out;
        if (!item.contains("embedding") || !item["embedding"].is_array())
            return out;
        out.reserve(item["embedding"].size());
        for (const auto& value : item["embedding"])
        {
            if (value.is_number())
                out.push_back(value.get<float>());
        }
        return out;
    }
}

bool generateAndPersistInsights(
    const db::DbClientPtr& client,
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
    body["embed"] = true;

    AgentInsightJobResult result;
    if (runInsightJobSync(agent_base_url, body, result, out_error) != AgentClientResult::success)
    {
        WLOG_WARN(
            "insight generation job failed (user {}, surface {}, date {}): {}",
            user_id,
            surface,
            date,
            out_error);
        return false;
    }

    try
    {
        InsightVecStore vec_store(client);

        std::vector<int64_t> previous_ids;
        {
            const auto old_rows = client->execSqlSync(
                "SELECT id FROM insight WHERE user_id = ? AND surface = ? AND date = ?",
                user_id,
                surface,
                date);
            previous_ids.reserve(old_rows.size());
            for (const auto& row : old_rows)
                previous_ids.push_back(row["id"].as<int64_t>());
        }
        vec_store.deleteEmbeddings(surface, previous_ids);

        // 문서 규칙: 동일 userId+surface+date 기존 행은 삭제 후 insert.
        client->execSqlSync(
            "DELETE FROM insight WHERE user_id = ? AND surface = ? AND date = ?",
            user_id,
            surface,
            date);

        const auto id_rows = client->execSqlSync("SELECT COALESCE(MAX(id), 0) AS max_id FROM insight");
        int64_t next_id = (id_rows.empty() ? 0 : id_rows[0]["max_id"].as<int64_t>()) + 1;

        const auto now = formatTimestamp();
        size_t embedded_count = 0;
        for (const auto& item : result.items)
        {
            const std::string kind = item.value("kind", std::string("tip"));
            const bool actionable = item.value("actionable", false);

            std::optional<std::string> label = item.contains("label") && item["label"].is_string()
                ? std::optional<std::string>(item["label"].get<std::string>())
                : std::nullopt;
            if (!label || label->empty())
            {
                if (surface == "power")
                    label = "전력";
                else if (surface == "sleep_report")
                    label = "수면";
                else if (surface == "posture_report")
                    label = "자세";
                else if (surface == "weekly_plan")
                    label = "주간계획";
                else if (surface == "dashboard_banner")
                    label = "안내";
            }
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

            const int64_t insight_id = next_id;
            client->execSqlSync(
                R"SQL(
INSERT INTO insight (
    id, user_id, surface, kind, date, label, title, text,
    actionable, action_type, approved, rule_json, schedule_task_json, created_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?)
)SQL",
                insight_id,
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

            const auto embedding = parse_embedding(item);
            if (!embedding.empty())
            {
                vec_store.storeEmbedding(surface, insight_id, embedding);
                ++embedded_count;
            }
            else
            {
                WLOG_WARN(
                    "insight item missing embedding (user {}, surface {}, id {})",
                    user_id,
                    surface,
                    insight_id);
            }

            ++next_id;
        }

        WLOG_INFO(
            "insight generation persisted {} item(s), {} embedded (user {}, surface {}, date {})",
            result.items.size(),
            embedded_count,
            user_id,
            surface,
            date);
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        WLOG_WARN(
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
