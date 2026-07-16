#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

/**
 * 에이전트(POST /goal-coaching/v1/reports)에 목표 코칭 생성을 요청하고,
 * 결과를 goal_coaching_report + goal_recommendation 테이블에 저장한다.
 *
 * insight_generator.cpp의 generateAndPersistInsights()와 같은 모양(같은 goal_id+date
 * 기존 행은 삭제 후 insert)이지만 완전히 별도 함수다 — insight 테이블/그 파이프라인은
 * 이 기능 때문에 전혀 건드리지 않는다(동료가 insight 의존 기능을 동시에 작업 중이라
 * insight 쪽 스키마·코드는 무충돌로 유지하기로 결정함).
 *
 * embed=false 로 요청한다 — insight/power 와 같은 이유로, vec_* 임베딩 저장소가
 * 아직 없어서 받아도 버려질 뿐이다.
 *
 * 성공하면 응답 API 모양 그대로(`{periodStart, pastSummary, projection,
 * projectedMetrics, recommendations}`)를 돌려준다 — goals_controller 가 바로
 * HTTP 응답으로 쓸 수 있게.
 */
std::optional<Json::Value> generateGoalCoaching(
    const db::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    int64_t goal_id,
    const std::string& goal_title,
    const std::string& category,
    const std::string& date,
    std::string& out_error);

/** 캐시된(이미 오늘자로 생성된) goal_coaching_report + goal_recommendation 을 읽어서
 * generateGoalCoaching() 과 같은 응답 모양으로 돌려준다. 없으면 nullopt. */
std::optional<Json::Value> readCachedGoalCoaching(
    const db::DbClientPtr& client,
    int64_t goal_id,
    const std::string& date);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
