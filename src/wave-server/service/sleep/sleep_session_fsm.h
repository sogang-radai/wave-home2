#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/coredefs.h"
#include "sleep_aggregator.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

enum class SessionPhase
{
    OutOfBed,
    InBedAwake,
    Sleeping,
    MicroWake,
    WakePending,
    SessionClosed,
};

struct SessionState
{
    SessionPhase phase = SessionPhase::OutOfBed;
    std::optional<std::string> onset;
    std::optional<std::string> finalWake;
    std::optional<std::string> nightDate;
    std::optional<int64_t> sessionId;
    int32_t onsetMinuteHits = 0;
    int32_t wakeMinuteHits = 0;
    int32_t absentMinuteHits = 0;
    int32_t microWakeMinutes = 0;
    std::vector<MinuteStat> pendingMinutes;
    int32_t totalTossEvents = 0;
    int32_t asleepTotalMinutes = 0;
    int32_t inBedMinutes = 0;
};

struct SessionCloseResult
{
    bool closed = false;
    std::string nightDate;
    std::string onset;
    std::string finalWake;
    int32_t timeInBedS = 0;
    int32_t asleepTotalS = 0;
    double efficiency = 0.0;
    int32_t tossEvents = 0;
};

class SessionFsm
{
public:
    void reset();
    std::optional<SessionCloseResult> onMinute(const MinuteStat& minute);

    SessionPhase phase() const { return m_state.phase; }
    const SessionState& state() const { return m_state; }
    SessionState& mutableState() { return m_state; }

private:
    static bool is_asleep_minute(const MinuteStat& minute);
    static bool is_wake_minute(const MinuteStat& minute);
    static bool is_absent_minute(const MinuteStat& minute);
    static std::string date_from_timestamp(const std::string& timestamp);

    SessionState m_state;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
