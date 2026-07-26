#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../core/coredefs.h"
#include "../../db/database.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

// A "habit event key" is a semantic, schema-decoupled identifier for a
// recurring signal — e.g. "home.philips_wiz_e29.off" or "schedule.posture" —
// stored verbatim on user_habit.evidence_json so that changes to the
// underlying home_event/schedule_task shape only require updating the lookup
// logic here, not every stored habit row. Two sources are recognized:
//   - "home.<device_class>.<action>[.<bucket>]" — home_event device actions
//     (bucketed low/mid/high for numeric actions like brightness).
//   - "schedule.<category>" — schedule_task completions grouped by category
//     (posture/diet/mental/sleep/life), counted via their completion history
//     in user_action_log (action_type='schedule_task_completed'), NOT
//     schedule_task.done (that's a live checkbox with no per-day history —
//     the completion *log* is what lets us count distinct days). Grouped by
//     category rather than exact title because real schedule_task rows use a
//     different worded title per day_of_week for the same routine (e.g.
//     Monday "기상 후 목 스트레칭 20초" vs Tuesday "어깨 스트레칭 10분" are
//     both category='posture') — title-grouping would never accumulate
//     enough same-title days to clear the repetition floor. This is what
//     lets routine/lifestyle habits surface at all, since they never touch
//     home_event.
// Sleep/gesture-derived keys can be added the same way later.
struct HabitCandidate
{
    std::string event;
    std::string label;       // short Korean hint for the LLM, e.g. "조명 끄기"
    std::string deviceName;  // best-effort device display name, may be empty
    int matchedDays = 0;
    int windowDays = 0;
};

/** Recount how many distinct days in the trailing `window_days` (ending at
 *  `for_date`, inclusive) the given event key actually occurred for this
 *  user. Returns matchedDays=0 for an unrecognized/malformed key rather than
 *  throwing — callers (confidence refresh) treat that as "no longer
 *  matches," which naturally drives a habit toward expiry instead of
 *  crashing when the catalog's key format evolves. */
HabitCandidate countEventOccurrences(
    const db::DbClientPtr& client,
    int64_t user_id,
    const std::string& event_key,
    int window_days,
    const std::string& for_date);

/** Scan home_event (device.class/action/bucket) AND schedule_task completion
 *  history (via user_action_log) for every distinct signal actually observed
 *  for `user_id` in the trailing window, count matched days for each, and
 *  keep entries with matchedDays >= min_repeat_days — excluding any key
 *  already covered by an existing active user_habit row. This is the full
 *  candidate list handed to the Habit Builder LLM call. */
std::vector<HabitCandidate> extractHabitCandidates(
    const db::DbClientPtr& client,
    int64_t user_id,
    int window_days,
    const std::string& for_date,
    int min_repeat_days = 5);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
