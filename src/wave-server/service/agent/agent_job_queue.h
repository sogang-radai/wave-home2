#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../../core/json.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

enum class AgentJobKind
{
    SleepSummary30m,
    SleepDailyReport,
    SleepWeeklyReport,
    PowerReport,
    Insight,
};

struct AgentJob
{
    AgentJobKind kind = AgentJobKind::SleepSummary30m;

    // Sleep
    int32_t userId = 0;
    int32_t roomId = 0;
    int64_t statId = 0;
    int64_t sessionId = 0;
    json payload;

    // Power report / shared period key
    std::string period;
    std::string periodStart;
    std::string windowStart;
    std::string windowEnd;
    double expected5mBuckets = 0.0;

    // Insight
    std::string surface;
    std::string date;

    std::string targetKey() const;
    int periodRank() const;
    int domainRank() const;
};

/**
 * Single-worker priority queue for sleep/power report & insight agent jobs.
 * Priority: (1) in-flight exclusive (2) smaller period first (3) sleep before power.
 */
class AgentJobQueue
{
public:
    static AgentJobQueue& get();

    void start();
    void stop();

    /** Fire-and-forget. Skips if an identical targetKey is already queued or running. */
    bool enqueue(AgentJob job);

    /**
     * Enqueue and wait until the job finishes (or timeout).
     * Returns false on timeout, failure, or duplicate skip without a waiter attached.
     */
    bool enqueueAndWait(AgentJob job, std::chrono::milliseconds timeout);

private:
    AgentJobQueue() = default;
    ~AgentJobQueue();
    AgentJobQueue(const AgentJobQueue&) = delete;
    AgentJobQueue& operator=(const AgentJobQueue&) = delete;

    struct PendingJob
    {
        AgentJob job;
        uint64_t seq = 0;
        std::shared_ptr<std::promise<bool>> waiter;
    };

    void runLoop();
    void process(const AgentJob& job);
    bool insertPending(PendingJob pending);
    std::optional<PendingJob> popHighest();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<PendingJob> m_pending;
    std::string m_runningKey;
    uint64_t m_nextSeq = 1;
    std::atomic<bool> m_running{false};
    std::thread m_worker;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
