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
        std::string stage = "awake";
        if (!row["status_ratio"].isNull())
        {
            Json::Value ratio;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream stream(row["status_ratio"].as<std::string>());
            if (Json::parseFromStream(builder, stream, &ratio, &errors))
            {
                const double asleep = ratio.get("asleep", 0.0).asDouble();
                const double absent = ratio.get("absent", 0.0).asDouble();
                if (asleep >= 0.5)
                    stage = "light";
                else if (absent >= 0.5)
                    stage = "awake";
                else
                    stage = "awake";
            }
        }
        segment["stage"] = stage;
        segment["durationMinutes"] = 1;
        segments.append(segment);

        const double toss = row["toss_mean"].isNull() ? 0.0 : row["toss_mean"].as<double>();
        movement.append(static_cast<int>(std::round(std::min(1.0, toss) * 100.0)));
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
    out["stageBreakdown"] = Json::Value(Json::arrayValue);

    out["hypnogram"] = buildHypnogram(stat_rows, onset, final_wake);
    out["stageLog"] = Json::Value(Json::arrayValue);
    out["snoringEpisodes"] = Json::Value(Json::arrayValue);
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
    auto week_end_rows = m_client->execSqlSync(
        "SELECT date(?, '+7 day') AS week_end",
        week_start);
    const std::string week_end = week_end_rows.empty()
        ? week_start
        : week_end_rows[0]["week_end"].as<std::string>();

    auto session_rows = m_client->execSqlSync(
        "SELECT * FROM sleep_session WHERE user_id = ? AND night_date >= ? AND night_date < ? ORDER BY night_date",
        user_id,
        week_start,
        week_end);

    Json::Value out;
    out["weekStart"] = week_start;
    out["weekEnd"] = week_end.substr(0, 10);

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
        ? "이번 주 수면 데이터를 기반으로 한 주간 요약입니다."
        : report_rows[0]["report_text"].as<std::string>();

    return out;
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
