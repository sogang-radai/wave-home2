#include "habit_generator.h"

#include <algorithm>
#include <optional>
#include <unordered_map>

#include "../../core/json.h"
#include "../../core/logger.h"
#include "util/time_util.h"
#include "agent_client.h"
#include "habit_event_catalog.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr int kWindowDays = 14;
    constexpr int kMinRepeatDays = 5;

    bool isValidHabitType(const std::string& habit_type)
    {
        return habit_type == "sleep" || habit_type == "power"
            || habit_type == "gesture" || habit_type == "lifestyle";
    }

    struct ExistingHabit
    {
        int64_t id = 0;
        std::string title;
        std::string event;
    };

    std::vector<ExistingHabit> loadActiveHabits(const db::DbClientPtr& client, int64_t user_id)
    {
        std::vector<ExistingHabit> out;
        const auto rows = client->execSqlSync(
            "SELECT id, title, evidence_json FROM user_habit WHERE user_id = ? AND status = 'active'",
            user_id);
        for (const auto& row : rows)
        {
            ExistingHabit habit;
            habit.id = row["id"].as<int64_t>();
            habit.title = row["title"].as<std::string>();
            try
            {
                const auto evidence = json::parse(row["evidence_json"].as<std::string>());
                if (evidence.contains("event") && evidence["event"].is_string())
                    habit.event = evidence["event"].get<std::string>();
            }
            catch (const std::exception&)
            {
                // Malformed evidence_json — still list the habit (by title) so the LLM
                // doesn't propose an exact duplicate, just without an `event` hint.
            }
            out.push_back(std::move(habit));
        }
        return out;
    }
}

bool generateAndPersistHabitsForUser(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& for_date,
    std::string& out_error)
{
    if (!client)
    {
        out_error = "no db client";
        return false;
    }

    const auto candidates = extractHabitCandidates(client, user_id, kWindowDays, for_date, kMinRepeatDays);
    if (candidates.empty())
        return true; // nothing new to discover today — not an error

    std::unordered_map<std::string, HabitCandidate> candidate_by_event;
    json candidates_json = json::array();
    for (const auto& c : candidates)
    {
        candidate_by_event[c.event] = c;
        candidates_json.push_back(json{
            {"event", c.event},
            {"label", c.label},
            {"deviceName", c.deviceName},
            {"days", c.matchedDays},
            {"window", c.windowDays},
        });
    }

    const auto existing_habits = loadActiveHabits(client, user_id);
    json existing_habits_json = json::array();
    for (const auto& h : existing_habits)
        existing_habits_json.push_back(json{{"id", h.id}, {"title", h.title}, {"event", h.event}});

    json body;
    body["userId"] = user_id;
    body["date"] = for_date;
    body["candidates"] = candidates_json;
    body["existingHabits"] = existing_habits_json;

    AgentHabitJobResult result;
    if (runHabitJobSync(agent_base_url, body, result, out_error) != AgentClientResult::success)
    {
        WLOG_WARN("habit builder job failed (user {}, date {}): {}", user_id, for_date, out_error);
        return false;
    }

    std::unordered_map<int64_t, std::string> existing_ids;
    for (const auto& h : existing_habits)
        existing_ids[h.id] = h.title;

    try
    {
        const auto now = formatTimestamp();

        for (const auto& item : result.items)
        {
            const std::string event = item.value("event", std::string());
            const auto candidate_it = candidate_by_event.find(event);
            if (candidate_it == candidate_by_event.end())
            {
                // The LLM only ever sees the candidates we sent — a returned
                // event key that isn't one of them is either a hallucination
                // or a stale/mismatched response. Never persist on it.
                WLOG_WARN(
                    "habit builder: rejecting item with unverified event '{}' (user {}, date {})",
                    event,
                    user_id,
                    for_date);
                continue;
            }
            const auto& candidate = candidate_it->second;

            std::string habit_type = item.value("habitType", std::string("lifestyle"));
            if (!isValidHabitType(habit_type))
            {
                WLOG_WARN(
                    "habit builder: rejecting item with invalid habitType '{}' (user {}, event {})",
                    habit_type,
                    user_id,
                    event);
                continue;
            }

            const std::string title = item.value("title", std::string());
            const std::string description = item.value("description", std::string());
            if (title.empty() || description.empty())
                continue;

            const double confidence = candidate.windowDays > 0
                ? std::clamp(static_cast<double>(candidate.matchedDays) / candidate.windowDays, 0.0, 1.0)
                : 0.0;

            json evidence;
            evidence["event"] = candidate.event;
            evidence["days"] = candidate.matchedDays;
            evidence["window"] = candidate.windowDays;
            const std::string evidence_json = evidence.dump();

            std::optional<int64_t> existing_habit_id;
            if (item.contains("existingHabitId") && item["existingHabitId"].is_number_integer())
            {
                const int64_t candidate_id = item["existingHabitId"].get<int64_t>();
                if (existing_ids.count(candidate_id))
                    existing_habit_id = candidate_id;
            }

            if (existing_habit_id)
            {
                client->execSqlSync(
                    R"SQL(
UPDATE user_habit SET
    habit_type = ?, title = ?, description = ?, evidence_json = ?,
    confidence = ?, window_days = ?, last_verified_at = ?, status = 'active', updated_at = ?
WHERE id = ? AND user_id = ?
)SQL",
                    habit_type, title, description, evidence_json,
                    confidence, candidate.windowDays, now, now,
                    *existing_habit_id, user_id);
            }
            else
            {
                const auto id_rows = client->execSqlSync("SELECT COALESCE(MAX(id), 0) AS max_id FROM user_habit");
                const int64_t next_id = (id_rows.empty() ? 0 : id_rows[0]["max_id"].as<int64_t>()) + 1;

                client->execSqlSync(
                    R"SQL(
INSERT INTO user_habit (
    id, user_id, habit_type, title, description, evidence_json,
    confidence, window_days, valid_from, last_verified_at, status, created_at, updated_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'active', ?, ?)
)SQL",
                    next_id, user_id, habit_type, title, description, evidence_json,
                    confidence, candidate.windowDays, for_date, now, now, now);
            }
        }

        WLOG_INFO(
            "habit builder persisted from {} candidate(s), {} proposed (user {}, date {})",
            candidates.size(),
            result.items.size(),
            user_id,
            for_date);
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        WLOG_WARN("habit persist failed (user {}, date {}): {}", user_id, for_date, out_error);
        return false;
    }

    return true;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
