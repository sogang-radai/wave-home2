#include "sleep_stage_synth.h"

#include <algorithm>
#include <cmath>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    double asleepFraction(const ThirtyMinStat& stat)
    {
        if (!stat.statusRatio.is_object())
            return 0.0;
        return stat.statusRatio.value("asleep", 0.0);
    }

    double moderateFraction(const ThirtyMinStat& stat)
    {
        if (!stat.tossRatio.is_object())
            return 0.0;
        return stat.tossRatio.value("moderate", 0.0);
    }

    int cycleIndex(int32_t asleep_minutes)
    {
        if (asleep_minutes < 0)
            return 0;
        return asleep_minutes / 90;
    }

    double cycleDeepWeight(int index)
    {
        const double t = std::min(1.0, index / 4.0);
        return 1.6 - t * 1.1;
    }

    double cycleRemWeight(int index)
    {
        const double t = std::min(1.0, index / 4.0);
        return 0.5 + t * 1.1;
    }
}

StageSynthResult synthesizeThirtyMinStage(
    const ThirtyMinStat& stat,
    int32_t asleep_minutes_before_window)
{
    StageSynthResult result;
    result.stageRatio = json::object();

    const double asleep_r = asleepFraction(stat);
    if (asleep_r < 0.2)
    {
        if (stat.statusRatio.is_object() && stat.statusRatio.value("absent", 0.0) > 0.5)
            result.stageLabel = "absent";
        else
            result.stageLabel = "awake";
        result.stageRatio[result.stageLabel] = 1.0;
        result.confidence = 0.6;
        return result;
    }

    if (moderateFraction(stat) > 0.15 || stat.tossMean > 0.35)
    {
        result.stageLabel = "light";
        result.stageRatio = {{"light", 0.7}, {"awake", 0.3}};
        result.confidence = 0.55;
        return result;
    }

    const int cycle = cycleIndex(asleep_minutes_before_window);
    const double deep_w = cycleDeepWeight(cycle);
    const double rem_w = cycleRemWeight(cycle);
    const double light_w = 1.0;

    const double sum = deep_w + rem_w + light_w;
    const double deep = deep_w / sum;
    const double rem = rem_w / sum;
    const double light = light_w / sum;

    if (deep >= rem && deep >= light)
        result.stageLabel = "deep";
    else if (rem >= light)
        result.stageLabel = "rem";
    else
        result.stageLabel = "light";

    result.stageRatio = {
        {"light", light},
        {"deep", deep},
        {"rem", rem},
        {"awake", std::max(0.0, 1.0 - asleep_r)},
    };
    result.confidence = 0.45 + asleep_r * 0.35;
    return result;
}

json synthesizeStageTotals(const std::vector<StageSynthResult>& windows)
{
    json totals = json::object();
    for (const auto& window : windows)
    {
        if (!window.stageRatio.is_object())
            continue;
        for (auto it = window.stageRatio.begin(); it != window.stageRatio.end(); ++it)
        {
            const double minutes = 30.0 * it.value().get<double>();
            totals[it.key()] = totals.value(it.key(), 0.0) + minutes;
        }
    }
    return totals;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
