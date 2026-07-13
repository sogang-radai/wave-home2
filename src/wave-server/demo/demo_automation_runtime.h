#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

class DemoAutomationRuntime
{
public:
    static DemoAutomationRuntime& get();

    void start();
    void stop();

private:
    DemoAutomationRuntime() = default;
    ~DemoAutomationRuntime();
    DemoAutomationRuntime(const DemoAutomationRuntime&) = delete;
    DemoAutomationRuntime& operator=(const DemoAutomationRuntime&) = delete;

    void runLoop();
    void tick();
    void tickSession(const std::string& runtime_id);

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::mutex m_stopMutex;
    std::condition_variable m_stopCv;
};

WAVE_NAMESPACE_END
