#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct CompanionStationRuntime
{
    std::string externalId;
    std::string name;
    int64_t userId = 0;
    int64_t roomId = 0;
    bool companionEnabled = false;
    bool micSubscribed = false;
    bool processing = false;
    std::string sttSessionId;
    std::optional<int64_t> conversationId;
    std::chrono::steady_clock::time_point lastActivity {};
    bool hasLastActivity = false;
    std::string lastPartialText;
    std::string lastFinalText;
};

struct CompanionListenStatus
{
    bool enabled = false;
    bool listening = false;
    bool processing = false;
    std::string partialText;
    std::string finalText;
};

class CompanionManager
{
public:
    static CompanionManager& get();

    void start();
    void stop();
    void reconcile();
    /** Re-apply mic_gain from DB after DeviceManager finishes creating devices. */
    void onDevicesReady();

    /** Apply settings.companion (and optional mic_gain) from a successful device PATCH. */
    void notifyDeviceUpdated(
        const std::string& external_id,
        bool companion_enabled,
        std::optional<float> mic_gain = std::nullopt);

    CompanionListenStatus listenStatus(const std::string& external_id) const;

private:
    CompanionManager() = default;
    ~CompanionManager();
    CompanionManager(const CompanionManager&) = delete;
    CompanionManager& operator=(const CompanionManager&) = delete;

    void runLoop();
    void tickAll();
    void ensureMicSubscription(CompanionStationRuntime& runtime, bool want_subscribed);
    void ensureSttSession(CompanionStationRuntime& runtime);
    void releaseSttSession(CompanionStationRuntime& runtime, bool abort);
    void drainMic(const std::string& external_id);
    void processUtterance(
        const std::string& external_id,
        int64_t user_id,
        const std::string& text,
        std::optional<int64_t> conversation_id,
        bool has_last_activity,
        std::chrono::steady_clock::time_point last_activity);
    bool playChime(const std::string& external_id);
    bool loadChimeWav();
    void applyMicGainToStation(const std::string& external_id, float mic_gain);
    void applyMicGainsFromDb();

    std::vector<CompanionStationRuntime> loadStationConfigs();

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, CompanionStationRuntime> m_stations;

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::mutex m_stopMutex;
    std::condition_variable m_stopCv;

    std::vector<int16_t> m_chimePcm;
    int32_t m_chimeSampleRate = 0;
    bool m_chimeLoaded = false;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
