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
};

// Comb-filter + bandpass placeholder (see test-iq/docs/vital-signs.md).
class VitalSignsProcessor
{
public:
    void reset();
    void pushSample(const dev::RadarIQ& iq);
    VitalEstimate estimate() const;

private:
    std::deque<float> m_phaseHistory;
    static constexpr size_t kMinSamples = 600;
};

double estimateSnoreRatio(const ThirtyMinStat& stat, const std::optional<double>& env_temp);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
