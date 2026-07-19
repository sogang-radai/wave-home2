#include "sleep_session_fsm.h"

#include <cmath>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr double kOnsetAsleepR = 0.85;
    constexpr int32_t kOnsetMinutesRequired = 5;
    constexpr int32_t kOnsetHitsRequired = 4;
    constexpr int32_t kWakeMinutesRequired = 5;
    constexpr double kWakeAwakeAbsentRatio = 0.7;
    constexpr int32_t kAbsentMinutesRequired = 3;
    constexpr double kAbsentRatio = 0.8;
    constexpr int32_t kMicroWakeMaxMinutes = 3;
}

void SessionFsm::reset()
{
    m_state = SessionState{};
}

bool SessionFsm::is_asleep_minute(const MinuteStat& minute)
{
    return minute.asleepR >= kOnsetAsleepR
        || (minute.statusRatio.is_object()
            && minute.statusRatio.value("asleep", 0.0) >= kOnsetAsleepR);
}

bool SessionFsm::is_wake_minute(const MinuteStat& minute)
{
    if (!minute.statusRatio.is_object())
        return false;
    const double awake = minute.statusRatio.value("awake", 0.0);
    const double absent = minute.statusRatio.value("absent", 0.0);
    return (awake + absent) >= kWakeAwakeAbsentRatio;
}

bool SessionFsm::is_absent_minute(const MinuteStat& minute)
{
    if (!minute.statusRatio.is_object())
        return false;
    return minute.statusRatio.value("absent", 0.0) >= kAbsentRatio;
}

std::string SessionFsm::date_from_timestamp(const std::string& timestamp)
{
    if (timestamp.size() >= 10)
        return timestamp.substr(0, 10);
    return timestamp;
}

std::optional<SessionCloseResult> SessionFsm::onMinute(const MinuteStat& minute)
{
    m_state.pendingMinutes.push_back(minute);
    m_state.totalTossEvents += minute.tossEvents;

    const bool asleep_minute = is_asleep_minute(minute);
    const bool wake_minute = is_wake_minute(minute);
    const bool absent_minute = is_absent_minute(minute);

    switch (m_state.phase)
    {
    case SessionPhase::OutOfBed:
    case SessionPhase::SessionClosed:
        if (asleep_minute)
        {
            m_state.phase = SessionPhase::InBedAwake;
            m_state.onsetMinuteHits = 1;
            m_state.wakeMinuteHits = 0;
            m_state.absentMinuteHits = 0;
            m_state.microWakeMinutes = 0;
            m_state.onset = minute.timeStart;
            m_state.nightDate = date_from_timestamp(minute.timeStart);
            m_state.inBedMinutes = 1;
        }
        break;

    case SessionPhase::InBedAwake:
        ++m_state.inBedMinutes;
        if (asleep_minute)
        {
            ++m_state.onsetMinuteHits;
            if (m_state.onsetMinuteHits >= kOnsetHitsRequired)
            {
                m_state.phase = SessionPhase::Sleeping;
                m_state.onset = minute.timeStart;
                m_state.nightDate = date_from_timestamp(minute.timeStart);
            }
        }
        else
        {
            m_state.onsetMinuteHits = 0;
            if (absent_minute && !wake_minute)
            {
                m_state.phase = SessionPhase::OutOfBed;
                m_state.pendingMinutes.clear();
                m_state.onset.reset();
                m_state.nightDate.reset();
                m_state.inBedMinutes = 0;
            }
        }
        break;

    case SessionPhase::Sleeping:
        ++m_state.inBedMinutes;
        if (asleep_minute)
        {
            ++m_state.asleepTotalMinutes;
            m_state.finalWake = minute.timeEnd;
            m_state.wakeMinuteHits = 0;
            m_state.absentMinuteHits = 0;
            m_state.microWakeMinutes = 0;
        }
        else if (wake_minute || absent_minute)
        {
            m_state.phase = SessionPhase::WakePending;
            m_state.wakeMinuteHits = wake_minute ? 1 : 0;
            m_state.absentMinuteHits = absent_minute ? 1 : 0;
            m_state.microWakeMinutes = 1;
        }
        break;

    case SessionPhase::MicroWake:
        ++m_state.inBedMinutes;
        if (asleep_minute)
        {
            m_state.phase = SessionPhase::Sleeping;
            ++m_state.asleepTotalMinutes;
            m_state.finalWake = minute.timeEnd;
            m_state.wakeMinuteHits = 0;
            m_state.absentMinuteHits = 0;
            m_state.microWakeMinutes = 0;
        }
        else
        {
            ++m_state.microWakeMinutes;
            if (m_state.microWakeMinutes >= kMicroWakeMaxMinutes)
            {
                m_state.phase = SessionPhase::WakePending;
                m_state.wakeMinuteHits = wake_minute ? m_state.microWakeMinutes : 0;
                m_state.absentMinuteHits = absent_minute ? m_state.microWakeMinutes : 0;
            }
        }
        break;

    case SessionPhase::WakePending:
        ++m_state.inBedMinutes;
        if (asleep_minute)
        {
            if (m_state.wakeMinuteHits > 0 && m_state.wakeMinuteHits < kMicroWakeMaxMinutes)
            {
                m_state.phase = SessionPhase::MicroWake;
                m_state.microWakeMinutes = 1;
            }
            else
            {
                m_state.phase = SessionPhase::Sleeping;
                ++m_state.asleepTotalMinutes;
                m_state.finalWake = minute.timeEnd;
            }
            m_state.wakeMinuteHits = 0;
            m_state.absentMinuteHits = 0;
            break;
        }

        if (wake_minute)
            ++m_state.wakeMinuteHits;
        if (absent_minute)
            ++m_state.absentMinuteHits;

        if (m_state.wakeMinuteHits >= kWakeMinutesRequired
            || m_state.absentMinuteHits >= kAbsentMinutesRequired)
        {
            SessionCloseResult result;
            result.closed = true;
            result.nightDate = m_state.nightDate.value_or(date_from_timestamp(minute.timeStart));
            result.onset = m_state.onset.value_or(minute.timeStart);
            result.finalWake = m_state.finalWake.value_or(minute.timeStart);
            result.timeInBedS = m_state.inBedMinutes * 60;
            result.asleepTotalS = m_state.asleepTotalMinutes * 60;
            result.tossEvents = m_state.totalTossEvents;
            result.efficiency = result.timeInBedS > 0
                ? static_cast<double>(result.asleepTotalS) / static_cast<double>(result.timeInBedS)
                : 0.0;

            m_state.phase = SessionPhase::SessionClosed;
            return result;
        }
        break;
    }

    if (m_state.phase == SessionPhase::InBedAwake && m_state.onsetMinuteHits > 0)
    {
        if (m_state.onsetMinuteHits >= kOnsetMinutesRequired
            && m_state.onsetMinuteHits >= kOnsetHitsRequired)
        {
            m_state.phase = SessionPhase::Sleeping;
        }
    }

    return std::nullopt;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
