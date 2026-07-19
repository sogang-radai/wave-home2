#include "agent_job_queue.h"

#include <algorithm>

#include "../../app/app_state.h"
#include "../../core/logger.h"
#include "../../web/http/v1/power_store.h"
#include "insight_generator.h"
#include "../sleep/sleep_manager.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    void notify_waiter(const std::shared_ptr<std::promise<bool>>& waiter, bool ok)
    {
        if (!waiter)
            return;
        try
        {
            waiter->set_value(ok);
        }
        catch (const std::future_error&)
        {
            // Already satisfied.
        }
    }
}

std::string AgentJob::targetKey() const
{
    switch (kind)
    {
    case AgentJobKind::SleepSummary30m:
        return "sleep:summary:" + std::to_string(statId);
    case AgentJobKind::SleepDailyReport:
        return "sleep:daily:" + std::to_string(userId) + ":" + periodStart + ":" + std::to_string(sessionId);
    case AgentJobKind::SleepWeeklyReport:
        return "sleep:weekly:" + std::to_string(userId) + ":" + periodStart;
    case AgentJobKind::PowerReport:
        return "power:" + period + ":" + periodStart;
    case AgentJobKind::Insight:
        return "insight:" + std::to_string(userId) + ":" + surface + ":" + date;
    }
    return "unknown";
}

int AgentJob::periodRank() const
{
    switch (kind)
    {
    case AgentJobKind::SleepSummary30m:
        return 0;
    case AgentJobKind::PowerReport:
        if (period == "1h")
            return 0;
        if (period == "24h")
            return 1;
        if (period == "1w")
            return 2;
        if (period == "1mo")
            return 3;
        if (period == "1yr")
            return 4;
        return 5;
    case AgentJobKind::SleepDailyReport:
    case AgentJobKind::Insight:
        return 1;
    case AgentJobKind::SleepWeeklyReport:
        return 2;
    }
    return 9;
}

int AgentJob::domainRank() const
{
    switch (kind)
    {
    case AgentJobKind::SleepSummary30m:
    case AgentJobKind::SleepDailyReport:
    case AgentJobKind::SleepWeeklyReport:
        return 0;
    case AgentJobKind::Insight:
        return surface == "sleep_report" ? 0 : 1;
    case AgentJobKind::PowerReport:
        return 1;
    }
    return 1;
}

AgentJobQueue& AgentJobQueue::get()
{
    static AgentJobQueue instance;
    return instance;
}

AgentJobQueue::~AgentJobQueue()
{
    stop();
}

void AgentJobQueue::start()
{
    if (m_running.exchange(true))
        return;

    m_worker = std::thread([this]() { runLoop(); });
    WLOG_INFO("AgentJobQueue started");
}

void AgentJobQueue::stop()
{
    if (!m_running.exchange(false))
        return;

    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();

    std::lock_guard lock(m_mutex);
    for (auto& pending : m_pending)
        notify_waiter(pending.waiter, false);
    m_pending.clear();
    m_runningKey.clear();
}

bool AgentJobQueue::insertPending(PendingJob pending)
{
    const std::string key = pending.job.targetKey();
    if (!m_runningKey.empty() && m_runningKey == key)
        return false;
    for (const auto& existing : m_pending)
    {
        if (existing.job.targetKey() == key)
            return false;
    }

    m_pending.push_back(std::move(pending));
    return true;
}

bool AgentJobQueue::enqueue(AgentJob job)
{
    {
        std::lock_guard lock(m_mutex);
        PendingJob pending;
        pending.job = std::move(job);
        pending.seq = m_nextSeq++;
        if (!insertPending(std::move(pending)))
            return false;
    }
    m_cv.notify_one();
    return true;
}

bool AgentJobQueue::enqueueAndWait(AgentJob job, std::chrono::milliseconds timeout)
{
    auto waiter = std::make_shared<std::promise<bool>>();
    auto future = waiter->get_future();

    {
        std::lock_guard lock(m_mutex);
        PendingJob pending;
        pending.job = std::move(job);
        pending.seq = m_nextSeq++;
        pending.waiter = waiter;
        if (!insertPending(std::move(pending)))
        {
            // Duplicate: wait for the in-flight / queued job with same key by polling DB is not
            // available here — treat as "already handled" and return true so callers re-read cache.
            return true;
        }
    }
    m_cv.notify_one();

    if (future.wait_for(timeout) != std::future_status::ready)
        return false;
    return future.get();
}

std::optional<AgentJobQueue::PendingJob> AgentJobQueue::popHighest()
{
    if (m_pending.empty())
        return std::nullopt;

    auto best = m_pending.begin();
    for (auto it = m_pending.begin() + 1; it != m_pending.end(); ++it)
    {
        const int pr = it->job.periodRank();
        const int best_pr = best->job.periodRank();
        if (pr < best_pr
            || (pr == best_pr && it->job.domainRank() < best->job.domainRank())
            || (pr == best_pr && it->job.domainRank() == best->job.domainRank() && it->seq < best->seq))
        {
            best = it;
        }
    }

    PendingJob chosen = std::move(*best);
    m_pending.erase(best);
    return chosen;
}

void AgentJobQueue::runLoop()
{
    while (m_running.load(std::memory_order_acquire))
    {
        PendingJob pending;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::seconds(1), [this]()
            {
                return !m_pending.empty() || !m_running.load(std::memory_order_acquire);
            });
            if (!m_running.load(std::memory_order_acquire))
                break;
            auto next = popHighest();
            if (!next)
                continue;
            pending = std::move(*next);
            m_runningKey = pending.job.targetKey();
        }

        bool ok = false;
        try
        {
            process(pending.job);
            ok = true;
        }
        catch (const std::exception& e)
        {
            WLOG_WARN("AgentJobQueue job failed ({}): {}", pending.job.targetKey(), e.what());
            ok = false;
        }

        {
            std::lock_guard lock(m_mutex);
            m_runningKey.clear();
        }
        notify_waiter(pending.waiter, ok);
    }
}

void AgentJobQueue::process(const AgentJob& job)
{
    switch (job.kind)
    {
    case AgentJobKind::SleepSummary30m:
    case AgentJobKind::SleepDailyReport:
    case AgentJobKind::SleepWeeklyReport:
        SleepManager::get().processQueuedJob(job);
        break;
    case AgentJobKind::PowerReport:
        web::v1::PowerStore::run_queued_report(job);
        break;
    case AgentJobKind::Insight:
    {
        auto client = AppState::get().db();
        if (!client)
            return;
        std::string error;
        if (!generateAndPersistInsights(
                client,
                AppState::get().config.agent.base_url,
                job.userId,
                job.surface,
                job.date,
                error))
        {
            WLOG_WARN(
                "queued insight failed (user {}, surface {}, date {}): {}",
                job.userId,
                job.surface,
                job.date,
                error);
        }
        break;
    }
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
