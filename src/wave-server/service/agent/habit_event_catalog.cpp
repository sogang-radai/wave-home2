#include "habit_event_catalog.h"

#include <algorithm>
#include <optional>

#include "../../core/json.h"
#include "../../core/logger.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    struct ParsedEventKey
    {
        bool valid = false;
        std::string deviceClass;
        std::string action;
        std::string bucket; // e.g. "low"/"mid"/"high" for numeric actions; empty otherwise
    };

    // "home.<device_class>.<action>[.<bucket>]" is the only key shape v1
    // recognizes (home_event type='execution' rows). Unrecognized shapes
    // return {valid=false} rather than throwing.
    ParsedEventKey parseHomeEventKey(const std::string& event_key)
    {
        ParsedEventKey out;
        static const std::string kPrefix = "home.";
        if (event_key.rfind(kPrefix, 0) != 0)
            return out;

        const std::string rest = event_key.substr(kPrefix.size());
        const auto first_dot = rest.find('.');
        if (first_dot == std::string::npos)
            return out;

        out.deviceClass = rest.substr(0, first_dot);
        const std::string action_and_bucket = rest.substr(first_dot + 1);
        const auto second_dot = action_and_bucket.find('.');
        if (second_dot == std::string::npos)
            out.action = action_and_bucket;
        else
        {
            out.action = action_and_bucket.substr(0, second_dot);
            out.bucket = action_and_bucket.substr(second_dot + 1);
        }

        out.valid = !out.deviceClass.empty() && !out.action.empty();
        return out;
    }

    std::string makeHomeEventKey(
        const std::string& device_class,
        const std::string& action,
        const std::string& bucket = {})
    {
        std::string key = "home." + device_class + "." + action;
        if (!bucket.empty())
            key += "." + bucket;
        return key;
    }

    // "schedule.<category>" — grouped by schedule_task.category (posture/diet/
    // mental/sleep/life), NOT by exact title: real schedule_task rows use a
    // different worded title per day_of_week even for the same routine (e.g.
    // Monday "기상 후 목 스트레칭 20초" vs Tuesday "어깨 스트레칭 10분" are both
    // `category='posture'`) — grouping by title would never accumulate enough
    // same-title days to clear min_repeat_days, so category is what actually
    // captures "이 사용자는 자세 루틴을 꾸준히 완료한다"-style consistency.
    const std::string kScheduleKeyPrefix = "schedule.";

    std::string makeScheduleKey(const std::string& category)
    {
        return kScheduleKeyPrefix + category;
    }

    // Returns the category if `event_key` is a schedule-sourced key, else nullopt.
    std::optional<std::string> parseScheduleKey(const std::string& event_key)
    {
        if (event_key.rfind(kScheduleKeyPrefix, 0) != 0)
            return std::nullopt;
        return event_key.substr(kScheduleKeyPrefix.size());
    }

    // Matches CATEGORY_TO_KOREAN in wave-home-front/src/App.js — same taxonomy,
    // just also needed server-side for the LLM-facing candidate label.
    std::string scheduleCategoryLabel(const std::string& category)
    {
        if (category == "posture") return "자세 관리 루틴";
        if (category == "sleep") return "수면 준비 루틴";
        if (category == "diet") return "식습관 관리 루틴";
        if (category == "mental") return "멘탈 관리 루틴";
        if (category == "life") return "생활 루틴";
        return category + " 루틴";
    }

    // Actions whose value is a continuous number rather than a discrete
    // state — bucketed into low/mid/high so "브라이트니스를 30으로" and
    // "31로" count as the same recurring behavior instead of splitting the
    // signal across near-infinite distinct exact values.
    bool isBucketedAction(const std::string& action)
    {
        return action == "brightness";
    }

    std::string bucketLabel(double value)
    {
        if (value <= 30.0)
            return "low";
        if (value <= 70.0)
            return "mid";
        return "high";
    }

    std::string windowStartModifier(int window_days)
    {
        return "-" + std::to_string(std::max(0, window_days - 1)) + " day";
    }

    // Best-effort Korean label for a (class, action[, bucket]) triple — falls
    // back to the raw technical key when nothing more specific is known. The
    // LLM only needs enough context to write a natural title; it never sees
    // the underlying SQL shape.
    std::string humanLabelFor(const std::string& deviceClass, const std::string& action, const std::string& bucket)
    {
        // philips_wiz_e29 is a class-name PREFIX in this codebase (device_manager.cpp
        // dispatches "philips_wiz_e29_white"/"philips_wiz_e29_color"/... to the same
        // driver via rfind(..., 0) == 0) — match the same way here rather than exact
        // equality, or seeded/real light devices never get a nice label.
        if (deviceClass.rfind("philips_wiz_e29", 0) == 0)
        {
            if (action == "off") return "조명 끄기";
            if (action == "on") return "조명 켜기";
            if (action == "brightness")
            {
                if (bucket == "low") return "조명 밝기 낮게";
                if (bucket == "high") return "조명 밝기 높게";
                return "조명 밝기 중간";
            }
        }
        if (deviceClass == "samsung_g7")
        {
            if (action == "power_toggle" || action == "off") return "TV 끄기";
            if (action == "on") return "TV 켜기";
        }
        if (deviceClass == "tuya_ep2h")
        {
            if (action == "off") return "플러그 끄기";
            if (action == "on") return "플러그 켜기";
        }
        return deviceClass + " " + action;
    }
}

HabitCandidate countEventOccurrences(
    const db::DbClientPtr& client,
    int64_t user_id,
    const std::string& event_key,
    int window_days,
    const std::string& for_date)
{
    HabitCandidate result;
    result.event = event_key;
    result.windowDays = window_days;
    if (!client)
        return result;

    if (const auto category = parseScheduleKey(event_key))
    {
        result.label = scheduleCategoryLabel(*category);
        try
        {
            const auto rows = client->execSqlSync(
                R"SQL(
SELECT COUNT(DISTINCT substr(ual.occurred_at, 1, 10)) AS matched_days
FROM user_action_log ual
JOIN schedule_task st ON st.id = ual.ref_id
WHERE ual.user_id = ?
  AND ual.ref_type = 'schedule_task'
  AND ual.action_type = 'schedule_task_completed'
  AND st.category = ?
  AND substr(ual.occurred_at, 1, 10) >= date(?, ?)
  AND substr(ual.occurred_at, 1, 10) <= ?
)SQL",
                user_id, *category, for_date, windowStartModifier(window_days), for_date);
            if (!rows.empty() && !rows[0]["matched_days"].isNull())
                result.matchedDays = rows[0]["matched_days"].as<int>();
        }
        catch (const std::exception& e)
        {
            WLOG_WARN("countEventOccurrences: schedule query failed for key {}: {}", event_key, e.what());
        }
        return result;
    }

    const auto parsed = parseHomeEventKey(event_key);
    if (!parsed.valid)
        return result;

    result.label = humanLabelFor(parsed.deviceClass, parsed.action, parsed.bucket);

    try
    {
        if (parsed.bucket.empty())
        {
            const auto rows = client->execSqlSync(
                R"SQL(
SELECT COUNT(DISTINCT substr(he.occurred_at, 1, 10)) AS matched_days
FROM home_event he
JOIN device d ON d.id = he.device_id
WHERE he.user_id = ?
  AND he.type = 'execution'
  AND d.class = ?
  AND json_extract(he.detail_json, '$.action') = ?
  AND substr(he.occurred_at, 1, 10) >= date(?, ?)
  AND substr(he.occurred_at, 1, 10) <= ?
)SQL",
                user_id, parsed.deviceClass, parsed.action,
                for_date, windowStartModifier(window_days), for_date);
            if (!rows.empty() && !rows[0]["matched_days"].isNull())
                result.matchedDays = rows[0]["matched_days"].as<int>();
        }
        else
        {
            const auto rows = client->execSqlSync(
                R"SQL(
SELECT COUNT(DISTINCT substr(he.occurred_at, 1, 10)) AS matched_days
FROM home_event he
JOIN device d ON d.id = he.device_id
WHERE he.user_id = ?
  AND he.type = 'execution'
  AND d.class = ?
  AND json_extract(he.detail_json, '$.action') = ?
  AND CASE
        WHEN CAST(json_extract(he.detail_json, '$.params.value') AS REAL) <= 30 THEN 'low'
        WHEN CAST(json_extract(he.detail_json, '$.params.value') AS REAL) <= 70 THEN 'mid'
        ELSE 'high'
      END = ?
  AND substr(he.occurred_at, 1, 10) >= date(?, ?)
  AND substr(he.occurred_at, 1, 10) <= ?
)SQL",
                user_id, parsed.deviceClass, parsed.action, parsed.bucket,
                for_date, windowStartModifier(window_days), for_date);
            if (!rows.empty() && !rows[0]["matched_days"].isNull())
                result.matchedDays = rows[0]["matched_days"].as<int>();
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("countEventOccurrences: query failed for key {}: {}", event_key, e.what());
    }

    return result;
}

std::vector<HabitCandidate> extractHabitCandidates(
    const db::DbClientPtr& client,
    int64_t user_id,
    int window_days,
    const std::string& for_date,
    int min_repeat_days)
{
    std::vector<HabitCandidate> candidates;
    if (!client)
        return candidates;

    try
    {
        // Discover every distinct (class, action, numeric-value) triple this
        // user actually produced in the window, bucketing numeric actions in
        // SQL up front so grouping collapses e.g. brightness=28 and
        // brightness=31 into the same "low" candidate.
        const auto rows = client->execSqlSync(
            R"SQL(
SELECT
    d.class AS device_class,
    json_extract(he.detail_json, '$.action') AS action,
    CASE
        WHEN json_extract(he.detail_json, '$.action') = 'brightness'
             AND json_extract(he.detail_json, '$.params.value') IS NOT NULL
        THEN CASE
                WHEN CAST(json_extract(he.detail_json, '$.params.value') AS REAL) <= 30 THEN 'low'
                WHEN CAST(json_extract(he.detail_json, '$.params.value') AS REAL) <= 70 THEN 'mid'
                ELSE 'high'
             END
        ELSE ''
    END AS bucket,
    MAX(d.name) AS device_name
FROM home_event he
JOIN device d ON d.id = he.device_id
WHERE he.user_id = ?
  AND he.type = 'execution'
  AND json_extract(he.detail_json, '$.action') IS NOT NULL
  AND substr(he.occurred_at, 1, 10) >= date(?, ?)
  AND substr(he.occurred_at, 1, 10) <= ?
GROUP BY d.class, action, bucket
)SQL",
            user_id, for_date, windowStartModifier(window_days), for_date);

        std::vector<std::string> seen_keys;
        for (const auto& row : rows)
        {
            const auto device_class = row["device_class"].as<std::string>();
            const auto action = row["action"].as<std::string>();
            const auto bucket = row["bucket"].as<std::string>();
            const auto device_name = row["device_name"].isNull() ? std::string() : row["device_name"].as<std::string>();

            const auto event_key = makeHomeEventKey(device_class, action, bucket);
            auto candidate = countEventOccurrences(client, user_id, event_key, window_days, for_date);
            if (candidate.matchedDays < min_repeat_days)
                continue;
            candidate.deviceName = device_name;
            candidates.push_back(std::move(candidate));
        }

        // Same idea for recurring schedule_task routines, but counted via their
        // completion *log* (user_action_log.action_type='schedule_task_completed')
        // rather than schedule_task.done — done is a live checkbox with no
        // per-day history, so it can't tell us "completed on N distinct days".
        // Grouped by category, not title — see makeScheduleKey's comment.
        const auto schedule_rows = client->execSqlSync(
            R"SQL(
SELECT DISTINCT st.category AS category
FROM user_action_log ual
JOIN schedule_task st ON st.id = ual.ref_id
WHERE ual.user_id = ?
  AND ual.ref_type = 'schedule_task'
  AND ual.action_type = 'schedule_task_completed'
  AND substr(ual.occurred_at, 1, 10) >= date(?, ?)
  AND substr(ual.occurred_at, 1, 10) <= ?
)SQL",
            user_id, for_date, windowStartModifier(window_days), for_date);

        for (const auto& row : schedule_rows)
        {
            const auto category = row["category"].as<std::string>();
            const auto event_key = makeScheduleKey(category);
            auto candidate = countEventOccurrences(client, user_id, event_key, window_days, for_date);
            if (candidate.matchedDays < min_repeat_days)
                continue;
            candidates.push_back(std::move(candidate));
        }

        // Drop any candidate already covered by an existing active habit —
        // the LLM should only ever see genuinely new, undiscovered signals.
        const auto existing_rows = client->execSqlSync(
            "SELECT evidence_json FROM user_habit WHERE user_id = ? AND status = 'active'",
            user_id);
        std::vector<std::string> already_known;
        for (const auto& row : existing_rows)
        {
            try
            {
                const auto evidence = json::parse(row["evidence_json"].as<std::string>());
                if (evidence.contains("event") && evidence["event"].is_string())
                    already_known.push_back(evidence["event"].get<std::string>());
            }
            catch (const std::exception&)
            {
                // Malformed evidence_json on an existing row — skip rather than fail extraction.
            }
        }

        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [&already_known](const HabitCandidate& c)
                {
                    return std::find(already_known.begin(), already_known.end(), c.event) != already_known.end();
                }),
            candidates.end());
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("extractHabitCandidates: query failed for user {}: {}", user_id, e.what());
    }

    return candidates;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
