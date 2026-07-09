#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "../core/json.h"
#include "../core/coredefs.h"
#include "../device/device_manager.h"
#include "trigger_types.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct ActionJob
{
    std::string targetDeviceId;
    std::string actionName;
    json params = json::object();
    ExecMode execMode = ExecMode::Once;
    uint32_t repeatIntervalMs = 0;
    std::string sourceRef = "manual";
    std::string ruleId;
    std::string deviceName;
    std::string logMessage;
};

struct ActionResult
{
    int rc = -1;
    std::string code;
};

class ActionQueue
{
public:
    ActionQueue();
    ~ActionQueue();

    void start(dev::DeviceManager& devices);
    void stop();

    void enqueue(ActionJob job);
    std::future<ActionResult> enqueueAndWait(ActionJob job, uint32_t timeout_ms = 5000);

    void cancelRepeatJobs(const std::string& rule_id);

private:
    struct PendingWait
    {
        std::promise<ActionResult> promise;
        ActionJob job;
    };

    struct RepeatState
    {
        ActionJob job;
        std::chrono::steady_clock::time_point lastFire;
        bool toggleOn = false;
    };

    void runLoop();
    ActionResult executeJob(const ActionJob& job);
    void processRepeatJobs();
    dev::Device* findDevice(const std::string& external_id) const;
    dev::Actionable* asActionable(dev::Device* device) const;

    dev::DeviceManager* m_devices = nullptr;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<ActionJob> m_queue;
    std::deque<std::shared_ptr<PendingWait>> m_waiters;
    std::unordered_map<std::string, RepeatState> m_repeatJobs;
    std::unordered_map<std::string, bool> m_toggleStates;

    std::atomic<bool> m_running{false};
    std::thread m_worker;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
