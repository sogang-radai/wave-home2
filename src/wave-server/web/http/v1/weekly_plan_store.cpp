#include "weekly_plan_store.h"

#include "insights_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

namespace
{
    bool tableExists(drogon::orm::DbClientPtr client, const char* name)
    {
        const auto rows = client->execSqlSync(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
            name);
        return !rows.empty();
    }
}

WeeklyPlanStore::WeeklyPlanStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

Json::Value WeeklyPlanStore::weeklyReport(int64_t user_id, const std::string& period_start) const
{
    if (!tableExists(m_client, "weekly_plan_report"))
        return Json::nullValue;

    const auto ref_date = period_start.empty() ? InsightsStore::referenceDate(m_client) : period_start;
    const auto rows = m_client->execSqlSync(
        "SELECT headline, report_text FROM weekly_plan_report"
        " WHERE user_id = ? AND period_start <= ?"
        " ORDER BY period_start DESC LIMIT 1",
        user_id,
        ref_date);
    if (rows.empty())
        return Json::nullValue;

    Json::Value out;
    out["headline"] = rows[0]["headline"].as<std::string>();
    const auto& body_field = rows[0]["report_text"];
    out["body"] = body_field.isNull() ? "" : body_field.as<std::string>();
    return out;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
