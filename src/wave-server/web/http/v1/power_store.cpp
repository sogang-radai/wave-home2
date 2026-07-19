#include "power_store.h"
#include "../../../db/database.h"

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
#include "util/time_util.h"
#include "../../../service/agent/agent_client.h"
#include "../../../service/agent/agent_job_queue.h"
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

    const dev::DeviceManifestEntry* find_manifest_entry(
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

    std::string room_name_for_entry(const dev::DeviceManifestEntry& entry, const dev::DeviceManager& devices)
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

int PowerStore::step_seconds_for_range(const std::string& range)
{
    if (range == "min10")
        return 10;
    if (range == "min30")
        return 30;
    if (range == "hour")
        return 60;
    return 1;
}

int PowerStore::point_count_for_range(const std::string& range)
{
    (void)range;
    return 60;
}

std::string PowerStore::format_ago_label(int seconds_ago)
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
        const auto* entry = find_manifest_entry(devices, plug_id);
        const auto reading = power.getReading(plug_id);

        std::string room_name = "미지정";
        std::string name = entry ? entry->config.value("name", plug_id) : plug_id;
        std::string summary = entry ? entry->config.value("description", "") : "";
        if (entry)
            room_name = room_name_for_entry(*entry, devices);

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
        plug["metering"] = power.isMeteringEnabled(plug_id);
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

    const int step_seconds = step_seconds_for_range(range);
    const int points = point_count_for_range(range);
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
        point["label"] = format_ago_label(seconds_ago);
        point["value"] = found || !history.empty() ? chosen.w : 0.0;
        point["wh"] = point["value"].asDouble() * (step_seconds / 3600.0);
        point["v"] = found || !history.empty() ? chosen.v : 0.0;
        point["a"] = found || !history.empty() ? chosen.a : 0.0;
        series.append(point);
    }
    return series;
}

Json::Value PowerStore::period_trend(
    db::DbClientPtr client,
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
        ? InsightsStore::reference_date(client)
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
        // date-only(`YYYY-MM-DD`)와 datetime(`YYYY-MM-DD 00:00:00`) 행이 공존할 수 있어
        // calendar day로 묶어 요일 라벨이 두 번 나오지 않게 한다.
        const auto rows = client->execSqlSync(
            "SELECT date(time_start) AS day, MAX(energy_wh) AS energy_wh"
            " FROM power_energy WHERE granularity='24h'"
            " AND date(time_start) BETWEEN date(?, '-6 days') AND date(?)" + device_clause
            + " GROUP BY date(time_start) ORDER BY day",
            ref_date,
            ref_date);
        static const char* weekdays[] = {"일", "월", "화", "수", "목", "금", "토"};
        for (const auto& row : rows)
        {
            const auto day = row["day"].as<std::string>();
            const auto date_rows = client->execSqlSync("SELECT strftime('%w', ?)", day);
            int wday = date_rows.empty() ? 0 : date_rows[0][0].as<int>();
            appendPoint(weekdays[wday], row["energy_wh"].as<double>());
        }
        return series;
    }

    if (ui_period == "month")
    {
        const std::string month_prefix = ref_date.substr(0, 7);
        const auto rows = client->execSqlSync(
            "SELECT date(time_start) AS day, MAX(energy_wh) AS energy_wh"
            " FROM power_energy WHERE granularity='24h'"
            " AND time_start LIKE ?" + device_clause
            + " GROUP BY date(time_start) ORDER BY day",
            month_prefix + "%");
        for (const auto& row : rows)
        {
            const auto day = row["day"].as<std::string>();
            if (day.size() < 10)
                continue;
            const int day_num = std::stoi(day.substr(8, 2));
            appendPoint(std::to_string(day_num), row["energy_wh"].as<double>());
        }
        return series;
    }

    if (ui_period == "year")
    {
        const std::string year_prefix = ref_date.substr(0, 4);
        std::map<int, double> monthly;
        // 일별 중복 행을 먼저 접은 뒤 월합 — 그렇지 않으면 date/datetime 중복이 요금을 두 배로 만든다.
        const auto rows = client->execSqlSync(
            "SELECT substr(day, 6, 2) AS month_key, SUM(energy_wh) AS total FROM ("
            "  SELECT date(time_start) AS day, MAX(energy_wh) AS energy_wh"
            "  FROM power_energy WHERE granularity='24h' AND time_start LIKE ?" + device_clause
            + "  GROUP BY date(time_start)"
            ") GROUP BY substr(day, 1, 7) ORDER BY month_key",
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

void PowerStore::store_report_embedding(
    const db::DbClientPtr& client,
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
        WLOG_WARN("power report embedding store failed: {}", e.what());
    }
}

std::optional<int64_t> PowerStore::generate_report(
    const db::DbClientPtr& client,
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
        return std::nullopt;

    const double energy_wh = std::round(agg_rows[0]["energy_wh"].as<double>() * 10000.0) / 10000.0;
    const int64_t sample_count = agg_rows[0]["samples"].as<int64_t>();
    const int64_t bucket_count = agg_rows[0]["buckets"].as<int64_t>();
    const double coverage = std::min(1.0, static_cast<double>(bucket_count) / expected_5m_buckets);

    const std::string energy_time_start =
        (period == "24h" || period == "1w" || period == "1mo" || period == "1yr")
            && period_start.size() >= 10
            ? period_start.substr(0, 10)
            : period_start;

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
        energy_time_start,
        energy_wh,
        coverage,
        sample_count);

    const auto energy_rows = client->execSqlSync(
        "SELECT id FROM power_energy WHERE device_id IS NULL AND granularity = ? AND time_start = ?",
        period,
        energy_time_start);
    if (energy_rows.empty())
        return std::nullopt;
    const int64_t energy_id = energy_rows[0]["id"].as<int64_t>();

    json target;
    target["id"] = energy_id;
    target["deviceId"] = nullptr;
    target["granularity"] = period;
    target["timeStart"] = energy_time_start;
    target["energyWh"] = energy_wh;
    target["coverage"] = coverage;
    target["sampleCount"] = sample_count;

    json metrics;
    metrics["totalEnergyWh"] = energy_wh;
    metrics["coveragePercent"] = std::round(coverage * 1000.0) / 10.0;
    metrics["sampleCount"] = sample_count;

    if (period == "1w" || period == "1mo" || period == "1yr")
    {
        const auto day_rows = client->execSqlSync(
            R"SQL(
SELECT COUNT(DISTINCT date(time_start)) AS days
FROM power_energy
WHERE device_id IS NULL AND granularity = '5m' AND time_start >= ? AND time_start < ?
)SQL",
            window_start,
            window_end);
        const int64_t days = day_rows.empty() ? 0 : day_rows[0]["days"].as<int64_t>();
        metrics["days"] = days;
        if (days > 0)
            metrics["avgDailyWh"] = std::round((energy_wh / static_cast<double>(days)) * 100.0) / 100.0;
    }

    const auto by_device = build_by_device(client, window_start, window_end);
    if (!by_device.empty())
        metrics["byDevice"] = by_device;

    if (const auto prev = previous_window_energy(client, period, period_start, window_start, window_end))
    {
        if (*prev > 0.0)
            metrics["vsPrevPct"] = std::round(((energy_wh - *prev) / *prev) * 1000.0) / 10.0;
    }

    const auto peak_rows = client->execSqlSync(
        R"SQL(
SELECT time_start, energy_wh FROM power_energy
WHERE device_id IS NULL AND granularity = '5m' AND time_start >= ? AND time_start < ?
ORDER BY energy_wh DESC LIMIT 1
)SQL",
        window_start,
        window_end);
    if (!peak_rows.empty())
    {
        // 5m Wh → approximate average W over the bucket (Wh * 12).
        metrics["peakW"] = std::round(peak_rows[0]["energy_wh"].as<double>() * 12.0 * 10.0) / 10.0;
        metrics["peakAt"] = peak_rows[0]["time_start"].as<std::string>();
    }

    json body;
    body["deviceId"] = nullptr;
    body["period"] = period;
    body["periodStart"] = period_start;
    body["metrics"] = metrics;
    body["target"] = target;
    body["children"] = build_children(client, period, window_start, window_end);
    body["embed"] = true;

    service::AgentSleepJobResult agent_result;
    std::string error;
    if (service::runPowerJobSync(AppState::get().config.agent.base_url, body, agent_result, error)
        != service::AgentClientResult::success)
    {
        WLOG_WARN("power report generation failed ({} {}): {}", period, period_start, error);
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
        WLOG_WARN("power_report write failed ({} {}): {}", period, period_start, e.what());
        return std::nullopt;
    }

    store_report_embedding(client, report_id, agent_result.embedding);
    return report_id;
}

json PowerStore::build_children(
    const db::DbClientPtr& client,
    const std::string& period,
    const std::string& window_start,
    const std::string& window_end)
{
    json children = json::array();
    std::string child_granularity;
    if (period == "1h")
        child_granularity = "5m";
    else if (period == "24h")
        child_granularity = "1h";
    else if (period == "1w" || period == "1mo" || period == "1yr")
        child_granularity = "24h";
    else
        return children;

    try
    {
        const auto rows = client->execSqlSync(
            R"SQL(
SELECT id, device_id, granularity, time_start, energy_wh, coverage, sample_count
FROM power_energy
WHERE device_id IS NULL AND granularity = ? AND time_start >= ? AND time_start < ?
ORDER BY time_start
)SQL",
            child_granularity,
            window_start,
            window_end);
        for (const auto& row : rows)
        {
            json child;
            child["id"] = row["id"].as<int64_t>();
            child["deviceId"] = nullptr;
            child["granularity"] = row["granularity"].as<std::string>();
            child["timeStart"] = row["time_start"].as<std::string>();
            child["energyWh"] = row["energy_wh"].as<double>();
            child["coverage"] = row["coverage"].as<double>();
            child["sampleCount"] = row["sample_count"].as<int64_t>();
            children.push_back(std::move(child));
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("power report children query failed: {}", e.what());
    }
    return children;
}

json PowerStore::build_by_device(
    const db::DbClientPtr& client,
    const std::string& window_start,
    const std::string& window_end)
{
    json by_device = json::array();
    try
    {
        const auto rows = client->execSqlSync(
            R"SQL(
SELECT pe.device_id AS device_id,
       COALESCE(SUM(pe.energy_wh), 0) AS energy_wh
FROM power_energy pe
LEFT JOIN device d ON d.id = pe.device_id
WHERE pe.device_id IS NOT NULL
  AND pe.granularity = '5m'
  AND pe.time_start >= ? AND pe.time_start < ?
GROUP BY pe.device_id
HAVING COALESCE(json_extract(d.settings_json, '$.metering'), 1) != 0
ORDER BY energy_wh DESC
)SQL",
            window_start,
            window_end);
        for (const auto& row : rows)
        {
            json item;
            item["deviceId"] = row["device_id"].as<int64_t>();
            item["energyWh"] = std::round(row["energy_wh"].as<double>() * 100.0) / 100.0;
            by_device.push_back(std::move(item));
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("power report by_device query failed: {}", e.what());
    }
    return by_device;
}

std::optional<double> PowerStore::previous_window_energy(
    const db::DbClientPtr& client,
    const std::string& period,
    const std::string& period_start,
    const std::string& window_start,
    const std::string& window_end)
{
    (void)period_start;
    std::string offset;
    if (period == "1h")
        offset = "-1 hour";
    else if (period == "24h")
        offset = "-1 day";
    else if (period == "1w")
        offset = "-7 day";
    else if (period == "1mo")
        offset = "-30 day";
    else if (period == "1yr")
        offset = "-365 day";
    else
        return std::nullopt;

    try
    {
        const auto prev_start_rows = client->execSqlSync("SELECT datetime(?, ?) AS d", window_start, offset);
        const auto prev_end_rows = client->execSqlSync("SELECT datetime(?, ?) AS d", window_end, offset);
        if (prev_start_rows.empty() || prev_end_rows.empty())
            return std::nullopt;
        const auto rows = client->execSqlSync(
            R"SQL(
SELECT COALESCE(SUM(energy_wh), 0) AS energy_wh, COUNT(*) AS buckets
FROM power_energy
WHERE device_id IS NULL AND granularity = '5m' AND time_start >= ? AND time_start < ?
)SQL",
            prev_start_rows[0]["d"].as<std::string>(),
            prev_end_rows[0]["d"].as<std::string>());
        if (rows.empty() || rows[0]["buckets"].as<int64_t>() == 0)
            return std::nullopt;
        return rows[0]["energy_wh"].as<double>();
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

void PowerStore::run_queued_report(const service::AgentJob& job)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    const bool existed = !client
                              ->execSqlSync(
                                  "SELECT id FROM power_report WHERE period = ? AND period_start = ? AND device_id IS NULL",
                                  job.period,
                                  job.periodStart)
                              .empty();

    const auto report_id = generate_report(
        client,
        job.period,
        job.periodStart,
        job.windowStart,
        job.windowEnd,
        job.expected5mBuckets);
    if (!report_id)
        return;

    if (!existed && job.period == "24h")
    {
        const std::string date =
            job.periodStart.size() >= 10 ? job.periodStart.substr(0, 10) : job.periodStart;
        enqueue_power_insights_for_date(date);
    }
}

bool PowerStore::enqueue_report_job(
    const std::string& period,
    const std::string& period_start,
    const std::string& window_start,
    const std::string& window_end,
    double expected_5m_buckets,
    bool wait)
{
    service::AgentJob job;
    job.kind = service::AgentJobKind::PowerReport;
    job.period = period;
    job.periodStart = period_start;
    job.windowStart = window_start;
    job.windowEnd = window_end;
    job.expected5mBuckets = expected_5m_buckets;

    if (wait)
        return service::AgentJobQueue::get().enqueueAndWait(std::move(job), std::chrono::seconds(90));
    return service::AgentJobQueue::get().enqueue(std::move(job));
}

void PowerStore::enqueue_hourly_report(const std::string& hour_start)
{
    const auto client = AppState::get().db();
    if (!client)
        return;
    const auto hour_end_rows = client->execSqlSync("SELECT datetime(?, '+1 hour') AS d", hour_start);
    const std::string hour_end = hour_end_rows.empty() ? hour_start : hour_end_rows[0]["d"].as<std::string>();
    enqueue_report_job("1h", hour_start, hour_start, hour_end, 12.0, false);
}

void PowerStore::enqueue_daily_report(const std::string& date)
{
    const std::string day_start = date + " 00:00:00";
    const auto client = AppState::get().db();
    if (!client)
        return;
    const auto day_end_rows = client->execSqlSync("SELECT date(?, '+1 day') AS d", date);
    const std::string day_end =
        (day_end_rows.empty() ? date : day_end_rows[0]["d"].as<std::string>()) + " 00:00:00";
    enqueue_report_job("24h", day_start, day_start, day_end, 288.0, false);
}

void PowerStore::enqueue_weekly_report(const std::string& period_start_date)
{
    const auto client = AppState::get().db();
    if (!client)
        return;
    const std::string window_start = period_start_date + " 00:00:00";
    const auto end_rows = client->execSqlSync("SELECT date(?, '+7 day') AS d", period_start_date);
    const std::string window_end =
        (end_rows.empty() ? period_start_date : end_rows[0]["d"].as<std::string>()) + " 00:00:00";
    enqueue_report_job("1w", period_start_date, window_start, window_end, 288.0 * 7.0, false);
}

void PowerStore::enqueue_monthly_report(const std::string& period_start_date)
{
    const auto client = AppState::get().db();
    if (!client)
        return;
    const std::string window_start = period_start_date + " 00:00:00";
    const auto end_rows = client->execSqlSync("SELECT date(?, '+30 day') AS d", period_start_date);
    const std::string window_end =
        (end_rows.empty() ? period_start_date : end_rows[0]["d"].as<std::string>()) + " 00:00:00";
    enqueue_report_job("1mo", period_start_date, window_start, window_end, 288.0 * 30.0, false);
}

void PowerStore::enqueue_yearly_report(const std::string& period_start_date)
{
    const auto client = AppState::get().db();
    if (!client)
        return;
    const std::string window_start = period_start_date + " 00:00:00";
    const auto end_rows = client->execSqlSync("SELECT date(?, '+365 day') AS d", period_start_date);
    const std::string window_end =
        (end_rows.empty() ? period_start_date : end_rows[0]["d"].as<std::string>()) + " 00:00:00";
    enqueue_report_job("1yr", period_start_date, window_start, window_end, 288.0 * 365.0, false);
}

void PowerStore::enqueue_power_insights_for_date(const std::string& date)
{
    auto client = AppState::get().db();
    if (!client)
        return;
    try
    {
        const auto user_rows = client->execSqlSync("SELECT id FROM user");
        for (const auto& row : user_rows)
        {
            service::AgentJob insight;
            insight.kind = service::AgentJobKind::Insight;
            insight.userId = row["id"].as<int64_t>();
            insight.surface = "power";
            insight.date = date;
            service::AgentJobQueue::get().enqueue(std::move(insight));
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("enqueue power insights failed: {}", e.what());
    }
}

bool PowerStore::ensure_daily_report(const db::DbClientPtr& client, const std::string& date)
{
    if (!client)
        return false;
    const std::string day_start = date + " 00:00:00";
    const auto day_end_rows = client->execSqlSync("SELECT date(?, '+1 day') AS d", date);
    const std::string day_end =
        (day_end_rows.empty() ? date : day_end_rows[0]["d"].as<std::string>()) + " 00:00:00";
    return enqueue_report_job("24h", day_start, day_start, day_end, 288.0, true);
}

bool PowerStore::ensure_hourly_report(const db::DbClientPtr& client, const std::string& hour_start)
{
    if (!client)
        return false;
    const auto hour_end_rows = client->execSqlSync("SELECT datetime(?, '+1 hour') AS d", hour_start);
    const std::string hour_end = hour_end_rows.empty() ? hour_start : hour_end_rows[0]["d"].as<std::string>();
    return enqueue_report_job("1h", hour_start, hour_start, hour_end, 12.0, true);
}

Json::Value PowerStore::query_report(
    db::DbClientPtr client,
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
        ? InsightsStore::reference_date(client)
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
            ensure_daily_report(client, target_date);
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
            ensure_hourly_report(client, target_hour_start);
    }

    std::ostringstream sql;
    sql << "SELECT metrics, report_text, period_start FROM power_report WHERE period = '" << *api_period << "'";
    if (exact_match)
    {
        // 24h 도 1h 와 마찬가지로 "YYYY-MM-DD 00:00:00" 전체 타임스탬프로 저장되므로,
        // 날짜만 담긴 hint(예: "2026-07-11")를 그대로 비교하면 절대 매치되지 않는다.
        const std::string exact_value = *api_period == "1h"
            ? period_start_hint.substr(0, 13) + ":00:00"
            : *api_period == "24h" ? period_start_hint.substr(0, 10) + " 00:00:00" : period_start_hint;
        sql << " AND period_start = '" << exact_value << "'";
    }
    else
    {
        // 1h/24h 모두 저장 시 시각까지 포함한 전체 타임스탬프를 쓰므로, 날짜만으로 비교하면
        // (예: period_start <= '2026-07-11') 같은 날짜의 실제 행("2026-07-11 00:00:00")보다
        // 문자열 비교상 더 크다고 판정되어 방금 생성한 리포트를 못 찾는 문제가 있었다.
        // 항상 시각까지 포함한 값으로 비교해 "가장 최근 리포트"가 정확히 나오게 한다.
        const std::string compare_value = *api_period == "1h" ? now_full : today + " 23:59:59";
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
