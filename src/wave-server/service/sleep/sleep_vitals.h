#pragma once

#include <cstdint>
#include <deque>
#include <optional>
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

class VitalTargetPicker
{
public:
    void update(const dev::RadarPointCloud& frame);
    const VitalTarget& target() const { return m_target; }

private:
    VitalTarget m_target;
};

struct VitalEstimate
{
    std::optional<double> hrBpm;
    std::optional<double> brRpm;
    double hrConfidence = 0.0;
    double brConfidence = 0.0;
    // 0~1: inverse of BR spectral spread (for stage synth deep nudge).
    double brStability = 0.0;
};

// Abbreviated test-iq vital-signs path: phase unwrap deltas -> band FFT peak.
class VitalSignsProcessor
{
public:
    void reset();
    void pushSample(const dev::RadarIQ& iq);
    void setFrameRateHz(double hz) { m_frameRateHz = hz > 1.0 ? hz : 20.0; }
    VitalEstimate estimate() const;

private:
    std::deque<float> m_phaseDeltas;
    float m_prevPhase = 0.0f;
    bool m_havePhase = false;
    double m_frameRateHz = 20.0;

    static constexpr size_t kMinSamples = 200;   // ~10 s @ 20 Hz
    static constexpr size_t kMaxSamples = 400;   // ~20 s
};

// Legacy toss/temp heuristic (used when mic audio unavailable).
double estimateSnoreRatio(const ThirtyMinStat& stat, const std::optional<double>& env_temp);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
