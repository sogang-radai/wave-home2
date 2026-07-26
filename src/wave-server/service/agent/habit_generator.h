#pragma once

#include <cstdint>
#include <string>

#include "../../db/database.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

/**
 * Runs Step 5's deterministic candidate extraction for `user_id`, sends the
 * resulting candidate list + existing active habits inline to the agent
 * (POST /insight/v1/habits — no gather/tool-loop on the agent side, no
 * AgentJobQueue on this side, mirroring how sleep/power report generation
 * receive metrics inline rather than self-serving raw tables), then
 * verifies each proposed habit's `event` key against the candidates that
 * were actually sent (rejecting anything the LLM didn't see) before
 * inserting/updating `user_habit`. `confidence` is always computed here from
 * the candidate's own recounted days/window — never from anything the LLM
 * stated.
 */
bool generateAndPersistHabitsForUser(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& for_date,
    std::string& out_error);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
