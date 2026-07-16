#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class SleepStore
{
public:
    explicit SleepStore(db::DbClientPtr client);

    Json::Value getTodaySummary(int64_t user_id) const;
    Json::Value getTodayPlan(int64_t user_id) const;
    Json::Value getTodayPhoneUsage() const;
    Json::Value getTodayAutomationSummary(int64_t user_id) const;
    Json::Value getDailySessions(int64_t user_id, const std::string& date) const;
    Json::Value getDailyReport(int64_t user_id, const std::string& date, const std::string& session_id) const;
    Json::Value getWeeklyReport(int64_t user_id, const std::string& week_start) const;

private:
    static std::string toIsoKst(const std::string& timestamp);
    static int computeScore(double efficiency);
    Json::Value buildHypnogram(const drogon::orm::Result& stats, const std::string& start, const std::string& end) const;

    db::DbClientPtr m_client;
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
