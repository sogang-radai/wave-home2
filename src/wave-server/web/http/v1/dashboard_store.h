#pragma once

#include <cstdint>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class DashboardStore
{
public:
    explicit DashboardStore(db::DbClientPtr client);

    Json::Value currentState() const;
    Json::Value upcomingAlarms(int64_t user_id) const;
    Json::Value upcomingAlarmsFromItems(const Json::Value& alarms) const;
    Json::Value activeGestureRules(int64_t user_id) const;

private:
    db::DbClientPtr m_client;

    static Json::Value parse_json_column(const drogon::orm::Field& field);
    static Json::Value parse_days_json(const std::string& raw);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
