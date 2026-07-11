#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class InsightsStore
{
public:
    explicit InsightsStore(drogon::orm::DbClientPtr client);

    static std::string referenceDate(drogon::orm::DbClientPtr client);

    Json::Value list(
        int64_t user_id,
        const std::optional<std::string>& surface,
        const std::optional<std::string>& date,
        const std::optional<std::string>& kind,
        const std::optional<bool>& approved,
        const std::optional<bool>& actionable) const;

    Json::Value getById(int64_t user_id, int64_t insight_id) const;

    /** dashboard_banner → { headline, body } or null */
    Json::Value dashboardDailyMessage(int64_t user_id) const;

    /** approved=1 로 전환. rule_json_override 가 있으면 rule_json 컬럼도 함께 덮어쓴다(생성된 rule id 기록용). */
    bool markApplied(int64_t user_id, int64_t insight_id, const std::optional<Json::Value>& rule_json_override) const;

    /** approved=0 으로 되돌린다 (apply 취소). */
    bool markCanceled(int64_t user_id, int64_t insight_id) const;

private:
    drogon::orm::DbClientPtr m_client;

    Json::Value rowToJson(const drogon::orm::Row& row) const;
    static Json::Value parseJsonColumn(const drogon::orm::Field& field);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
