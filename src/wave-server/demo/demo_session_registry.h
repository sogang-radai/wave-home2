#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

struct DemoSessionData
{
    int64_t last_touch_ms = 0;
    bool data_seeded = false;
    std::unordered_map<std::string, Json::Value> device_state;
    std::unordered_map<std::string, std::string> radar_gesture_sets;
    Json::Value alarms = Json::Value(Json::arrayValue);
    Json::Value rules = Json::Value(Json::arrayValue);
    Json::Value schedule_tasks = Json::Value(Json::arrayValue);
    Json::Value goals = Json::Value(Json::arrayValue);
    // goalId(string) -> coaching payload {periodStart, pastSummary, projection, projectedMetrics, recommendations}
    Json::Value goal_coachings = Json::Value(Json::objectValue);
    Json::Value chat_histories = Json::Value(Json::arrayValue);
    Json::Value notifications = Json::Value(Json::arrayValue);
    // deviceId -> {voiceLabel, text, expiresAtMs, intervalSec, nextShowAtMs, alarmId, alarmName, ...}
    Json::Value speech_overlays = Json::Value(Json::objectValue);
    // userId(string) -> {personalPrompt, selectedModelId, ctrlEnterSend, waveAiSound}
    Json::Value ai_agent_settings = Json::Value(Json::objectValue);

    // Runtime fire tracking (not exposed via list APIs)
    std::unordered_map<int64_t, std::string> alarm_last_fired_date;
    std::unordered_set<int64_t> alarm_once_fired;
    std::unordered_map<std::string, int64_t> schedule_next_fire_ms;
    std::unordered_set<std::string> schedule_once_fired;
    std::unordered_map<std::string, bool> schedule_slot_fired;
};

/** Holds the registry mutex for the lifetime of the handle so parallel demo CRUD
 *  (e.g. agent creating 7 weekday schedule tasks) cannot race on Json::Value arrays. */
class LockedDemoSession
{
public:
    LockedDemoSession(std::unique_lock<std::recursive_mutex> lock, DemoSessionData& data)
        : m_lock(std::move(lock)), m_data(&data)
    {
    }

    DemoSessionData& operator*() { return *m_data; }
    const DemoSessionData& operator*() const { return *m_data; }
    DemoSessionData* operator->() { return m_data; }
    const DemoSessionData* operator->() const { return m_data; }

private:
    std::unique_lock<std::recursive_mutex> m_lock;
    DemoSessionData* m_data;
};

class DemoSessionRegistry
{
public:
    LockedDemoSession lockSession(const std::string& runtime_id);
    std::optional<DemoSessionData> get(const std::string& runtime_id) const;
    std::vector<std::string> listRuntimeIds() const;
    void evictExpired(int64_t ttl_ms = 45 * 60 * 1000, size_t max_sessions = 500);

private:
    // recursive: tick/fire paths may re-enter lockSession on the same thread.
    mutable std::recursive_mutex m_mutex;
    std::unordered_map<std::string, DemoSessionData> m_sessions;
};

/** Active DemoProfileRuntime session store (asserts demo profile). */
DemoSessionRegistry& demoSessionRegistry();

WAVE_NAMESPACE_END
