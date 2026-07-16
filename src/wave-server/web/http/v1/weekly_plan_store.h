#pragma once

#include <cstdint>
#include <string>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class WeeklyPlanStore
{
public:
    explicit WeeklyPlanStore(db::DbClientPtr client);

    Json::Value weeklyReport(int64_t user_id, const std::string& period_start) const;

private:
    db::DbClientPtr m_client;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
