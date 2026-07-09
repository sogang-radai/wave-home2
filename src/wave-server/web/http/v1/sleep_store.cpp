#include "sleep_store.h"

#include <cmath>
#include <sstream>

#include "../../../app/app_state.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace
{
    std::string padTime(const std::string& hhmm)
    {
        if (hhmm.size() >= 5)
            return hhmm.substr(0, 5);
        return hhmm;
    }

    int64_t parseSessionId(const std::string& session_id, int64_t fallback)
    {
        if (session_id.empty() || session_id == "main")
            return fallback;
        try
        {
            return std::stoll(session_id);
        }
        catch (const std::exception&)
        {
            return fallback;
        }
    }

    int minutesOf(const std::string& time)
    {
        if (time.size() < 5)
            return 0;
        const int hour = std::stoi(time.substr(0, 2));
        const int minute = std::stoi(time.substr(3, 2));
        return hour * 60 + minute;
    }

    bool parseJsonField(const drogon::orm::Field& field, Json::Value& out)
    {
        if (field.isNull())
            return false;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream stream(field.as<std::string>());
        return Json::parseFromStream(builder, stream, &out, &errors);
    }

    std::string normalizeStageLabel(const std::string& label)
    {
        if (label == "deep" || label == "light" || label == "rem")
            return label;
        return "awake";
    }

    std::string resolveStageLabel(const drogon::orm::Row& row)
    {
        if (!row["stage_label"].isNull())
            return normalizeStageLabel(row["stage_label"].as<std::string>());

        Json::Value ratio;
        if (parseJsonField(row["stage_ratio"], ratio))
        {
            std::string best = "awake";
            double best_value = -1.0;
            static const char* kStages[] = {"deep", "light", "rem", "awake"};
            for (const char* stage : kStages)
            {
                const double value = ratio.get(stage, 0.0).asDouble();
                if (value > best_value)
                {
                    best_value = value;
                    best = stage;
                }
            }
            return best;
        }

        Json::Value status;
        if (parseJsonField(row["status_ratio"], status))
        {
            const double asleep = status.get("asleep", 0.0).asDouble();
            return asleep >= 0.5 ? "light" : "awake";
        }

        return "awake";
    }

    int movementLevelFromRow(const drogon::orm::Row& row)
    {
        const double toss_mean = row["toss_mean"].isNull() ? 0.0 : row["toss_mean"].as<double>();
        const int toss_events = row["toss_events"].isNull() ? 0 : row["toss_events"].as<int>();
        const double level = std::min(100.0, toss_mean * 700.0 + toss_events * 14.0);
        return static_cast<int>(std::round(level));
    }

    std::string formatDurationText(int total_seconds)
    {
        const int minutes = static_cast<int>(std::round(total_seconds / 60.0));
        const int hours = minutes / 60;
        const int remainder = minutes % 60;
        if (hours > 0 && remainder > 0)
            return std::to_string(hours) + "시간 " + std::to_string(remainder) + "분";
        if (hours > 0)
            return std::to_string(hours) + "시간";
        return std::to_string(remainder) + "분";
    }

    std::string formatClockLabel(const std::string& timestamp)
    {
        if (timestamp.size() < 16)
            return timestamp;
        const int hour = std::stoi(timestamp.substr(11, 2));
        const int minute = std::stoi(timestamp.substr(14, 2));
        const bool pm = hour >= 12;
        const int hour12 = hour % 12 == 0 ? 12 : hour % 12;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", hour12, minute, pm ? "PM" : "AM");
        return buf;
    }

    Json::Value buildStageBreakdown(const drogon::orm::Field& stage_totals_field)
    {
        Json::Value breakdown(Json::arrayValue);
        Json::Value totals;
        if (!parseJsonField(stage_totals_field, totals))
            return breakdown;

        static const struct
        {
            const char* key;
            const char* label;
            const char* tone;
            int typical_start;
            int typical_end;
        } kStages[] = {
            {"deep", "깊은 수면", "deep", 15, 25},
            {"light", "얕은 수면", "light", 45, 55},
            {"rem", "REM 수면", "rem", 20, 25},
            {"awake", "각성", "awake", 5, 10},
        };

        int asleep_total_s = 0;
        for (const auto& stage : kStages)
            asleep_total_s += totals.get(stage.key, 0).asInt();

        if (asleep_total_s <= 0)
            return breakdown;

        for (const auto& stage : kStages)
        {
            const int seconds = totals.get(stage.key, 0).asInt();
            if (seconds <= 0)
                continue;

            Json::Value item;
            item["label"] = stage.label;
            item["tone"] = stage.tone;
            item["percent"] = static_cast<int>(std::round((seconds * 100.0) / asleep_total_s));
            item["durationText"] = formatDurationText(seconds);
            Json::Value typical(Json::arrayValue);
            typical.append(stage.typical_start);
            typical.append(stage.typical_end);
            item["typicalPercentRange"] = typical;
            breakdown.append(item);
        }

        return breakdown;
    }

    Json::Value buildStageLog(const drogon::orm::Result& stats)
    {
        Json::Value stage_log(Json::arrayValue);
        for (size_t i = 0; i < stats.size(); ++i)
        {
            const auto& row = stats[i];
            if (row["hr_mean"].isNull() && row["br_mean"].isNull())
                continue;

            Json::Value point;
            point["time"] = formatClockLabel(row["time_start"].as<std::string>());
            point["heartRate"] = row["hr_mean"].isNull() ? 0.0 : row["hr_mean"].as<double>();
            point["breathRate"] = row["br_mean"].isNull() ? 0.0 : row["br_mean"].as<double>();
            stage_log.append(point);
        }
        return stage_log;
    }

    Json::Value buildSnoringEpisodes(const drogon::orm::Result& stats)
    {
        Json::Value episodes(Json::arrayValue);
        for (size_t i = 0; i < stats.size(); ++i)
        {
            const auto& row = stats[i];
            const double snore_ratio = row["snore_ratio"].isNull() ? 0.0 : row["snore_ratio"].as<double>();
            if (snore_ratio < 0.12)
                continue;

            Json::Value episode;
            episode["time"] = formatClockLabel(row["time_start"].as<std::string>());
            episode["durationMinutes"] = static_cast<int>(std::round(snore_ratio * 30.0));
            episodes.append(episode);
        }
        return episodes;
    }
}

SleepStore::SleepStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::string SleepStore::toIsoKst(const std::string& timestamp)
{
    if (timestamp.size() >= 19)
        return timestamp.substr(0, 10) + "T" + timestamp.substr(11, 8) + "+09:00";
    return timestamp;
}

int SleepStore::computeScore(double efficiency)
{
    if (efficiency <= 0.0)
        return 0;
    return static_cast<int>(std::round(std::min(1.0, efficiency) * 100.0));
}

Json::Value SleepStore::getTodaySummary(int64_t user_id) const
{
    Json::Value out;
    out["date"] = Json::Value(Json::objectValue);

    const auto& state = AppState::get();
    const auto rows = (state.demo_mode && !state.anchor_date.empty())
        ? m_client->execSqlSync(
            "SELECT * FROM sleep_session WHERE user_id = ? AND night_date = ? ORDER BY id DESC LIMIT 1",
            user_id,
            state.anchor_date)
        : m_client->execSqlSync(
            "SELECT * FROM sleep_session WHERE user_id = ? ORDER BY night_date DESC, id DESC LIMIT 1",
            user_id);
    if (rows.empty())
    {
        out["date"] = "";
        out["score"] = 0;
        out["achievedHours"] = 0;
        out["goalHours"] = 7.5;
        out["bedTime"] = "--:--";
        out["wakeTime"] = "--:--";
        return out;
    }

    const auto& row = rows[0];
    const double efficiency = row["efficiency"].isNull() ? 0.0 : row["efficiency"].as<double>();
    const int asleep_s = row["asleep_total_s"].isNull() ? 0 : row["asleep_total_s"].as<int>();
    const std::string onset = row["onset"].isNull() ? "" : row["onset"].as<std::string>();
    const std::string final_wake = row["final_wake"].isNull() ? "" : row["final_wake"].as<std::string>();

    out["date"] = row["night_date"].as<std::string>();
    out["score"] = computeScore(efficiency);
    out["achievedHours"] = std::round((asleep_s / 3600.0) * 10.0) / 10.0;
    out["goalHours"] = 7.5;
    out["bedTime"] = onset.size() >= 16 ? onset.substr(11, 5) : "--:--";
    out["wakeTime"] = final_wake.size() >= 16 ? final_wake.substr(11, 5) : "--:--";
    return out;
}

Json::Value SleepStore::getTodayPlan(int64_t user_id) const
{
    SettingsStore settings(m_client);
    const auto config = settings.getSleepConfig(user_id);

    Json::Value out;
    out["bedtime"] = config["bedtime"].asString();
    out["wakeTime"] = config["wakeTime"].asString();

    const int bedtime_min = minutesOf(config["bedtime"].asString());
    const int prep_min = std::max(10, config.get("dimStartMinutes", 30).asInt() + 10);
    const int prep_total = bedtime_min - prep_min;
    const int prep_hour = ((prep_total % (24 * 60)) + (24 * 60)) % (24 * 60) / 60;
    const int prep_minute = prep_total % 60;
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", prep_hour, prep_minute);
    out["prepTime"] = buf;

    const int dim_min = bedtime_min - config.get("dimStartMinutes", 30).asInt();
    const int dim_hour = ((dim_min % (24 * 60)) + (24 * 60)) % (24 * 60) / 60;
    const int dim_minute = dim_min % 60;
    std::snprintf(buf, sizeof(buf), "%02d:%02d", dim_hour, dim_minute);
    out["lightDimTime"] = buf;
    out["recommendedTemperatureCelsius"] = config.get("acTemp", 24).asInt();
    return out;
}

Json::Value SleepStore::getTodayPhoneUsage() const
{
    Json::Value out;
    out["usedMinutes"] = 0;
    out["goalMinutes"] = 10;
    return out;
}

Json::Value SleepStore::getTodayAutomationSummary(int64_t user_id) const
{
    SettingsStore settings(m_client);
    const auto config = settings.getSleepConfig(user_id);

    Json::Value items(Json::arrayValue);
    {
        Json::Value item;
        item["title"] = "취침 루틴";
        item["text"] = "취침 " + padTime(config["bedtime"].asString()) + " · 기상 "
            + padTime(config["wakeTime"].asString());
        items.append(item);
    }
    {
        Json::Value item;
        item["title"] = "조명";
        item["text"] = config.get("lightAuto", true).asBool()
            ? "취침 " + std::to_string(config.get("dimStartMinutes", 30).asInt()) + "분 전부터 어둡게"
            : "수동 조명";
        items.append(item);
    }
    {
        Json::Value item;
        item["title"] = "온도";
        item["text"] = config.get("acAuto", true).asBool()
            ? "에어컨 " + std::to_string(config.get("acTemp", 24).asInt()) + "℃ 목표"
            : "수동 온도";
        items.append(item);
    }
    return items;
}

Json::Value SleepStore::getDailySessions(int64_t user_id, const std::string& date) const
{
    auto rows = m_client->execSqlSync(
        "SELECT * FROM sleep_session WHERE user_id = ? AND night_date = ? ORDER BY onset ASC, id ASC",
        user_id,
        date);
    if (rows.empty())
        return Json::Value();

    Json::Value out;
    out["date"] = date;
    Json::Value sessions(Json::arrayValue);
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& row = rows[i];
        Json::Value session;
        session["sessionId"] = std::to_string(row["id"].as<int64_t>());
        session["label"] = rows.size() > 1 && i > 0 ? "추가 수면" : "주 수면";
        const double efficiency = row["efficiency"].isNull() ? 0.0 : row["efficiency"].as<double>();
        session["score"] = computeScore(efficiency);
        Json::Value window;
        window["start"] = row["onset"].isNull() ? "" : toIsoKst(row["onset"].as<std::string>());
        window["end"] = row["final_wake"].isNull() ? "" : toIsoKst(row["final_wake"].as<std::string>());
        session["sleepWindow"] = window;
        sessions.append(session);
    }
    out["sessions"] = sessions;
    return out;
}

Json::Value SleepStore::buildHypnogram(
    const drogon::orm::Result& stats,
    const std::string& start,
    const std::string& end) const
{
    Json::Value hypno;
    hypno["start"] = toIsoKst(start);
    hypno["end"] = toIsoKst(end);

    Json::Value segments(Json::arrayValue);
    Json::Value movement(Json::arrayValue);
    for (size_t i = 0; i < stats.size(); ++i)
    {
        const auto& row = stats[i];
        Json::Value segment;
        segment["stage"] = resolveStageLabel(row);
        segment["durationMinutes"] = 1;
        segments.append(segment);
        movement.append(movementLevelFromRow(row));
    }
    hypno["segments"] = segments;
    hypno["movementLevels"] = movement;
    return hypno;
}

Json::Value SleepStore::getDailyReport(
    int64_t user_id,
    const std::string& date,
    const std::string& session_id_text) const
{
    auto session_rows = m_client->execSqlSync(
        "SELECT * FROM sleep_session WHERE user_id = ? AND night_date = ? ORDER BY id ASC",
        user_id,
        date);
    if (session_rows.empty())
        return Json::Value();

    const int64_t default_id = session_rows[0]["id"].as<int64_t>();
    const int64_t session_id = parseSessionId(session_id_text, default_id);

    size_t session_index = 0;
    for (size_t i = 0; i < session_rows.size(); ++i)
    {
        if (session_rows[i]["id"].as<int64_t>() == session_id)
        {
            session_index = i;
            break;
        }
    }

    const auto& session_row = session_rows[session_index];
    const std::string onset = session_row["onset"].isNull()
        ? date + " 00:00:00"
        : session_row["onset"].as<std::string>();
    const std::string final_wake = session_row["final_wake"].isNull()
        ? date + " 23:59:00"
        : session_row["final_wake"].as<std::string>();

    auto stat_rows = m_client->execSqlSync(
        R"SQL(
SELECT * FROM sleep_stat
WHERE user_id = ? AND granularity = '1m' AND time_start >= ? AND time_start < ?
ORDER BY time_start ASC
)SQL",
        user_id,
        onset,
        final_wake);

    const int time_in_bed_s = session_row["time_in_bed_s"].isNull()
        ? 0
        : session_row["time_in_bed_s"].as<int>();
    const int asleep_s = session_row["asleep_total_s"].isNull()
        ? 0
        : session_row["asleep_total_s"].as<int>();
    const double efficiency = session_row["efficiency"].isNull()
        ? 0.0
        : session_row["efficiency"].as<double>();

    Json::Value out;
    out["sessionId"] = std::to_string(session_row["id"].as<int64_t>());
    out["label"] = "주 수면";
    out["date"] = date;
    out["score"] = computeScore(efficiency);
    out["sleepWindow"] = Json::Value(Json::objectValue);
    out["sleepWindow"]["start"] = toIsoKst(onset);
    out["sleepWindow"]["end"] = toIsoKst(final_wake);
    out["timeInBedMinutes"] = static_cast<int>(std::round(time_in_bed_s / 60.0));
    out["actualSleepMinutes"] = static_cast<int>(std::round(asleep_s / 60.0));

    Json::Value factors(Json::arrayValue);
    {
        Json::Value factor;
        factor["key"] = "duration";
        factor["label"] = "수면 시간";
        factor["value"] = std::to_string(out["actualSleepMinutes"].asInt()) + "분";
        factor["tag"] = out["actualSleepMinutes"].asInt() >= 420 ? "양호" : "부족";
        factor["tone"] = out["actualSleepMinutes"].asInt() >= 420 ? "good" : "warn";
        factors.append(factor);
    }
    {
        Json::Value factor;
        factor["key"] = "efficiency";
        factor["label"] = "수면 효율";
        factor["value"] = std::to_string(static_cast<int>(efficiency * 100)) + "%";
        factor["tag"] = efficiency >= 0.85 ? "양호" : "주의";
        factor["tone"] = efficiency >= 0.85 ? "good" : "warn";
        factors.append(factor);
    }
    out["scoreFactors"] = factors;
    out["stageBreakdown"] = buildStageBreakdown(session_row["stage_totals"]);

    out["hypnogram"] = buildHypnogram(stat_rows, onset, final_wake);

    auto stat_30m_rows = m_client->execSqlSync(
        R"SQL(
SELECT * FROM sleep_stat
WHERE user_id = ? AND session_id = ? AND granularity = '30m'
ORDER BY time_start ASC
)SQL",
        user_id,
        session_id);

    out["stageLog"] = buildStageLog(stat_30m_rows);
    out["snoringEpisodes"] = buildSnoringEpisodes(stat_30m_rows);
    out["analysis"] = Json::Value(Json::arrayValue);

    auto report_rows = m_client->execSqlSync(
        "SELECT report_text FROM sleep_report WHERE user_id = ? AND period = 'daily' AND period_start = ? LIMIT 1",
        user_id,
        date);
    if (!report_rows.empty() && !report_rows[0]["report_text"].isNull())
    {
        Json::Value analysis_item;
        analysis_item["label"] = "AI 분석";
        analysis_item["value"] = "요약";
        analysis_item["description"] = report_rows[0]["report_text"].as<std::string>();
        out["analysis"].append(analysis_item);
    }

    return out;
}

Json::Value SleepStore::getWeeklyReport(int64_t user_id, const std::string& week_start) const
{
    auto week_end_exclusive_rows = m_client->execSqlSync(
        "SELECT date(?, '+7 day') AS week_end",
        week_start);
    const std::string week_end_exclusive = week_end_exclusive_rows.empty()
        ? week_start
        : week_end_exclusive_rows[0]["week_end"].as<std::string>();

    auto week_end_inclusive_rows = m_client->execSqlSync(
        "SELECT date(?, '+6 day') AS week_end",
        week_start);
    const std::string week_end_inclusive = week_end_inclusive_rows.empty()
        ? week_start
        : week_end_inclusive_rows[0]["week_end"].as<std::string>();

    auto session_rows = m_client->execSqlSync(
        "SELECT * FROM sleep_session WHERE user_id = ? AND night_date >= ? AND night_date < ? ORDER BY night_date",
        user_id,
        week_start,
        week_end_exclusive);

    Json::Value out;
    out["weekStart"] = week_start;
    out["weekEnd"] = week_end_inclusive;

    Json::Value trend(Json::arrayValue);
    int score_sum = 0;
    int score_count = 0;
    static const char* kDays[] = {"일", "월", "화", "수", "목", "금", "토"};

    for (int i = 0; i < 7; ++i)
    {
        auto day_rows = m_client->execSqlSync(
            "SELECT date(?, '+' || ? || ' day') AS night_date",
            week_start,
            i);
        const std::string night_date = day_rows.empty() ? week_start : day_rows[0]["night_date"].as<std::string>();

        Json::Value point;
        point["date"] = night_date;

        auto day_tm_rows = m_client->execSqlSync(
            "SELECT strftime('%w', ?) AS wday",
            night_date);
        const int wday = day_tm_rows.empty() ? 0 : std::stoi(day_tm_rows[0]["wday"].as<std::string>());
        point["day"] = kDays[wday % 7];
        point["hours"] = 0;
        point["score"] = 0;

        for (size_t j = 0; j < session_rows.size(); ++j)
        {
            if (session_rows[j]["night_date"].as<std::string>() == night_date)
            {
                const double efficiency = session_rows[j]["efficiency"].isNull()
                    ? 0.0
                    : session_rows[j]["efficiency"].as<double>();
                const int asleep_s = session_rows[j]["asleep_total_s"].isNull()
                    ? 0
                    : session_rows[j]["asleep_total_s"].as<int>();
                point["hours"] = std::round((asleep_s / 3600.0) * 10.0) / 10.0;
                point["score"] = computeScore(efficiency);
                score_sum += point["score"].asInt();
                ++score_count;
                break;
            }
        }
        trend.append(point);
    }

    out["trend"] = trend;
    out["averageScore"] = score_count > 0 ? score_sum / score_count : 0;
    out["score"] = out["averageScore"];

    auto report_rows = m_client->execSqlSync(
        "SELECT report_text FROM sleep_report WHERE user_id = ? AND period = 'weekly' AND period_start = ? LIMIT 1",
        user_id,
        week_start);
    out["summary"] = report_rows.empty() || report_rows[0]["report_text"].isNull()
        ? ""
        : report_rows[0]["report_text"].as<std::string>();

    return out;
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
