#include "sleep_plan_generator.h"

#include "../core/json.h"
#include "../core/logger.h"
#include "util/time_util.h"
#include "agent/agent_client.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    // sleep_store.cpp 의 SettingsStore::defaultSleepConfig() 기본값(dimStartMinutes=30,
    // acTemp=24)과 동일한 값 — 에이전트가 prepMinute/recommendedTempC 를 생략했을 때만 쓴다.
    constexpr int kDefaultPrepBufferMinutes = 40;
    constexpr double kDefaultRecommendedTempC = 24.0;

    // strftime('%w', ...) 인덱스(0=일) 순서와 일치.
    const char* kWeekdayByDowIndex[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

    bool isValidMinuteOfDay(int minutes)
    {
        return minutes >= 0 && minutes < 24 * 60;
    }

    // app/schemas/sleep_analysis.py 의 SleepSessionRow 와 동일한 camelCase 필드로 채운다 -
    // 에이전트가 sleep/reports 에서 이미 이 모양을 소비하고 있어 재사용한다.
    json fetchRecentSessions(const db::DbClientPtr& client, int64_t user_id, int limit)
    {
        json out = json::array();
        const auto rows = client->execSqlSync(
            "SELECT id, user_id, room_id, radar_id, station_id, night_date, onset, final_wake,"
            " time_in_bed_s, asleep_total_s, efficiency, stage_totals, toss_events, hr_mean, br_mean, snore_ratio"
            " FROM sleep_session WHERE user_id = ? ORDER BY night_date DESC LIMIT ?",
            user_id,
            limit);

        for (const auto& row : rows)
        {
            json item;
            item["id"] = row["id"].as<int64_t>();
            item["userId"] = row["user_id"].as<int64_t>();
            item["roomId"] = row["room_id"].as<int64_t>();
            item["radarId"] = row["radar_id"].as<int64_t>();
            if (!row["station_id"].isNull())
                item["stationId"] = row["station_id"].as<int64_t>();
            item["nightDate"] = row["night_date"].as<std::string>();
            if (!row["onset"].isNull())
                item["onset"] = row["onset"].as<std::string>();
            if (!row["final_wake"].isNull())
                item["finalWake"] = row["final_wake"].as<std::string>();
            if (!row["time_in_bed_s"].isNull())
                item["timeInBedS"] = row["time_in_bed_s"].as<int64_t>();
            if (!row["asleep_total_s"].isNull())
                item["asleepTotalS"] = row["asleep_total_s"].as<int64_t>();
            if (!row["efficiency"].isNull())
                item["efficiency"] = row["efficiency"].as<double>();
            if (!row["stage_totals"].isNull())
            {
                try
                {
                    item["stageTotals"] = json::parse(row["stage_totals"].as<std::string>());
                }
                catch (const std::exception&)
                {
                    // 파싱 실패 시 그냥 생략 - 에이전트 프롬프트에서 필수 필드가 아니다.
                }
            }
            if (!row["toss_events"].isNull())
                item["tossEvents"] = row["toss_events"].as<int64_t>();
            if (!row["hr_mean"].isNull())
                item["hrMean"] = row["hr_mean"].as<double>();
            if (!row["br_mean"].isNull())
                item["brMean"] = row["br_mean"].as<double>();
            if (!row["snore_ratio"].isNull())
                item["snoreRatio"] = row["snore_ratio"].as<double>();
            out.push_back(std::move(item));
        }
        return out;
    }

    // app/tools/schedule_tasks_internal.py 의 ScheduleTask 와 동일한 camelCase 필드.
    // sleep_store.cpp(구 rule-based 로직)와 동일한 규칙으로 once/weekly 일정을 함께 조회한다:
    // schedule_kind='once'면 event_date 일치, 'weekly'면 그 날짜의 요일이 day_of_week 와 일치.
    json fetchScheduleForDate(const db::DbClientPtr& client, int64_t user_id, const std::string& date)
    {
        const auto wday_rows = client->execSqlSync("SELECT CAST(strftime('%w', ?) AS INTEGER) AS w", date);
        const int wday_index = wday_rows.empty() ? 0 : (((wday_rows[0]["w"].as<int>() % 7) + 7) % 7);
        const std::string weekday = kWeekdayByDowIndex[wday_index];

        json out = json::array();
        const auto rows = client->execSqlSync(
            R"SQL(
SELECT id, user_id, title, created_at, created_by, category, schedule_kind, day_of_week,
       event_date, start_minute, end_minute, done, source_insight_id
FROM schedule_task
WHERE user_id = ?
  AND (
    (schedule_kind = 'once' AND event_date = ?)
    OR (schedule_kind = 'weekly' AND day_of_week = ?)
  )
ORDER BY start_minute ASC
)SQL",
            user_id,
            date,
            weekday);

        for (const auto& row : rows)
        {
            json item;
            item["id"] = row["id"].as<int64_t>();
            item["userId"] = row["user_id"].as<int64_t>();
            item["title"] = row["title"].as<std::string>();
            if (!row["created_at"].isNull())
                item["createdAt"] = row["created_at"].as<std::string>();
            item["createdBy"] = row["created_by"].as<std::string>();
            item["category"] = row["category"].as<std::string>();
            item["scheduleKind"] = row["schedule_kind"].as<std::string>();
            item["dayOfWeek"] = row["day_of_week"].as<std::string>();
            if (!row["event_date"].isNull())
                item["eventDate"] = row["event_date"].as<std::string>();
            if (!row["start_minute"].isNull())
                item["startMinute"] = row["start_minute"].as<int>();
            if (!row["end_minute"].isNull())
                item["endMinute"] = row["end_minute"].as<int>();
            item["done"] = row["done"].as<int>() != 0;
            if (!row["source_insight_id"].isNull())
                item["sourceInsightId"] = row["source_insight_id"].as<int64_t>();
            out.push_back(std::move(item));
        }
        return out;
    }
}

bool generateAndPersistSleepPlan(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& plan_date,
    std::string& out_error)
{
    const auto tomorrow_rows = client->execSqlSync("SELECT date(?, '+1 day') AS d", plan_date);
    const std::string tomorrow_date = tomorrow_rows.empty() ? plan_date : tomorrow_rows[0]["d"].as<std::string>();

    json body;
    body["userId"] = user_id;
    body["planDate"] = plan_date;
    body["sessions"] = fetchRecentSessions(client, user_id, 7);
    body["todaySchedule"] = fetchScheduleForDate(client, user_id, plan_date);
    body["tomorrowSchedule"] = fetchScheduleForDate(client, user_id, tomorrow_date);
    body["embed"] = false;

    AgentSleepJobResult result;
    if (runSleepJobSync(agent_base_url, "/sleep/v1/plans", body, result, out_error) != AgentClientResult::success)
    {
        WLOG_WARN("sleep plan job failed (user {}, date {}): {}", user_id, plan_date, out_error);
        return false;
    }

    // app/schemas/sleep_plan.py: reportText 는 SleepPlanContent 를 json.dumps 한 문자열이다
    // (sleep 요약/리포트 job 의 "reportText = 평문" 계약 위에 구조화된 값을 실어 보낸 것).
    json content;
    try
    {
        content = json::parse(result.text);
    }
    catch (const std::exception& e)
    {
        out_error = std::string("invalid plan JSON: ") + e.what();
        WLOG_WARN("sleep plan parse failed (user {}, date {}): {}", user_id, plan_date, out_error);
        return false;
    }

    if (!content.contains("bedtimeMinute") || !content["bedtimeMinute"].is_number_integer()
        || !content.contains("wakeMinute") || !content["wakeMinute"].is_number_integer()
        || !content.contains("targetDurationMinutes") || !content["targetDurationMinutes"].is_number_integer()
        || !content.contains("rationale") || !content["rationale"].is_string())
    {
        out_error = "plan missing required fields";
        WLOG_WARN("sleep plan validation failed (user {}, date {}): {}", user_id, plan_date, out_error);
        return false;
    }

    const int bedtime_min = content["bedtimeMinute"].get<int>();
    const int wake_min = content["wakeMinute"].get<int>();
    if (!isValidMinuteOfDay(bedtime_min) || !isValidMinuteOfDay(wake_min))
    {
        out_error = "plan minute-of-day out of range";
        WLOG_WARN("sleep plan validation failed (user {}, date {}): {}", user_id, plan_date, out_error);
        return false;
    }

    const int target_duration_min = content["targetDurationMinutes"].get<int>();
    const int prep_min = content.contains("prepMinute") && content["prepMinute"].is_number_integer()
        ? content["prepMinute"].get<int>()
        : bedtime_min - kDefaultPrepBufferMinutes;
    const double recommended_temp = content.contains("recommendedTempC") && content["recommendedTempC"].is_number()
        ? content["recommendedTempC"].get<double>()
        : kDefaultRecommendedTempC;
    const std::string rationale = content["rationale"].get<std::string>();

    try
    {
        // 문서 규칙(insight 와 동일): 동일 userId+planDate 기존 행은 삭제 후 insert.
        client->execSqlSync(
            "DELETE FROM sleep_plan WHERE user_id = ? AND plan_date = ?",
            user_id,
            plan_date);

        const auto id_rows = client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM sleep_plan");
        const int64_t plan_id = id_rows.empty() ? 1 : id_rows[0]["next_id"].as<int64_t>();

        client->execSqlSync(
            R"SQL(
INSERT INTO sleep_plan (
    id, user_id, plan_date, bedtime_minute, wake_minute, prep_minute,
    recommended_temp_c, target_duration_minutes, rationale_text, created_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
            plan_id,
            user_id,
            plan_date,
            bedtime_min,
            wake_min,
            prep_min,
            recommended_temp,
            target_duration_min,
            rationale,
            formatTimestamp());
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        WLOG_WARN("sleep plan persist failed (user {}, date {}): {}", user_id, plan_date, out_error);
        return false;
    }

    WLOG_INFO("sleep plan generated (user {}, date {})", user_id, plan_date);
    return true;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
