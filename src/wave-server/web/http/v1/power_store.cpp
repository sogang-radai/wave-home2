#include "power_store.h"

#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>

#include "../../../app/app_state.h"
#include "../../../service/power_manager.h"
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
        const auto device_rows = client->execSqlSync(
            "SELECT id FROM device WHERE external_id = ? LIMIT 1",
            device_external_id);
        if (!device_rows.empty())
            device_clause = " AND device_id = " + device_rows[0]["id"].as<std::string>();
        else
            return series;
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
        const auto device_rows = client->execSqlSync(
            "SELECT id FROM device WHERE external_id = ? LIMIT 1",
            device_external_id);
        if (!device_rows.empty())
            device_db_id = device_rows[0]["id"].as<int64_t>();
    }

    const std::string ref_date = period_start_hint.empty()
        ? InsightsStore::referenceDate(client)
        : period_start_hint;

    const bool exact_match = !period_start_hint.empty() && (
        (*api_period == "1h" && period_start_hint.find(' ') != std::string::npos)
        || (*api_period == "24h" && period_start_hint.size() == 10)
        || (*api_period == "1mo" && period_start_hint.size() == 10)
        || (*api_period == "1w" && period_start_hint.size() == 10));

    std::ostringstream sql;
    sql << "SELECT metrics, report_text, period_start FROM power_report WHERE period = '" << *api_period << "'";
    if (exact_match)
        sql << " AND period_start = '" << period_start_hint << "'";
    else
        sql << " AND period_start <= '" << ref_date.substr(0, 10) << "'";
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
