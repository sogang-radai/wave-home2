#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../device/device_manager.h"
#include "../nn/gesture_pipeline.h"
#include "action_queue.h"
#include "rule_store.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

class TriggerManager
{
public:
    TriggerManager();
    ~TriggerManager();

    void start(
        RuleStore& rules,
        ActionQueue& actions,
        dev::DeviceManager& devices,
        const std::function<std::filesystem::path(const std::string&)>& resolve_path);
    void stop();

    void reconcile();
    void onIrReceived(const std::string& device_id, const std::string& command_id);

private:
    struct GestureRuntime
    {
        std::string radarDeviceId;
        std::string gestureSetPath;
        std::unique_ptr<nn::GesturePipeline> pipeline;
        uint64_t lastFrameIndex = 0;
    };

    struct ScheduleState
    {
        Rule rule;
        std::chrono::system_clock::time_point nextFire;
        std::chrono::system_clock::time_point createdAt;
        bool firedOnce = false;
    };

    struct DeviceStateRuntime
    {
        bool lastMatched = false;
    };

    void runLoop();
    void tickGesture();
    void tickDeviceState();
    void tickSchedule();
    void dispatchBindings(
        const std::vector<TriggerBinding>& bindings,
        int32_t gesture_class_id,
        const std::string& source_ref);
    bool evaluateDeviceState(const Trigger& trigger, double& out_value) const;
    bool compareValue(const std::string& op, double lhs, double rhs) const;
    bool checkCooldown(const std::string& rule_id, uint32_t cooldown_ms);
    void markFired(const std::string& rule_id);
    void ensureGestureRuntime(const GestureIndexKey& key);
    void removeStaleGestureRuntimes(const TriggerIndexSnapshot& index);
    dev::Device* findDevice(const std::string& external_id) const;

    RuleStore* m_rules = nullptr;
    ActionQueue* m_actions = nullptr;
    dev::DeviceManager* m_devices = nullptr;
    std::function<std::filesystem::path(const std::string&)> m_resolvePath;

    std::mutex m_stateMutex;
    TriggerIndexSnapshot m_index;
    std::vector<GestureRuntime> m_gestureRuntimes;
    std::map<DeviceStateIndexKey, DeviceStateRuntime> m_deviceStateRuntime;
    std::unordered_map<std::string, ScheduleState> m_scheduleStates;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastFired;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastRepeatFire;

    std::atomic<bool> m_running{false};
    std::thread m_worker;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
