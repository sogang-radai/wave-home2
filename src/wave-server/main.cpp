#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <thread>

#include "app/app_state.h"
#include "core/logger.h"
#include "core/task_queue.h"

namespace
{
    std::atomic<bool> g_forceExit{false};

    void installShutdownHandlers()
    {
        struct sigaction action {};
        action.sa_handler = [](int signal)
        {
            if (g_forceExit.exchange(true, std::memory_order_acq_rel))
            {
                LOG_WARN("Forced exit (signal {})", signal);
                std::_Exit(128 + signal);
            }

            if (signal == SIGTSTP)
                LOG_INFO("Shutdown requested (Ctrl+Z)");
            else if (signal == SIGINT)
                LOG_INFO("Shutdown requested (Ctrl+C)");
            else
                LOG_INFO("Shutdown requested (signal {})", signal);

            ws::AppState::get().running.store(false, std::memory_order_release);
        };
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;

        sigaction(SIGTSTP, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);
    }
}

class MyApp
{
public:
    MyApp()
    {
        m_taskQueue.init(12);
        m_app.init("config.json");
        installShutdownHandlers();
    }

    ~MyApp()
    {
        shutdown();
    }

    int run()
    {
        LOG_INFO("Main loop started (Ctrl+Z or Ctrl+C to stop)");

        while (m_app.running.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        LOG_INFO("Main loop stopped");
        shutdown();
        return 0;
    }

private:
    void shutdown()
    {
        if (m_shutdownDone.exchange(true))
            return;

        m_app.shutdown();
        m_taskQueue.shutdown();
        LOG_INFO("Task queue shutdown complete");
    }

    std::atomic<bool> m_shutdownDone{false};

    ws::TaskQueue m_taskQueue;
    ws::AppState m_app;
};

int main()
{
    auto app = std::make_unique<MyApp>();
    return app->run();
}
