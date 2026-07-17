#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../core/json.h"

#include "sleep_aggregator.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct StageSynthResult
{
    std::string stageLabel;
    json stageRatio;
    double confidence = 0.0;
};

// Optional extras for explainable heuristics (ignored when unset).
struct StageSynthHints
{
    std::optional<double> envLux;
    std::optional<double> brStability;   // 0~1, higher = more stable breathing
    std::optional<double> hrConfidence;  // 0~1
};

// 1-minute stage synthesis from SleepNet minute aggregates + optional hints.
StageSynthResult synthesizeMinuteStage(
    const MinuteStat& stat,
    int32_t asleep_minutes_before,
    const StageSynthHints& hints = {});

// 30m window stage synthesis (heuristic; prefers aggregateMinuteStages when available).
StageSynthResult synthesizeThirtyMinStage(
    const ThirtyMinStat& stat,
    int32_t asleep_minutes_before_window,
    const StageSynthHints& hints = {});

// Majority / ratio aggregate of 1m stage labels into one 30m result.
StageSynthResult aggregateMinuteStages(const std::vector<StageSynthResult>& minutes);

json synthesizeStageTotals(const std::vector<StageSynthResult>& windows);
// When windows are 1-minute results, pass minutes_per_window=1.0 (default 30 for legacy 30m windows).
json synthesizeStageTotals(
    const std::vector<StageSynthResult>& windows,
    double minutes_per_window);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
