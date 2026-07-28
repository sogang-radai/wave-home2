#include "weekly_plan_store.h"
#include "../../../db/database.h"

#include "insights_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

namespace
{
    bool tableExists(db::DbClientPtr client, const char* name)
    {
        const auto rows = client->execSqlSync(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
            name);
        return !rows.empty();
    }
}

WeeklyPlanStore::WeeklyPlanStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

Json::Value WeeklyPlanStore::weeklyReport(int64_t user_id, const std::string& period_start) const
{
    // Primary: the nightly LLM-synthesized weekly banner (banner_generator.cpp),
    // combining the user's top active sleep/power/lifestyle habits into one
    // narrative — this is the same all-types habit banner the dashboard used
    // to show before it moved to a deterministic weekly-stats summary.
    if (tableExists(m_client, "weekly_plan_report"))
    {
        const auto ref_date = period_start.empty() ? InsightsStore::reference_date(m_client) : period_start;
        const auto rows = m_client->execSqlSync(
            "SELECT headline, report_text FROM weekly_plan_report"
            " WHERE user_id = ? AND period_start <= ?"
            " ORDER BY period_start DESC LIMIT 1",
            user_id,
            ref_date);
        if (!rows.empty())
        {
            Json::Value out;
            out["headline"] = rows[0]["headline"].as<std::string>();
            const auto& body_field = rows[0]["report_text"];
            out["body"] = body_field.isNull() ? "" : body_field.as<std::string>();
            return out;
        }
    }

    // Fallback: synthesis hasn't run yet (or the agent was unreachable) — show
    // the single best active habit of any type directly rather than nothing.
    return InsightsStore::bestActiveHabitBanner(m_client, user_id);
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
