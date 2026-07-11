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

/**
 * user_action_log 적재. insight 승인/적용, schedule_task 완료 등
 * "언제 실행했는지" 이력이 필요한 상태 전이 지점에서 호출한다.
 */
class ActionLogStore
{
public:
    explicit ActionLogStore(drogon::orm::DbClientPtr client);

    void record(
        int64_t user_id,
        const std::string& action_type,
        const std::string& ref_type,
        int64_t ref_id,
        const std::optional<std::string>& category = std::nullopt,
        const std::optional<Json::Value>& metadata = std::nullopt) const;

private:
    drogon::orm::DbClientPtr m_client;

    int64_t nextId() const;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
