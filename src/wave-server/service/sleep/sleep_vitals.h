#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "../../device/interface/radar.h"

#include "sleep_aggregator.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct VitalTarget
{
    float azimuth = 0.0f;
    float elevation = 0.0f;
    float distance = 0.0f;
    bool valid = false;
};

/** 1D constant-velocity Kalman for a scalar angle (or distance). */
class ScalarKalman1D
{
public:
    void reset();
    float update(float measurement, float dt_s);

private:
    bool m_initialized = false;
    float m_x = 0.0f;
    float m_v = 0.0f;
    float m_p00 = 1.0f;
    float m_p01 = 0.0f;
    float m_p10 = 0.0f;
    float m_p11 = 1.0f;
};

class VitalTargetPicker
{
public:
    void update(const dev::RadarPointCloud& frame);
    const VitalTarget& target() const { return m_target; }

    /** Firmware range-bin spacing (m) — matches test-iq / CubeMmap. */
    static constexpr float kRangeBinSizeM = 0.07156503945589066f;

private:
    VitalTarget m_target;
    ScalarKalman1D m_azFilter;
    ScalarKalman1D m_elFilter;
    ScalarKalman1D m_distFilter;
    bool m_haveStamp = false;
    std::chrono::steady_clock::time_point m_lastStamp{};
};

struct VitalEstimate
{
    std::optional<double> hrBpm;
    std::optional<double> brRpm;
    double hrConfidence = 0.0;
    double brConfidence = 0.0;
    // 0~1: peak dominance in BR band (for stage synth deep nudge).
    double brStability = 0.0;
    int rangeBin = -1;
};

/**
 * Production vitals path ported from test-iq/vital_signs.py:
 * TI phase unwrap → displacement → bandpass FFT → comb → multi-estimator fusion.
 * Estimates independently on each range bin and picks the highest-confidence bin.
 */
class VitalSignsProcessor
{
public:
    void reset();
    void setFrameRateHz(double hz);
    void pushSample(int range_bin, const dev::RadarIQ& iq);
    VitalEstimate estimate();

    static constexpr int kHalfWidthBins = 2; // center ± 2 → 5 bins
    static constexpr size_t kBufferFrames = 400;
    static constexpr int kFftSize = 2048;

private:
    struct PhaseTracker
    {
        float phasePrev = 0.0f;
        float diffCum = 0.0f;
        float lastUnwrapped = 0.0f;
        bool initialized = false;
    };

    struct BinState
    {
        std::deque<float> phaseDeltas;
        PhaseTracker tracker;
    };

    double m_frameRateHz = 20.0;
    double m_bpmPerBin = 0.0;
    int m_breathStart = 1;
    int m_breathEnd = 2;
    int m_heartBandStart = 1;
    int m_heartBandEnd = 2;
    int m_heartSearchStart = 1;
    int m_heartSearchEnd = 2;
    uint32_t m_frameCount = 0;
    uint32_t m_vsLoop = 0;
    std::unordered_map<int, BinState> m_bins;

    void refreshAnalysisBins();
    std::vector<double> displacementSeries(const BinState& bin, int passes) const;
    bool estimateOnBin(
        const BinState& bin,
        double& out_hr,
        double& out_br,
        double& out_hr_conf,
        double& out_br_conf,
        double& out_br_stab) const;
};

// Legacy toss/temp heuristic (used when mic audio unavailable).
double estimateSnoreRatio(const ThirtyMinStat& stat, const std::optional<double>& env_temp);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
