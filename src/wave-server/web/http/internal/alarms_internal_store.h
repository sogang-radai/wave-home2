#pragma once

#include <optional>
#include <string>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {

struct AlarmListFilter
{
    int64_t user_id = 0;
    std::optional<bool> enabled;
};

class AlarmsInternalStore
{
public:
    explicit AlarmsInternalStore(db::DbClientPtr client);

    Json::Value listAlarms(const AlarmListFilter& filter) const;
    Json::Value createAlarm(const Json::Value& body, std::string& error, std::string& field) const;
    Json::Value updateAlarm(
        int64_t user_id,
        int64_t alarm_id,
        const Json::Value& body,
        std::string& error,
        std::string& field) const;
    Json::Value deleteAlarm(int64_t user_id, int64_t alarm_id, std::string& error) const;

private:
    db::DbClientPtr m_client;

    Json::Value rowToJson(const drogon::orm::Row& row) const;
    std::optional<int64_t> resolveInternalDeviceId(const std::string& external_id) const;
    std::string externalDeviceId(int64_t internal_id) const;
    static std::string now_stamp();
    static bool validate_payload(const Json::Value& body, bool partial, std::string& error, std::string& field);
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
