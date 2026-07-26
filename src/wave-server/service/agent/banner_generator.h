#pragma once

#include <cstdint>
#include <string>

#include "../../db/database.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

/**
 * Synthesizes a short, natural-language banner (headline + body) from the
 * user's active habits via a single inline LLM call (no gather/tool-loop —
 * same shape as habit_generator: the habits are already known, only wording
 * is the LLM's job), then persists it. Runs once nightly from
 * UserModelManager's rollover, not on every page load, so the actual banner
 * read paths (InsightsStore::dashboardDailyMessage / WeeklyPlanStore::weeklyReport)
 * stay a fast SQL SELECT with no LLM latency in the request path.
 *
 * Dashboard: combines the user's top active sleep/power/lifestyle habits into
 * one banner, persisted to insight(surface='dashboard_banner', kind='banner').
 *
 * Weekly plan: intentionally scoped to lifestyle (routine/goal) habits ONLY —
 * the LLM is never given sleep-score or power-kWh numbers for this one, so it
 * structurally cannot produce the old verbose stat-dump text; persisted to
 * weekly_plan_report(headline, report_text).
 */
bool generateAndPersistDashboardBanner(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& for_date,
    std::string& out_error);

bool generateAndPersistWeeklyPlanBanner(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& period_start,
    std::string& out_error);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
