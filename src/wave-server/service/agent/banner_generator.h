#pragma once

#include <cstdint>
#include <string>

#include "../../db/database.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

/**
 * Runs once nightly from UserModelManager's rollover, not on every page
 * load, so the actual banner read paths
 * (InsightsStore::dashboardDailyMessage / WeeklyPlanStore::weeklyReport)
 * stay a fast SQL SELECT with no LLM latency in the request path.
 *
 * Dashboard: a deterministic weekly sleep+power+appliance-control data
 * summary (no habits involved) — backend computes the real numbers,
 * the LLM only wording them into a few sentences via a single inline LLM
 * call (no gather/tool-loop). Persisted to
 * insight(surface='dashboard_banner', kind='banner').
 *
 * Weekly plan: combines the user's top active sleep/power/lifestyle habits
 * (same all-types combination the dashboard banner used to show) into one
 * banner via a single inline LLM call (habits are already known server-side,
 * only wording is the LLM's job); persisted to
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
