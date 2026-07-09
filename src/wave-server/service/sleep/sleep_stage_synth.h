#pragma once

#include <cstdint>
#include <string>

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

// 30m window stage synthesis (90-minute cycle heuristic; see mock/sleep.md).
StageSynthResult synthesizeThirtyMinStage(
    const ThirtyMinStat& stat,
    int32_t asleep_minutes_before_window);

json synthesizeStageTotals(const std::vector<StageSynthResult>& windows);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
