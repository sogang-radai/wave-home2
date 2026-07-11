#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

struct DemoPowerReading
{
    bool switch_on = false;
    double rated_w = 0;
    double power_w = 0;
    double voltage_v = 235.0;
    double current_ma = 0;
    int64_t ts_ms = 0;
};

// In-memory per-demo-session power tracker. Remembers switch/rated baselines
// from user actions and applies light jitter on ~5s sample intervals without
// writing to the database.
class DemoPowerMeter
{
public:
    static DemoPowerMeter& instance();

    void syncPlug(
        const std::string& runtime_id,
        const std::string& device_id,
        bool switch_on,
        double rated_w,
        double voltage_v = 235.0);

    DemoPowerReading samplePlug(
        const std::string& runtime_id,
        const std::string& device_id,
        bool switch_on,
        double rated_w,
        double voltage_v = 235.0);

    Json::Value listPlugs(const std::string& runtime_id, const drogon::orm::DbClientPtr& client);
    Json::Value comboTrend(
        const std::string& runtime_id,
        const std::string& device_id,
        const std::string& range);

private:
    DemoPowerMeter() = default;

    struct Sample
    {
        int64_t ts_ms = 0;
        double w = 0;
        double v = 0;
        double a = 0;
    };

    struct PlugSlot
    {
        DemoPowerReading reading;
        int64_t last_jitter_ms = 0;
        std::deque<Sample> history;
    };

    struct SessionSlot
    {
        std::unordered_map<std::string, PlugSlot> plugs;
    };

    static int64_t nowMs();
    static double jitterFactor();
    void pushHistory(PlugSlot& slot, const DemoPowerReading& reading);
    DemoPowerReading refreshLocked(PlugSlot& slot, bool switch_on, double rated_w, double voltage_v, bool force);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, SessionSlot> m_sessions;
};

WAVE_NAMESPACE_END
