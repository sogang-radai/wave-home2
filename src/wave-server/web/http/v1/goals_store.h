#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

/**
 * `goal`/`goal_recommendation` 테이블 CRUD. goal_coaching_report 읽기/쓰기는
 * service/agent/goal_coaching_generator.h 쪽에 있다(에이전트 호출을 끼고 있어서 여기 두지 않음).
 *
 * insight/schedule_task 파이프라인과 완전히 독립 — 이 파일은 그쪽 테이블을 전혀 건드리지
 * 않는다.
 */
class GoalsStore
{
public:
    explicit GoalsStore(db::DbClientPtr client);

    Json::Value list(int64_t user_id, const std::optional<std::string>& status) const;
    Json::Value getById(int64_t user_id, int64_t goal_id) const;
    std::optional<Json::Value> create(
        int64_t user_id,
        const std::string& title,
        const std::string& category,
        std::string& error,
        std::string& field) const;
    std::optional<Json::Value> updateStatus(
        int64_t user_id,
        int64_t goal_id,
        const std::string& status,
        std::string& error) const;

    /** 새 목표 생성 전에 호출 — 기존 활성 목표를 전부 archived 로 바꾼다(사용자당 활성 목표 1개 제약). */
    void archiveActiveGoals(int64_t user_id) const;

    Json::Value getRecommendation(int64_t user_id, int64_t goal_id, int64_t recommendation_id) const;

    /**
     * approved=1 로 전환. rule_json_override/schedule_task_json_override 가 있으면 해당
     * 컬럼도 함께 덮어쓴다 — 파생된 automation_rule id/schedule_task id를 그 안에 기록해둬야
     * 취소(cancel) 시 무엇을 지울지 알 수 있다(schedule_task 에는 goal_recommendation 을
     * 가리키는 역참조 컬럼을 새로 만들지 않고, insight의 automation_rule id 기록 방식과
     * 동일하게 이 테이블 자신에 순참조로 남기는 방식을 택함).
     */
    bool markRecommendationApplied(
        int64_t recommendation_id,
        const std::optional<Json::Value>& rule_json_override,
        const std::optional<Json::Value>& schedule_task_json_override) const;

    /** approved=0 으로 되돌린다 (apply 취소). */
    bool markRecommendationCanceled(int64_t recommendation_id) const;

private:
    db::DbClientPtr m_client;

    Json::Value rowToJson(const drogon::orm::Row& row) const;
    Json::Value recommendationRowToJson(const drogon::orm::Row& row) const;
    int64_t nextGoalId() const;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
