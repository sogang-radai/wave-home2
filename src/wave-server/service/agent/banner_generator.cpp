#include "banner_generator.h"

#include <vector>

#include "../../core/json.h"
#include "../../core/logger.h"
#include "util/time_util.h"
#include "agent_client.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    // Top-1 active habit per requested type, in the given type order — e.g. for
    // {"sleep","power","lifestyle"} this returns at most 3 rows, one per category,
    // each the highest-confidence active habit of that type. A type with no active
    // habit is simply absent from the result (never padded/faked).
    json fetch_active_habits(const db::DbClientPtr& client, int64_t user_id, const std::vector<std::string>& types)
    {
        json habits = json::array();
        for (const auto& habit_type : types)
        {
            const auto rows = client->execSqlSync(
                "SELECT habit_type, title, description, confidence FROM user_habit"
                " WHERE user_id = ? AND status = 'active' AND habit_type = ?"
                " ORDER BY confidence DESC, updated_at DESC LIMIT 1",
                user_id,
                habit_type);
            if (rows.empty())
                continue;
            habits.push_back(json{
                {"habitType", rows[0]["habit_type"].as<std::string>()},
                {"title", rows[0]["title"].as<std::string>()},
                {"description", rows[0]["description"].as<std::string>()},
                {"confidence", rows[0]["confidence"].as<double>()},
            });
        }
        return habits;
    }
}

bool generateAndPersistDashboardBanner(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& for_date,
    std::string& out_error)
{
    if (!client)
    {
        out_error = "no db client";
        return false;
    }

    const auto habits = fetch_active_habits(client, user_id, {"sleep", "power", "lifestyle"});
    if (habits.empty())
        return true; // nothing active yet — dashboardDailyMessage's existing fallbacks cover this

    json body;
    body["userId"] = user_id;
    body["date"] = for_date;
    body["surface"] = "dashboard";
    body["habits"] = habits;

    AgentBannerJobResult result;
    if (runBannerJobSync(agent_base_url, body, result, out_error) != AgentClientResult::success)
    {
        WLOG_WARN("dashboard banner job failed (user {}, date {}): {}", user_id, for_date, out_error);
        return false;
    }
    if (result.headline.empty() || result.body.empty())
        return true; // agent had nothing usable — leave prior banner/fallback in place

    try
    {
        const auto now = formatTimestamp();

        client->execSqlSync(
            "DELETE FROM insight WHERE user_id = ? AND surface = 'dashboard_banner' AND kind = 'banner' AND date = ?",
            user_id,
            for_date);

        const auto id_rows = client->execSqlSync("SELECT COALESCE(MAX(id), 0) AS max_id FROM insight");
        const int64_t next_id = (id_rows.empty() ? 0 : id_rows[0]["max_id"].as<int64_t>()) + 1;

        client->execSqlSync(
            R"SQL(
INSERT INTO insight (
    id, user_id, surface, kind, date, label, title, text,
    actionable, approved, created_at
) VALUES (?, ?, 'dashboard_banner', 'banner', ?, '안내', ?, ?, 0, 0, ?)
)SQL",
            next_id, user_id, for_date, result.headline, result.body, now);
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        WLOG_WARN("dashboard banner persist failed (user {}, date {}): {}", user_id, for_date, out_error);
        return false;
    }

    return true;
}

bool generateAndPersistWeeklyPlanBanner(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& period_start,
    std::string& out_error)
{
    if (!client)
    {
        out_error = "no db client";
        return false;
    }

    // Deliberately lifestyle-only: this call never sees sleep-score or power-kWh
    // numbers, so the LLM structurally cannot mix them into the weekly-plan banner
    // the way the old (removed) weekly_plan_graph-sourced text used to.
    const auto habits = fetch_active_habits(client, user_id, {"lifestyle"});
    if (habits.empty())
        return true; // weeklyReport's existing fallbacks cover this

    json body;
    body["userId"] = user_id;
    body["date"] = period_start;
    body["surface"] = "weekly_plan";
    body["habits"] = habits;

    AgentBannerJobResult result;
    if (runBannerJobSync(agent_base_url, body, result, out_error) != AgentClientResult::success)
    {
        WLOG_WARN("weekly plan banner job failed (user {}, period {}): {}", user_id, period_start, out_error);
        return false;
    }
    if (result.headline.empty() || result.body.empty())
        return true;

    try
    {
        const auto now = formatTimestamp();
        client->execSqlSync(
            R"SQL(
INSERT INTO weekly_plan_report (user_id, period_start, headline, report_text, created_at)
VALUES (?, ?, ?, ?, ?)
ON CONFLICT(user_id, period_start) DO UPDATE SET
    headline = excluded.headline, report_text = excluded.report_text, created_at = excluded.created_at
)SQL",
            user_id, period_start, result.headline, result.body, now);
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        WLOG_WARN("weekly plan banner persist failed (user {}, period {}): {}", user_id, period_start, out_error);
        return false;
    }

    return true;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
