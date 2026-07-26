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

class InsightsStore
{
public:
    explicit InsightsStore(db::DbClientPtr client);

    static std::string reference_date(db::DbClientPtr client);

    /** Highest-confidence active habit for `user_id`, shaped like dashboardDailyMessage's
     *  {headline, body} — Json::nullValue if none exist. Shared by the dashboard and
     *  weekly-plan banner reads as their no-synthesized-banner-yet fallback, so both
     *  prefer a stable, confidence-ranked pick over "whichever row happens to be most
     *  recent". `habit_type` restricts to one category (e.g. "lifestyle" for the
     *  weekly-plan fallback, to match generateAndPersistWeeklyPlanBanner's own scoping);
     *  empty considers any active habit. */
    static Json::Value bestActiveHabitBanner(
        const db::DbClientPtr& client,
        int64_t user_id,
        const std::string& habit_type = "");

    Json::Value list(
        int64_t user_id,
        const std::optional<std::string>& surface,
        const std::optional<std::string>& date,
        const std::optional<std::string>& kind,
        const std::optional<bool>& approved,
        const std::optional<bool>& actionable) const;

    Json::Value getById(int64_t user_id, int64_t insight_id) const;

    Json::Value dashboardDailyMessage(int64_t user_id) const;

    bool markApplied(int64_t user_id, int64_t insight_id, const std::optional<Json::Value>& rule_json_override) const;
    bool markCanceled(int64_t user_id, int64_t insight_id) const;

private:
    db::DbClientPtr m_client;

    Json::Value rowToJson(const drogon::orm::Row& row) const;
    static Json::Value parse_json_column(const drogon::orm::Field& field);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
