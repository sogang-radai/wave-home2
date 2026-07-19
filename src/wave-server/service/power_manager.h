#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct PlugReading
{
    int64_t ts_ms = 0;
    bool connected = false;
    bool switch_on = false;
    double power_w = 0;
    double voltage_v = 0;
    double current_ma = 0;
};

struct PowerSample
{
    int64_t ts_ms = 0;
    double w = 0;
    double v = 0;
    double a = 0;
};

// Samples smart-plug telemetry every second, keeps short history for live charts,
// and rolls 5-minute energy buckets into power_energy.
class PowerManager
{
public:
    static PowerManager& get();

    void start();
    void stop();

    std::optional<PlugReading> getReading(const std::string& external_id) const;
    std::deque<PowerSample> getHistory(const std::string& external_id) const;
    std::deque<PowerSample> getMergedHistory() const;

    void sampleNow();
    void samplePlugNow(const std::string& external_id);

    /** UI metering toggle — false stops energy aggregation for this plug. Default true. */
    void setMeteringEnabled(const std::string& external_id, bool enabled);
    bool isMeteringEnabled(const std::string& external_id) const;
    void reloadMeteringFromDb();

private:
    PowerManager() = default;
    ~PowerManager();
    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    struct EnergyBucket
    {
        std::string time_start;
        double energy_wh = 0;
        int sample_count = 0;
        int64_t last_ts_ms = 0;
        double last_power_w = 0;
    };

    void runLoop();
    void sampleAllPlugs();
    void maybeEnqueueSchedules();
    void samplePlug(const std::string& external_id);
    void storeSample(const std::string& external_id, const PlugReading& reading);
    void accumulateEnergy(const std::string& external_id, const PlugReading& reading);
    void flushBucket(const std::string& bucket_key, EnergyBucket& bucket, std::optional<int64_t> db_device_id);
    void flushDueBuckets(int64_t now_ms);
    std::optional<int64_t> resolveDbDeviceId(const std::string& external_id);
    static std::string bucket_time_start(int64_t ts_ms);
    static void trim_history(std::deque<PowerSample>& history);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, PlugReading> m_latest;
    std::unordered_map<std::string, std::deque<PowerSample>> m_history;
    std::unordered_map<std::string, EnergyBucket> m_buckets;
    std::unordered_map<std::string, int64_t> m_db_device_cache;
    /** external_id → metering; missing key means enabled (true). */
    std::unordered_map<std::string, bool> m_meteringEnabled;
    std::string m_lastHourlyReportHour;
    std::string m_lastDailyScheduleDate;
    std::string m_lastYearlyScheduleWeek;

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::mutex m_stopMutex;
    std::condition_variable m_stopCv;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
