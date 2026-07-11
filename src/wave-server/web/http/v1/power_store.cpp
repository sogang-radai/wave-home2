#include "power_store.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../core/logger.h"
#include "../../../core/time_util.h"
#include "../../../service/agent_client.h"
#include "../../../service/insight_generator.h"
#include "../../../service/power_manager.h"
#include "../../../device/device_wire_id.hpp"
#include "insights_store.h"
#include "iot_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {
namespace
{
    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    const dev::DeviceManifestEntry* findManifestEntry(
        const dev::DeviceManager& devices,
        const std::string& external_id)
    {
        for (const auto& entry : devices.manifestEntries())
        {
            if (entry.config.value("id", "") == external_id)
                return &entry;
        }
        return nullptr;
    }

    std::string roomNameForEntry(const dev::DeviceManifestEntry& entry, const dev::DeviceManager& devices)
    {
        const auto room_id_text = entry.config.value("room_id", "");
        if (room_id_text.empty())
            return "미지정";

        const auto room_id = dev::parseRoomID(room_id_text);
        if (const auto* room = devices.findRoom(room_id))
            return room->name;
        return "미지정";
    }
}

PowerStore::PowerStore(IotStore& iot) :
    m_iot(iot)
{
}

int PowerStore::stepSecondsForRange(const std::string& range)
{
    if (range == "min10")
        return 10;
    if (range == "min30")
        return 30;
    if (range == "hour")
        return 60;
    return 1;
}

int PowerStore::pointCountForRange(const std::string& range)
{
    (void)range;
    return 60;
}

std::string PowerStore::formatAgoLabel(int seconds_ago)
{
    if (seconds_ago <= 0)
        return "지금";
    if (seconds_ago < 60)
        return "-" + std::to_string(seconds_ago) + "s";
    return "-" + std::to_string((seconds_ago + 59) / 60) + "분";
}

Json::Value PowerStore::listPlugs()
{
    auto& power = ws::service::PowerManager::get();
    const auto& devices = AppState::get().deviceManager;

    double total_w = 0;
    double total_v = 0;
    double total_a_ma = 0;
    int switch_on_count = 0;
    int connected_count = 0;
    int plug_count = 0;

    Json::Value aggregate;
    aggregate["id"] = "all";
    aggregate["name"] = "전체";
    aggregate["room"] = "집합";
    aggregate["summary"] = "계측 플러그 합산";
    aggregate["connected"] = true;
    aggregate["connectionStatus"] = "online";
    aggregate["powerW"] = 0.0;
    aggregate["voltageV"] = 0.0;
    aggregate["currentMa"] = 0;
    aggregate["switchOn"] = false;
    aggregate["hourlyCostWon"] = 0.0;
    aggregate["trend"] = Json::Value(Json::objectValue);

    Json::Value body(Json::arrayValue);
    body.append(aggregate);

    for (const auto& plug_id : m_iot.listPlugIds())
    {
        const auto* entry = findManifestEntry(devices, plug_id);
        const auto reading = power.getReading(plug_id);

        std::string room_name = "미지정";
        std::string name = entry ? entry->config.value("name", plug_id) : plug_id;
        std::string summary = entry ? entry->config.value("description", "") : "";
        if (entry)
            room_name = roomNameForEntry(*entry, devices);

        std::string connection_status = "offline";
        if (entry)
            connection_status = m_iot.connectionStatusForEntry(*entry);
        else if (reading && reading->connected)
            connection_status = "online";

        const bool online = connection_status == "online";

        Json::Value plug;
        plug["id"] = plug_id;
        plug["name"] = name;
        plug["room"] = room_name;
        plug["summary"] = summary;
        plug["connected"] = online;
        plug["connectionStatus"] = connection_status;
        plug["switchOn"] = online && reading && reading->switch_on;
        plug["trend"] = Json::Value(Json::objectValue);

        if (online && reading)
        {
            plug["powerW"] = reading->power_w;
            plug["voltageV"] = reading->voltage_v;
            plug["currentMa"] = static_cast<Json::Int64>(static_cast<int64_t>(std::lround(reading->current_ma)));
            plug["hourlyCostWon"] = std::round(reading->power_w * 0.25 * 10.0) / 10.0;
            plug["sampledAtMs"] = static_cast<Json::Int64>(reading->ts_ms);

            total_w += reading->power_w;
            total_v += reading->voltage_v;
            total_a_ma += reading->current_ma;
            switch_on_count += reading->switch_on ? 1 : 0;
            ++connected_count;
        }
        else
        {
            plug["powerW"] = Json::Value();
            plug["voltageV"] = Json::Value();
            plug["currentMa"] = Json::Value();
            plug["hourlyCostWon"] = Json::Value();
        }

        body.append(plug);
        ++plug_count;
    }

    if (body.size() > 0 && body[0].isObject())
    {
        body[0]["powerW"] = total_w;
        body[0]["voltageV"] = connected_count > 0 ? total_v / connected_count : 0.0;
        body[0]["currentMa"] = static_cast<Json::Int64>(static_cast<int64_t>(std::lround(total_a_ma)));
        body[0]["switchOn"] = switch_on_count > 0;
        body[0]["hourlyCostWon"] = std::round(total_w * 0.25 * 10.0) / 10.0;
        body[0]["connected"] = connected_count > 0;
        body[0]["connectionStatus"] = connected_count > 0 ? "online" : "offline";
    }

    return body;
}

Json::Value PowerStore::comboTrend(const std::string& device_id, const std::string& range, const std::string& metric)
{
    (void)metric;
    auto& power = ws::service::PowerManager::get();

    const int step_seconds = stepSecondsForRange(range);
    const int points = pointCountForRange(range);
    const int64_t now = nowMs();

    const auto history = device_id == "all" ? power.getMergedHistory() : power.getHistory(device_id);

    Json::Value series(Json::arrayValue);
    for (int i = 0; i < points; ++i)
    {
        const int seconds_ago = (points - 1 - i) * step_seconds;
        const int64_t target_ts = now - static_cast<int64_t>(seconds_ago) * 1000;

        ws::service::PowerSample chosen{};
        bool found = false;
        for (auto it = history.rbegin(); it != history.rend(); ++it)
        {
            if (it->ts_ms <= target_ts)
            {
                chosen = *it;
                found = true;
                break;
            }
        }
        if (!found && !history.empty())
            chosen = history.back();

        Json::Value point;
        point["label"] = formatAgoLabel(seconds_ago);
        point["value"] = found || !history.empty() ? chosen.w : 0.0;
        point["wh"] = point["value"].asDouble() * (step_seconds / 3600.0);
        point["v"] = found || !history.empty() ? chosen.v : 0.0;
        point["a"] = found || !history.empty() ? chosen.a : 0.0;
        series.append(point);
    }
    return series;
}

Json::Value PowerStore::periodTrend(
    drogon::orm::DbClientPtr client,
    const std::string& device_external_id,
    const std::string& ui_period,
    const std::string& ref_date_hint)
{
    Json::Value series(Json::arrayValue);

    const auto table_rows = client->execSqlSync(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='power_energy' LIMIT 1");
    if (table_rows.empty())
        return series;

    const std::string ref_date = ref_date_hint.empty()
        ? InsightsStore::referenceDate(client)
        : ref_date_hint.substr(0, 10);

    std::string device_clause;
    if (!device_external_id.empty() && device_external_id != "all")
    {
        const auto db_id = dev::dbIdForWireId(client, device_external_id);
        if (!db_id)
            return series;
        device_clause = " AND device_id = " + std::to_string(*db_id);
    }
    else
    {
        device_clause = " AND device_id IS NULL";
    }

    auto appendPoint = [&](const std::string& label, double wh, bool unit_kwh = false) {
        Json::Value point;
        point["label"] = label;
        point["wh"] = unit_kwh ? wh : std::round(wh * 100.0) / 100.0;
        if (unit_kwh)
            point["unitKwh"] = true;
        series.append(point);
    };

    if (ui_period == "day")
    {
        const auto rows = client->execSqlSync(
            "SELECT time_start, energy_wh FROM power_energy WHERE granularity='1h'"
            " AND time_start LIKE ?" + device_clause + " ORDER BY time_start",
            ref_date + "%");
        for (const auto& row : rows)
        {
            const auto ts = row["time_start"].as<std::string>();
            if (ts.size() < 13)
                continue;
            const int hour = std::stoi(ts.substr(11, 2));
            appendPoint(std::to_string(hour) + "시", row["energy_wh"].as<double>());
        }
        return series;
    }

    if (ui_period == "week")
    {
        const auto rows = client->execSqlSync(
            "SELECT time_start, energy_wh FROM power_energy WHERE granularity='24h'"
            " AND date(time_start) BETWEEN date(?, '-6 days') AND date(?)" + device_clause
            + " ORDER BY time_start",
            ref_date,
            ref_date);
        static const char* weekdays[] = {"일", "월", "화", "수", "목", "금", "토"};
        for (const auto& row : rows)
        {
            const auto ts = row["time_start"].as<std::string>();
            const auto date_rows = client->execSqlSync("SELECT strftime('%w', ?)", ts);
            int wday = date_rows.empty() ? 0 : date_rows[0][0].as<int>();
            appendPoint(weekdays[wday], row["energy_wh"].as<double>());
        }
        return series;
    }

    if (ui_period == "month")
    {
        const std::string month_prefix = ref_date.substr(0, 7);
        const auto rows = client->execSqlSync(
            "SELECT time_start, energy_wh FROM power_energy WHERE granularity='24h'"
            " AND time_start LIKE ?" + device_clause + " ORDER BY time_start",
            month_prefix + "%");
        for (const auto& row : rows)
        {
            const auto ts = row["time_start"].as<std::string>();
            if (ts.size() < 10)
                continue;
            const int day = std::stoi(ts.substr(8, 2));
            appendPoint(std::to_string(day), row["energy_wh"].as<double>());
        }
        return series;
    }

    if (ui_period == "year")
    {
        const std::string year_prefix = ref_date.substr(0, 4);
        std::map<int, double> monthly;
        const auto rows = client->execSqlSync(
            "SELECT substr(time_start, 6, 2) AS month_key, SUM(energy_wh) AS total"
            " FROM power_energy WHERE granularity='24h' AND time_start LIKE ?" + device_clause
            + " GROUP BY substr(time_start, 1, 7) ORDER BY month_key",
            year_prefix + "-%");
        for (const auto& row : rows)
        {
            const int month = std::stoi(row["month_key"].as<std::string>());
            monthly[month] = row["total"].as<double>();
        }
        for (int month = 1; month <= 12; ++month)
        {
            const double wh = monthly.count(month) ? monthly[month] : 0.0;
            appendPoint(std::to_string(month) + "월", wh / 1000.0, true);
        }
        return series;
    }

    return series;
}

void PowerStore::storeReportEmbedding(
    const drogon::orm::DbClientPtr& client,
    int64_t report_id,
    const std::vector<float>& embedding)
{
    if (!client || embedding.empty())
        return;

    auto table_exists = [&](const char* name) {
        try
        {
            return !client->execSqlSync(
                        "SELECT 1 FROM sqlite_master WHERE type IN ('table','virtual table') AND name = ? LIMIT 1",
                        name)
                        .empty();
        }
        catch (const std::exception&)
        {
            return false;
        }
    };

    // std::string 파라미터는 sqlite3_bind_text 경로를 타서 임베딩 바이트 중간의 0x00 에서
    // 잘릴 수 있다 (실측 확인됨) — SqlBinder.h 의 std::vector<char> 전용 오버로드
    // (sqlite3_bind_blob 경로)를 써야 바이너리가 안전하게 저장된다.
    std::vector<char> blob(embedding.size() * sizeof(float));
    if (!embedding.empty())
        std::memcpy(blob.data(), embedding.data(), blob.size());

    try
    {
        if (table_exists("vec_power_report"))
        {
            client->execSqlSync("DELETE FROM vec_power_report WHERE report_id = ?", report_id);
            client->execSqlSync(
                "INSERT INTO vec_power_report (report_id, embedding) VALUES (?, ?)", report_id, blob);
            return;
        }

        if (table_exists("power_report_embedding"))
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO power_report_embedding (report_id, dim, embedding_blob, updated_at)
VALUES (?, ?, ?, datetime('now'))
ON CONFLICT(report_id) DO UPDATE SET
    dim = excluded.dim,
    embedding_blob = excluded.embedding_blob,
    updated_at = excluded.updated_at
)SQL",
                report_id,
                static_cast<int64_t>(embedding.size()),
                blob);
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("power report embedding store failed: {}", e.what());
    }
}

std::optional<int64_t> PowerStore::generateReport(
    const drogon::orm::DbClientPtr& client,
    const std::string& period,
    const std::string& period_start,
    const std::string& window_start,
    const std::string& window_end,
    double expected_5m_buckets)
{
    const auto existing = client->execSqlSync(
        "SELECT id FROM power_report WHERE period = ? AND period_start = ? AND device_id IS NULL",
        period,
        period_start);
    if (!existing.empty())
        return existing[0]["id"].as<int64_t>();

    const auto agg_rows = client->execSqlSync(
        R"SQL(
SELECT COALESCE(SUM(energy_wh), 0) AS energy_wh, COALESCE(SUM(sample_count), 0) AS samples, COUNT(*) AS buckets
FROM power_energy
WHERE device_id IS NULL AND granularity = '5m' AND time_start >= ? AND time_start < ?
)SQL",
        window_start,
        window_end);
    if (agg_rows.empty() || agg_rows[0]["buckets"].as<int64_t>() == 0)
        return std::nullopt; // 그 구간 5m 데이터가 아예 없음 — 리포트를 만들 근거가 없다.

    const double energy_wh = std::round(agg_rows[0]["energy_wh"].as<double>() * 10000.0) / 10000.0;
    const int64_t sample_count = agg_rows[0]["samples"].as<int64_t>();
    const int64_t bucket_count = agg_rows[0]["buckets"].as<int64_t>();
    const double coverage = std::min(1.0, static_cast<double>(bucket_count) / expected_5m_buckets);

    client->execSqlSync(
        R"SQL(
INSERT INTO power_energy (device_id, granularity, time_start, energy_wh, coverage, sample_count)
VALUES (NULL, ?, ?, ?, ?, ?)
ON CONFLICT DO UPDATE SET
    energy_wh = excluded.energy_wh,
    coverage = excluded.coverage,
    sample_count = excluded.sample_count
)SQL",
        period,
        period_start,
        energy_wh,
        coverage,
        sample_count);

    const auto energy_rows = client->execSqlSync(
        "SELECT id FROM power_energy WHERE device_id IS NULL AND granularity = ? AND time_start = ?",
        period,
        period_start);
    if (energy_rows.empty())
        return std::nullopt;
    const int64_t energy_id = energy_rows[0]["id"].as<int64_t>();

    json target;
    target["id"] = energy_id;
    target["deviceId"] = nullptr;
    target["granularity"] = period;
    target["timeStart"] = period_start;
    target["energyWh"] = energy_wh;
    target["coverage"] = coverage;
    target["sampleCount"] = sample_count;

    json metrics;
    metrics["totalEnergyWh"] = energy_wh;
    metrics["coveragePercent"] = std::round(coverage * 1000.0) / 10.0;
    metrics["sampleCount"] = sample_count;

    json body;
    body["deviceId"] = nullptr;
    body["period"] = period;
    body["periodStart"] = period_start;
    body["metrics"] = metrics;
    body["target"] = target;
    body["children"] = json::array();
    body["embed"] = true;

    service::AgentSleepJobResult agent_result;
    std::string error;
    if (service::runPowerJobSync(AppState::get().config.agent.base_url, body, agent_result, error)
        != service::AgentClientResult::success)
    {
        LOG_WARN("power report generation failed ({} {}): {}", period, period_start, error);
        return std::nullopt;
    }

    int64_t report_id = 0;
    try
    {
        client->execSqlSync(
            R"SQL(
INSERT INTO power_report (energy_id, device_id, period, period_start, metrics, report_text, created_at)
VALUES (?, NULL, ?, ?, ?, ?, ?)
ON CONFLICT DO UPDATE SET
    energy_id = excluded.energy_id,
    metrics = excluded.metrics,
    report_text = excluded.report_text,
    created_at = excluded.created_at
)SQL",
            energy_id,
            period,
            period_start,
            metrics.dump(),
            agent_result.text,
            formatTimestamp());

        const auto report_rows = client->execSqlSync(
            "SELECT id FROM power_report WHERE period = ? AND period_start = ? AND device_id IS NULL",
            period,
            period_start);
        if (report_rows.empty())
            return std::nullopt;
        report_id = report_rows[0]["id"].as<int64_t>();
    }
    catch (const std::exception& e)
    {
        LOG_WARN("power_report write failed ({} {}): {}", period, period_start, e.what());
        return std::nullopt;
    }

    storeReportEmbedding(client, report_id, agent_result.embedding);
    return report_id;
}

bool PowerStore::ensureDailyReport(const drogon::orm::DbClientPtr& client, const std::string& date)
{
    const std::string day_start = date + " 00:00:00";
    const auto day_end_rows = client->execSqlSync("SELECT date(?, '+1 day') AS d", date);
    const std::string day_end = (day_end_rows.empty() ? date : day_end_rows[0]["d"].as<std::string>()) + " 00:00:00";

    const auto pre_existing = client->execSqlSync(
        "SELECT id FROM power_report WHERE period = '24h' AND period_start = ? AND device_id IS NULL", day_start);
    const bool already_had_report = !pre_existing.empty();

    const auto report_id = generateReport(client, "24h", day_start, day_start, day_end, 288.0);
    if (!report_id)
        return false;

    if (already_had_report)
        return true; // 캐시 재사용 — 매 요청마다 인사이트를 다시 만들 필요는 없다.

    // sleep_manager.cpp 와 동일한 지점: 리포트가 막 새로 생성된 직후에만 그 날짜 인사이트도 생성한다.
    const auto user_rows = client->execSqlSync("SELECT id FROM user");
    for (const auto& row : user_rows)
    {
        const auto user_id = row["id"].as<int64_t>();
        std::string insight_error;
        if (!service::generateAndPersistInsights(
                client, AppState::get().config.agent.base_url, user_id, "power", date, insight_error))
            LOG_WARN("power insight generation failed (user {}): {}", user_id, insight_error);
    }

    return true;
}

bool PowerStore::ensureHourlyReport(const drogon::orm::DbClientPtr& client, const std::string& hour_start)
{
    const auto hour_end_rows = client->execSqlSync("SELECT datetime(?, '+1 hour') AS d", hour_start);
    const std::string hour_end = hour_end_rows.empty() ? hour_start : hour_end_rows[0]["d"].as<std::string>();

    return generateReport(client, "1h", hour_start, hour_start, hour_end, 12.0).has_value();
}

Json::Value PowerStore::queryReport(
    drogon::orm::DbClientPtr client,
    const std::string& device_external_id,
    const std::string& ui_period,
    const std::string& period_start_hint)
{
    Json::Value out;
    std::optional<std::string> api_period;
    if (ui_period == "hour")
        api_period = "1h";
    else if (ui_period == "day")
        api_period = "24h";
    else if (ui_period == "week")
        api_period = "1w";
    else if (ui_period == "month")
        api_period = "1mo";
    else if (ui_period == "year")
        api_period = "1yr";

    if (!api_period)
    {
        out["supported"] = false;
        out["text"] = "선택한 시간 간격은 AI 리포트를 제공하지 않습니다.";
        return out;
    }

    const auto table_rows = client->execSqlSync(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='power_report' LIMIT 1");
    if (table_rows.empty())
    {
        out["supported"] = true;
        out["period"] = *api_period;
        out["text"] = "리포트 준비 중입니다.";
        return out;
    }

    std::optional<int64_t> device_db_id;
    if (!device_external_id.empty() && device_external_id != "all")
    {
        device_db_id = dev::dbIdForWireId(client, device_external_id);
    }

    const std::string ref_date = period_start_hint.empty()
        ? InsightsStore::referenceDate(client)
        : period_start_hint;

    const bool exact_match = !period_start_hint.empty() && (
        (*api_period == "1h" && period_start_hint.find(' ') != std::string::npos)
        || (*api_period == "24h" && period_start_hint.size() == 10)
        || (*api_period == "1mo" && period_start_hint.size() == 10)
        || (*api_period == "1w" && period_start_hint.size() == 10));

    const auto now_rows = client->execSqlSync("SELECT datetime('now', 'localtime') AS now");
    const std::string now_full = now_rows.empty() ? (ref_date + " 00:00:00") : now_rows[0]["now"].as<std::string>();
    const std::string today = now_full.substr(0, 10);

    // 일간(24h)/시간(1h) 전체 가구(all) 리포트는 없으면 지금 만든다 (sleep_plan 과 동일한
    // "요청 시점에 캐시 확인 → 없으면 생성" 패턴). 주/월/년, 기기별 리포트는 아직 미지원.
    if (*api_period == "24h" && !device_db_id)
    {
        const std::string target_date = exact_match ? period_start_hint.substr(0, 10) : ref_date.substr(0, 10);
        if (target_date <= today)
            ensureDailyReport(client, target_date);
    }
    else if (*api_period == "1h" && !device_db_id)
    {
        // hint 가 있으면 그 시각을, 없으면 "방금 끝난 시간"(진행 중인 시간은 아직 데이터가 안 찼음)을 생성한다.
        std::string target_hour_start;
        if (exact_match)
        {
            target_hour_start = period_start_hint.substr(0, 13) + ":00:00";
        }
        else
        {
            const auto prev_hour_rows = client->execSqlSync("SELECT datetime(?, '-1 hour') AS d", now_full);
            const std::string prev_hour = prev_hour_rows.empty() ? now_full : prev_hour_rows[0]["d"].as<std::string>();
            target_hour_start = prev_hour.substr(0, 13) + ":00:00";
        }
        if (target_hour_start <= now_full)
            ensureHourlyReport(client, target_hour_start);
    }

    std::ostringstream sql;
    sql << "SELECT metrics, report_text, period_start FROM power_report WHERE period = '" << *api_period << "'";
    if (exact_match)
    {
        const std::string exact_value = *api_period == "1h" ? period_start_hint.substr(0, 13) + ":00:00" : period_start_hint;
        sql << " AND period_start = '" << exact_value << "'";
    }
    else
    {
        // 1h 는 "YYYY-MM-DD HH:00:00" 전체 타임스탬프로 저장되므로 날짜만으로 비교하면 안 되고,
        // 지금 시각(now_full)과 비교해야 "가장 최근 리포트"가 정확히 나온다.
        const std::string compare_value = *api_period == "1h" ? now_full : today;
        sql << " AND period_start <= '" << compare_value << "'";
    }
    if (device_db_id)
        sql << " AND device_id = " << *device_db_id;
    else
        sql << " AND device_id IS NULL";
    sql << " ORDER BY period_start DESC LIMIT 1";

    const auto rows = client->execSqlSync(sql.str());
    out["supported"] = true;
    out["period"] = *api_period;
    if (rows.empty() || rows[0]["report_text"].isNull() || rows[0]["report_text"].as<std::string>().empty())
    {
        out["text"] = "리포트 준비 중입니다.";
        return out;
    }

    out["text"] = rows[0]["report_text"].as<std::string>();
    if (!rows[0]["metrics"].isNull())
    {
        Json::Value metrics;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream stream(rows[0]["metrics"].as<std::string>());
        if (Json::parseFromStream(builder, stream, &metrics, &errors))
            out["metrics"] = metrics;
    }
    return out;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
