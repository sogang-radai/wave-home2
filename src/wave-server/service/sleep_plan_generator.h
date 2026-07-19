#pragma once

#include <cstdint>
#include <string>

#include <drogon/orm/DbClient.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

/**
 * 에이전트(POST /sleep/v1/plans)에 "오늘 밤 추천 수면 시간" 생성을 요청하고, 결과를
 * sleep_plan 테이블에 저장한다(동일 userId+planDate 기존 행은 삭제 후 insert —
 * insight_generator.cpp 와 동일 규칙).
 *
 * GET /api/v1/sleep/today/plan(sleep_store.cpp) 은 이 테이블을 읽기만 하고 직접 계산하지
 * 않는다 — 취침/기상 시각 판단은 항상 여기(에이전트, app/graph/sleep_plan_graph.py)에서
 * 이뤄진다. 이 함수는 SleepManager 의 작업 큐(비동기 워커 스레드)에서 호출되어, 실제 밤
 * 세션이 종료되거나(다음날 계획) 데모 세션이 시딩될 때 미리 생성해둔다 — GET 요청 경로에서는
 * 절대 호출하지 않는다(에이전트 응답 지연이 요청을 막지 않도록).
 */
bool generateAndPersistSleepPlan(
    const drogon::orm::DbClientPtr& client,
    const std::string& agent_base_url,
    int64_t user_id,
    const std::string& plan_date,
    std::string& out_error);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
