#pragma once

#include <cstdint>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class DashboardStore
{
public:
    explicit DashboardStore(drogon::orm::DbClientPtr client);

    Json::Value currentState() const;
    Json::Value upcomingAlarms(int64_t user_id) const;
    Json::Value upcomingAlarmsFromItems(const Json::Value& alarms) const;
    Json::Value activeGestureRules(int64_t user_id) const;

private:
    drogon::orm::DbClientPtr m_client;

    static Json::Value parseJsonColumn(const drogon::orm::Field& field);
    static Json::Value parseDaysJson(const std::string& raw);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
