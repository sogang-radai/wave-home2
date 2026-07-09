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

class IotStore;

class PowerStore
{
public:
    explicit PowerStore(IotStore& iot);

    Json::Value listPlugs();
    Json::Value comboTrend(const std::string& device_id, const std::string& range, const std::string& metric);

    static Json::Value periodTrend(
        drogon::orm::DbClientPtr client,
        const std::string& device_external_id,
        const std::string& ui_period,
        const std::string& ref_date_hint);

    static Json::Value queryReport(
        drogon::orm::DbClientPtr client,
        const std::string& device_external_id,
        const std::string& ui_period,
        const std::string& period_start_hint);

private:
    IotStore& m_iot;

    static int stepSecondsForRange(const std::string& range);
    static int pointCountForRange(const std::string& range);
    static std::string formatAgoLabel(int seconds_ago);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
