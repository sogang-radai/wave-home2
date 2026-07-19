#pragma once

#include <cstdint>
#include <string>

#include "../db/database.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

/**
 * 에이전트(POST /insight/v1/insights)에 인사이트 배치 생성을 요청하고, 결과를
 * insight 테이블에 저장한다 (agent-be/agent-api/insight-generation-api.md 의
 * "백엔드 저장 규칙": 동일 userId+surface+date 기존 행은 삭제 후 insert,
 * approved 는 항상 0).
 *
 * embed=true 로 요청하고, 각 item.embedding 을 surface 별 vec_insight_* 에 upsert 한다.
 */
bool generateAndPersistInsights(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& surface,
    const std::string& date,
    std::string& out_error);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
