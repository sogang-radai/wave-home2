#include "sleep_vitals.h"

#include <algorithm>
#include <cmath>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr float kHumanIdMax = 254.0f;

    float powerWeightedCentroid(
        const dev::RadarPointCloud& frame,
        float& out_x,
        float& out_y,
        float& out_z)
    {
        double weight_sum = 0.0;
        double x_sum = 0.0;
        double y_sum = 0.0;
        double z_sum = 0.0;

        for (const auto& target : frame.targets)
        {
            if (target.targetId > 254)
                continue;

            for (uint16_t idx : target.pointIndices)
            {
                if (idx >= frame.points.size())
                    continue;
                const auto& point = frame.points[idx];
                const double weight = std::max(1.0f, point.power);
                x_sum += point.x * weight;
                y_sum += point.y * weight;
                z_sum += point.z * weight;
                weight_sum += weight;
            }
        }

        if (weight_sum <= 0.0)
            return 0.0f;

        out_x = static_cast<float>(x_sum / weight_sum);
        out_y = static_cast<float>(y_sum / weight_sum);
        out_z = static_cast<float>(z_sum / weight_sum);
        return static_cast<float>(weight_sum);
    }

    VitalTarget xyzToSpherical(float x, float y, float z)
    {
        VitalTarget target;
        const float range = std::sqrt(x * x + y * y + z * z);
        if (range < 0.05f)
            return target;

        target.distance = range;
        target.azimuth = std::asin(std::clamp(x / range, -1.0f, 1.0f));
        target.elevation = std::asin(std::clamp(z / range, -1.0f, 1.0f));
        target.valid = true;
        return target;
    }
}

void VitalTargetPicker::update(const dev::RadarPointCloud& frame)
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    const float weight = powerWeightedCentroid(frame, x, y, z);
    if (weight <= 0.0f)
    {
        m_target.valid = false;
        return;
    }

    const VitalTarget measured = xyzToSpherical(x, y, z);
    if (!m_target.valid)
    {
        m_target = measured;
        return;
    }

    constexpr float alpha = 0.2f;
    m_target.azimuth = m_target.azimuth * (1.0f - alpha) + measured.azimuth * alpha;
    m_target.elevation = m_target.elevation * (1.0f - alpha) + measured.elevation * alpha;
    m_target.distance = m_target.distance * (1.0f - alpha) + measured.distance * alpha;
    m_target.valid = true;
}

void VitalSignsProcessor::reset()
{
    m_phaseHistory.clear();
}

void VitalSignsProcessor::pushSample(const dev::RadarIQ& iq)
{
    const float phase = std::atan2(iq.imag, iq.real);
    if (!m_phaseHistory.empty())
    {
        float delta = phase - m_phaseHistory.back();
        while (delta > static_cast<float>(M_PI))
            delta -= 2.0f * static_cast<float>(M_PI);
        while (delta < -static_cast<float>(M_PI))
            delta += 2.0f * static_cast<float>(M_PI);
        m_phaseHistory.push_back(delta);
    }
    else
    {
        m_phaseHistory.push_back(phase);
    }

    while (m_phaseHistory.size() > kMinSamples * 2)
        m_phaseHistory.pop_front();
}

VitalEstimate VitalSignsProcessor::estimate() const
{
    VitalEstimate result;
    if (m_phaseHistory.size() < kMinSamples)
        return result;

    // Placeholder: real comb-filter + bandpass DSP lands here.
    result.hrConfidence = 0.0;
    result.brConfidence = 0.0;
    return result;
}

double estimateSnoreRatio(const ThirtyMinStat& stat, const std::optional<double>& env_temp)
{
    double base = 0.12;
    if (stat.tossMean > 0.2)
        base -= stat.tossMean * 0.3;

    if (env_temp)
        base += std::max(0.0, *env_temp - 25.0) * 0.08;

    return std::clamp(base, 0.0, 1.0);
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
