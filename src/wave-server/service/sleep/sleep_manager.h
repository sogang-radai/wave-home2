#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../core/json.h"
#include "../../nn/sleep_pipeline.h"

#include "sleep_aggregator.h"
#include "sleep_audio.h"
#include "sleep_session_fsm.h"
#include "sleep_stage_synth.h"
#include "sleep_vitals.h"
#include "sleep_vec_store.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct SleepRoomConfig
{
    int32_t roomId = 0;
    int32_t userId = 0;
    int64_t radarDbId = 0;
    int64_t stationDbId = 0;
    std::string radarExternalId;
    std::optional<std::string> stationExternalId;
};

struct SleepRuntime
{
    SleepRoomConfig config;
    std::unique_ptr<nn::SleepPipeline> pipeline;
    uint64_t lastFrameIndex = 0;
    SecondAggregator secondAgg;
    MinuteAggregator minuteAgg;
    ThirtyMinAggregator thirtyMinAgg;
    SessionFsm sessionFsm;
    std::string activeSecondStart;
    std::string activeMinuteStart;
    std::string activeThirtyMinStart;
    bool secondInitialized = false;
    bool minuteInitialized = false;
    bool thirtyMinInitialized = false;
    VitalTargetPicker vitalPicker;
    VitalSignsProcessor vitalProcessor;
    VitalEstimate lastVitals;
    SnoreAudioAnalyzer snoreAudio;
    bool micSubscribed = false;
    int32_t asleepMinutesBeforeWindow = 0;
    int32_t asleepMinutesAtWindowStart = 0;
    std::vector<StageSynthResult> minuteStagesInWindow;
    std::vector<StageSynthResult> sessionStageWindows;
    std::vector<StageSynthResult> sessionMinuteStages;
    double windowSnoreSum = 0.0;
    double windowNoiseSum = 0.0;
    int32_t windowAudioMinutes = 0;
};

enum class SleepJobKind
{
    Summary30m,
    DailyReport,
    WeeklyReport,
    Plan,
};

struct SleepJob
{
    SleepJobKind kind = SleepJobKind::Summary30m;
    int32_t userId = 0;
    int32_t roomId = 0;
    int64_t statId = 0;
    int64_t sessionId = 0;
    std::string period;
    std::string periodStart;
    json payload;
};

class SleepManager
{
public:
    static SleepManager& get();

    void start();
    void stop();
    void reconcile();

    /** True while a sleep session is subscribed to this station's mic_opus. */
    bool isStationMicInUse(const std::string& station_external_id) const;

    /** plan_date(YYYY-MM-DD) 밤의 "오늘 밤 추천 수면 시간"을 에이전트에 비동기로 생성 요청한다
     * (job worker 스레드에서 처리되므로 호출부를 막지 않는다). 실제 라디오 파이프라인은
     * handleSessionClose() 에서 자동으로 호출하고, 데모 모드는 DemoProfileRuntime 이 자체
     * 자동화(DemoAutomationRuntime)를 쓰지만 이 job worker 는 별도로 시작해 활용한다. */
    void requestSleepPlan(int32_t user_id, const std::string& plan_date);

private:
    SleepManager() = default;
    ~SleepManager();
    SleepManager(const SleepManager&) = delete;
    SleepManager& operator=(const SleepManager&) = delete;

    void runLoop();
    void runJobLoop();
    void tickRuntime(SleepRuntime& runtime);
    void consumePointCloud(SleepRuntime& runtime);
    void ingestResult(SleepRuntime& runtime, const nn::SleepResult& result);
    void flushSecondBoundary(SleepRuntime& runtime, const std::string& now_ts);
    void flushMinuteBoundary(SleepRuntime& runtime, const std::string& now_ts);
    void flushThirtyMinBoundary(SleepRuntime& runtime, const std::string& now_ts);
    void persistMinuteStat(SleepRuntime& runtime, const MinuteStat& stat);
    void persistThirtyMinStat(SleepRuntime& runtime, const ThirtyMinStat& stat);
    void handleSessionClose(SleepRuntime& runtime, const SessionCloseResult& close);
    void backfillSessionId(int32_t userId, const std::string& onset, const std::string& final_wake, int64_t session_id);
    void enqueueJob(SleepJob job);
    void processJob(const SleepJob& job);
    bool initPipeline(SleepRuntime& runtime, std::string& out_error);
    std::vector<SleepRoomConfig> loadRoomConfigs();
    void tickVitals(SleepRuntime& runtime);
    void tickAudio(SleepRuntime& runtime);
    void ensureMicSubscription(SleepRuntime& runtime, bool want_subscribed);
    static bool is_sleep_enabled_radar(const std::string& external_id);
    std::optional<double> queryStationEnv(const std::string& external_id, const char* field);
    void storeAgentEmbeddings(SleepJobKind kind, int64_t row_id, const std::vector<float>& embedding);

    mutable std::mutex m_mutex;
    std::unordered_map<int32_t, SleepRuntime> m_runtimes;

    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<SleepJob> m_jobs;

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::thread m_jobWorker;
    std::mutex m_stopMutex;
    std::condition_variable m_stopCv;
    bool m_modelReady = false;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
