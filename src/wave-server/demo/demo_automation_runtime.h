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
    DemoAutomationRuntime() = default;
    ~DemoAutomationRuntime();

    DemoAutomationRuntime(const DemoAutomationRuntime&) = delete;
    DemoAutomationRuntime& operator=(const DemoAutomationRuntime&) = delete;

    void start();
    void stop();

private:
    void runLoop();
    void tick();
    void tickSession(const std::string& runtime_id);

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::mutex m_stopMutex;
    std::condition_variable m_stopCv;
};

WAVE_NAMESPACE_END
