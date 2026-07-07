#pragma once

#include <cstdint>
#include <string>

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

private:
    IotStore& m_iot;

    static int stepSecondsForRange(const std::string& range);
    static int pointCountForRange(const std::string& range);
    static std::string formatAgoLabel(int seconds_ago);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
