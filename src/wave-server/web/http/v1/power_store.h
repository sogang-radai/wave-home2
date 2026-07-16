#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
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
     * whole-house(device_id IS NULL) 일간(24h) power_report 가 없으면: 5m power_energy 를
     * 하루치로 합산해 24h power_energy 행을 upsert하고, 에이전트를 호출해 power_report 를
     * 생성·임베딩 저장한 뒤, 성공하면 이어서 해당 날짜의 인사이트(surface="power")도
     * 생성한다 (sleep_manager.cpp 의 "리포트 생성 직후 인사이트 트리거"와 동일한 지점).
     * 그 날의 5m 데이터가 아예 없으면 아무 것도 하지 않고 false를 반환한다.
     */
    static bool ensure_daily_report(const db::DbClientPtr& client, const std::string& date);

    /**
     * whole-house 1시간(1h) power_report 버전. PowerManager 의 정시(매시 정각) 트리거와
     * GET /power/reports(1h, 캐시 미스 시)에서 모두 사용한다. 인사이트는 트리거하지 않는다
     * (인사이트는 하루 단위 개념이라 매시간 재생성하면 과도하다).
     */
    static bool ensure_hourly_report(const db::DbClientPtr& client, const std::string& hour_start);

private:
    IotStore& m_iot;

    static int step_seconds_for_range(const std::string& range);
    static int point_count_for_range(const std::string& range);
    static std::string format_ago_label(int seconds_ago);

    /** ensure_daily_report/ensure_hourly_report 공용 코어. 성공 시 생성/재사용된 power_report.id. */
    static std::optional<int64_t> generate_report(
        const db::DbClientPtr& client,
        const std::string& period,
        const std::string& period_start,
        const std::string& window_start,
        const std::string& window_end,
        double expected_5m_buckets);

    /** vec_power_report(있으면) 또는 power_report_embedding 폴백에 저장 (sleep_vec_store.cpp 와 동일 패턴). */
    static void store_report_embedding(
        const db::DbClientPtr& client,
        int64_t report_id,
        const std::vector<float>& embedding);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
