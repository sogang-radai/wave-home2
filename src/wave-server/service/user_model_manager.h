#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

// Once per calendar day: (1) recomputes each user's rolling-window sleep/light
// behavior aggregate into daily_user_model (pure SQL — mirrors PowerManager's
// flushBucket idiom, not AgentJobQueue, since there's no LLM involved), then
// (2) refreshes/expires existing user_habit rows' confidence (also pure SQL),
// then (3) calls the agent directly (not via AgentJobQueue — this is a
// once-a-day batch with no dedup/priority need, same direct-call precedent
// InsightsController already uses) to discover new candidate habits.
class UserModelManager
{
public:
    static UserModelManager& get();

    void start();
    void stop();

    /** Force a full rollover computation for `for_date` now, bypassing the
     *  day-rollover guard — used for debugging/manual triggering. */
    void computeNow(const std::string& for_date);

private:
    UserModelManager() = default;
    ~UserModelManager();
    UserModelManager(const UserModelManager&) = delete;
    UserModelManager& operator=(const UserModelManager&) = delete;

    void runRolloverFor(const std::string& for_date);
    void computeAndStoreUserModelForAllUsers(const std::string& for_date);
    void computeAndStoreUserModelForUser(int64_t user_id, const std::string& for_date);
    void refreshHabitsForAllUsers(const std::string& for_date);
    void refreshHabitsForUser(int64_t user_id, const std::string& for_date);
    void discoverHabitsForAllUsers(const std::string& for_date);
    void synthesizeBannersForAllUsers(const std::string& for_date);

    static constexpr int kWindowDays = 14;
    static constexpr double kHabitExpireThreshold = 0.2;

    std::string m_lastModelDate;
    std::mutex m_mutex;

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::mutex m_stopMutex;
    std::condition_variable m_stopCv;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
