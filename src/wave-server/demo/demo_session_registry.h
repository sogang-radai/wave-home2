#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
    Json::Value chat_histories = Json::Value(Json::arrayValue);
};

class DemoSessionRegistry
{
public:
    static DemoSessionRegistry& instance();

    DemoSessionData& touch(const std::string& runtime_id);
    std::optional<DemoSessionData> get(const std::string& runtime_id) const;
    void evictExpired(int64_t ttl_ms = 45 * 60 * 1000, size_t max_sessions = 500);

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, DemoSessionData> m_sessions;
};

WAVE_NAMESPACE_END
