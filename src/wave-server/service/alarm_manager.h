#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <json/json.h>

#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct AlarmRecord
{
    int64_t id = 0;
    int64_t user_id = 0;
    std::string name;
    int time_minute = 0;
    std::vector<std::string> days_of_week;
    bool smart_wake = false;
    std::string device_external_id;
    std::string radar_external_id;
    Json::Value method;
    bool enabled = true;
};

class AlarmManager
{
public:
    static AlarmManager& get();

    void start();
    void stop();
    void reconcile();

private:
    AlarmManager() = default;
    ~AlarmManager();
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    struct RuntimeState
    {
        std::string last_fired_date;
        bool once_fired = false;
    };

    void runLoop();
    void tick();
    bool isDueNow(const AlarmRecord& alarm, const std::string& today) const;
    void fireAlarm(const AlarmRecord& alarm);
    void executeMethod(const AlarmRecord& alarm);
    void markFired(AlarmRecord& alarm, const std::string& today);
    void disableOnceAlarm(int64_t alarm_id);
    void insertNotification(int64_t user_id, const std::string& message);
    std::vector<AlarmRecord> loadEnabledAlarms();
    std::string todayDate() const;
    std::string nowStamp() const;

    mutable std::mutex m_mutex;
    std::unordered_map<int64_t, AlarmRecord> m_alarms;
    std::unordered_map<int64_t, RuntimeState> m_runtime;

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::mutex m_stop_mutex;
    std::condition_variable m_stop_cv;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
