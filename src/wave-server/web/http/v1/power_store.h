#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../../db/database.h"
#include <json/json.h>

#include "../../../core/json.h"
#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

SERVICE_NAMESPACE_BEGIN
struct AgentJob;
SERVICE_NAMESPACE_END

namespace web {
namespace v1 {

class IotStore;

class PowerStore
{
public:
    explicit PowerStore(IotStore& iot);

    Json::Value listPlugs();
    Json::Value comboTrend(const std::string& device_id, const std::string& range, const std::string& metric);

    static Json::Value period_trend(
        db::DbClientPtr client,
        const std::string& device_external_id,
        const std::string& ui_period,
        const std::string& ref_date_hint);

    static Json::Value query_report(
        db::DbClientPtr client,
        const std::string& device_external_id,
        const std::string& ui_period,
        const std::string& period_start_hint);

    /**
     * Enqueue (or wait for) whole-house 24h report. Insights are enqueued only when the
     * report is newly created.
     */
    static bool ensure_daily_report(const db::DbClientPtr& client, const std::string& date);

    /** Enqueue (or wait for) whole-house 1h report. Does not trigger insights. */
    static bool ensure_hourly_report(const db::DbClientPtr& client, const std::string& hour_start);

    /** Enqueue (or wait for) whole-house 1w report — unlike 24h/1h, weekly reports are
     *  never auto-created on a query_report() cache miss (see query_report's comment);
     *  this is the debug/admin path (POST /internal/v1/power/reports/weekly) used to
     *  force one for testing. */
    static bool ensure_weekly_report(const db::DbClientPtr& client, const std::string& period_start_date);

    /** Fire-and-forget schedule helpers (no wait). */
    static void enqueue_hourly_report(const std::string& hour_start);
    static void enqueue_daily_report(const std::string& date);
    static void enqueue_weekly_report(const std::string& period_start_date);
    static void enqueue_monthly_report(const std::string& period_start_date);
    static void enqueue_yearly_report(const std::string& period_start_date);
    static void enqueue_power_insights_for_date(const std::string& date);

    /** AgentJobQueue worker entry. */
    static void run_queued_report(const service::AgentJob& job);

private:
    IotStore& m_iot;

    static int step_seconds_for_range(const std::string& range);
    static int point_count_for_range(const std::string& range);
    static std::string format_ago_label(int seconds_ago);

    static bool enqueue_report_job(
        const std::string& period,
        const std::string& period_start,
        const std::string& window_start,
        const std::string& window_end,
        double expected_5m_buckets,
        bool wait);

    /** Core: upsert energy + call agent + persist report. Returns report id. */
    static std::optional<int64_t> generate_report(
        const db::DbClientPtr& client,
        const std::string& period,
        const std::string& period_start,
        const std::string& window_start,
        const std::string& window_end,
        double expected_5m_buckets);

    static void store_report_embedding(
        const db::DbClientPtr& client,
        int64_t report_id,
        const std::vector<float>& embedding);

    static json build_children(
        const db::DbClientPtr& client,
        const std::string& period,
        const std::string& window_start,
        const std::string& window_end);

    static json build_by_device(
        const db::DbClientPtr& client,
        const std::string& window_start,
        const std::string& window_end);

    static std::optional<double> previous_window_energy(
        const db::DbClientPtr& client,
        const std::string& period,
        const std::string& period_start,
        const std::string& window_start,
        const std::string& window_end);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
