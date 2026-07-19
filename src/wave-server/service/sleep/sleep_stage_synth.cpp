#include "sleep_stage_synth.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr double kLuxAwakeThreshold = 30.0;

    double ratio_field(const json& obj, const char* key)
    {
        if (!obj.is_object())
            return 0.0;
        return obj.value(key, 0.0);
    }

    int cycle_index(int32_t asleep_minutes)
    {
        if (asleep_minutes < 0)
            return 0;
        return asleep_minutes / 90;
    }

    double cycle_deep_weight(int index)
    {
        const double t = std::min(1.0, index / 4.0);
        return 1.6 - t * 1.1;
    }

    double cycle_rem_weight(int index)
    {
        const double t = std::min(1.0, index / 4.0);
        return 0.5 + t * 1.1;
    }

    StageSynthResult make_solid(const std::string& label, double confidence)
    {
        StageSynthResult result;
        result.stageLabel = label;
        result.stageRatio = json::object();
        result.stageRatio[label] = 1.0;
        result.confidence = confidence;
        return result;
    }

    StageSynthResult synthesize_from_ratios(
        double absent_r,
        double awake_r,
        double asleep_r,
        double toss_mean,
        double moderate_r,
        int32_t asleep_minutes_before,
        const StageSynthHints& hints)
    {
        if (hints.envLux && *hints.envLux >= kLuxAwakeThreshold && asleep_r < 0.85)
            return make_solid("awake", 0.65);

        if (asleep_r < 0.2)
        {
            if (absent_r > 0.5)
                return make_solid("absent", 0.6);
            return make_solid("awake", 0.6);
        }

        if (moderate_r > 0.15 || toss_mean > 0.35)
        {
            StageSynthResult result;
            result.stageLabel = "light";
            result.stageRatio = {{"light", 0.7}, {"awake", 0.3}};
            result.confidence = 0.55;
            return result;
        }

        double deep_w = cycle_deep_weight(cycle_index(asleep_minutes_before));
        double rem_w = cycle_rem_weight(cycle_index(asleep_minutes_before));
        double light_w = 1.0;

        // Stable breathing + calm toss nudges deep a little (vitals path).
        if (hints.brStability && *hints.brStability >= 0.7 && toss_mean < 0.2)
            deep_w *= 1.25;
        if (hints.hrConfidence && *hints.hrConfidence < 0.2)
        {
            // Ignore weak HR; no penalty.
        }

        const double sum = deep_w + rem_w + light_w;
        const double deep = deep_w / sum;
        const double rem = rem_w / sum;
        const double light = light_w / sum;

        StageSynthResult result;
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
}

StageSynthResult synthesizeMinuteStage(
    const MinuteStat& stat,
    int32_t asleep_minutes_before,
    const StageSynthHints& hints)
{
    const double absent_r = ratio_field(stat.statusRatio, "absent");
    const double awake_r = ratio_field(stat.statusRatio, "awake");
    const double asleep_r = ratio_field(stat.statusRatio, "asleep");
    const double moderate_r = ratio_field(stat.tossRatio, "moderate");
    return synthesize_from_ratios(
        absent_r,
        awake_r,
        asleep_r,
        stat.tossMean,
        moderate_r,
        asleep_minutes_before,
        hints);
}

StageSynthResult synthesizeThirtyMinStage(
    const ThirtyMinStat& stat,
    int32_t asleep_minutes_before_window,
    const StageSynthHints& hints)
{
    const double absent_r = ratio_field(stat.statusRatio, "absent");
    const double awake_r = ratio_field(stat.statusRatio, "awake");
    const double asleep_r = ratio_field(stat.statusRatio, "asleep");
    const double moderate_r = ratio_field(stat.tossRatio, "moderate");
    return synthesize_from_ratios(
        absent_r,
        awake_r,
        asleep_r,
        stat.tossMean,
        moderate_r,
        asleep_minutes_before_window,
        hints);
}

StageSynthResult aggregateMinuteStages(const std::vector<StageSynthResult>& minutes)
{
    if (minutes.empty())
        return make_solid("absent", 0.0);

    std::unordered_map<std::string, double> counts;
    json ratio_sum = json::object();
    double conf_sum = 0.0;

    for (const auto& m : minutes)
    {
        counts[m.stageLabel] += 1.0;
        conf_sum += m.confidence;
        if (!m.stageRatio.is_object())
            continue;
        for (auto it = m.stageRatio.begin(); it != m.stageRatio.end(); ++it)
        {
            const double v = it.value().get<double>();
            ratio_sum[it.key()] = ratio_sum.value(it.key(), 0.0) + v;
        }
    }

    std::string best = minutes.front().stageLabel;
    double best_n = 0.0;
    for (const auto& [label, n] : counts)
    {
        if (n > best_n)
        {
            best_n = n;
            best = label;
        }
    }

    StageSynthResult result;
    result.stageLabel = best;
    result.confidence = conf_sum / static_cast<double>(minutes.size());
    result.stageRatio = json::object();
    const double inv = 1.0 / static_cast<double>(minutes.size());
    for (auto it = ratio_sum.begin(); it != ratio_sum.end(); ++it)
        result.stageRatio[it.key()] = it.value().get<double>() * inv;
    if (result.stageRatio.empty())
        result.stageRatio[best] = 1.0;
    return result;
}

json synthesizeStageTotals(const std::vector<StageSynthResult>& windows)
{
    return synthesizeStageTotals(windows, 30.0);
}

json synthesizeStageTotals(
    const std::vector<StageSynthResult>& windows,
    double minutes_per_window)
{
    json totals = json::object();
    for (const auto& window : windows)
    {
        if (!window.stageRatio.is_object())
            continue;
        for (auto it = window.stageRatio.begin(); it != window.stageRatio.end(); ++it)
        {
            const double minutes = minutes_per_window * it.value().get<double>();
            totals[it.key()] = totals.value(it.key(), 0.0) + minutes;
        }
    }
    return totals;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
