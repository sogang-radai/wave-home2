#include "sleep_vitals.h"

#include "sleep_audio.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    float power_weighted_centroid(
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

    VitalTarget xyz_to_spherical(float x, float y, float z)
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

    // Peak rate (bpm/brpm) in [lo_hz, hi_hz] via DFT on displacement-like series.
    bool peak_rate_bpm(
        const std::deque<float>& series,
        double frame_rate_hz,
        double lo_hz,
        double hi_hz,
        double& out_bpm,
        double& out_confidence,
        double& out_stability)
    {
        out_bpm = 0.0;
        out_confidence = 0.0;
        out_stability = 0.0;
        const size_t n = series.size();
        if (n < 64 || frame_rate_hz <= 0.0)
            return false;

        std::vector<float> x(n);
        double mean = 0.0;
        for (size_t i = 0; i < n; ++i)
            mean += series[i];
        mean /= static_cast<double>(n);
        for (size_t i = 0; i < n; ++i)
            x[i] = static_cast<float>(series[i] - mean);

        const size_t n_freq = n / 2;
        double best_power = 0.0;
        double best_hz = 0.0;
        double band_sum = 0.0;
        double total_sum = 0.0;
        const double two_pi_n = 2.0 * M_PI / static_cast<double>(n);

        for (size_t k = 1; k < n_freq; ++k)
        {
            const double hz = static_cast<double>(k) * frame_rate_hz / static_cast<double>(n);
            std::complex<double> acc(0.0, 0.0);
            for (size_t t = 0; t < n; ++t)
            {
                const double angle = two_pi_n * static_cast<double>(k) * static_cast<double>(t);
                acc += static_cast<double>(x[t])
                    * std::complex<double>(std::cos(angle), -std::sin(angle));
            }
            const double power = std::norm(acc);
            total_sum += power;
            if (hz < lo_hz || hz > hi_hz)
                continue;
            band_sum += power;
            if (power > best_power)
            {
                best_power = power;
                best_hz = hz;
            }
        }

        if (best_hz <= 0.0 || band_sum <= 1e-18)
            return false;

        out_bpm = best_hz * 60.0;
        out_confidence = std::clamp(best_power / (band_sum + 1e-18), 0.0, 1.0);
        // Stability: peak dominates the band.
        out_stability = std::clamp(best_power / (band_sum + 1e-18), 0.0, 1.0);
        (void)total_sum;
        return true;
    }
}

void VitalTargetPicker::update(const dev::RadarPointCloud& frame)
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    const float weight = power_weighted_centroid(frame, x, y, z);
    if (weight <= 0.0f)
    {
        m_target.valid = false;
        return;
    }

    const VitalTarget measured = xyz_to_spherical(x, y, z);
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
    m_phaseDeltas.clear();
    m_havePhase = false;
    m_prevPhase = 0.0f;
}

void VitalSignsProcessor::pushSample(const dev::RadarIQ& iq)
{
    const float phase = std::atan2(iq.imag, iq.real);
    if (!m_havePhase)
    {
        m_prevPhase = phase;
        m_havePhase = true;
        return;
    }

    float delta = phase - m_prevPhase;
    while (delta > static_cast<float>(M_PI))
        delta -= 2.0f * static_cast<float>(M_PI);
    while (delta < -static_cast<float>(M_PI))
        delta += 2.0f * static_cast<float>(M_PI);
    m_prevPhase = phase;
    m_phaseDeltas.push_back(delta);

    while (m_phaseDeltas.size() > kMaxSamples)
        m_phaseDeltas.pop_front();
}

VitalEstimate VitalSignsProcessor::estimate() const
{
    VitalEstimate result;
    if (m_phaseDeltas.size() < kMinSamples)
        return result;

    double br = 0.0;
    double br_conf = 0.0;
    double br_stab = 0.0;
    if (peak_rate_bpm(m_phaseDeltas, m_frameRateHz, 0.10, 0.70, br, br_conf, br_stab))
    {
        if (br >= 6.0 && br <= 42.0 && br_conf >= 0.15)
        {
            result.brRpm = br;
            result.brConfidence = br_conf;
            result.brStability = br_stab;
        }
    }

    double hr = 0.0;
    double hr_conf = 0.0;
    double hr_stab = 0.0;
    if (peak_rate_bpm(m_phaseDeltas, m_frameRateHz, 0.80, 2.50, hr, hr_conf, hr_stab))
    {
        // Heart is weaker on radar displacement; require higher peak dominance.
        if (hr >= 48.0 && hr <= 150.0 && hr_conf >= 0.25)
        {
            result.hrBpm = hr;
            result.hrConfidence = hr_conf * 0.8;
        }
    }

    return result;
}

double estimateSnoreRatio(const ThirtyMinStat& stat, const std::optional<double>& env_temp)
{
    return estimateSnoreRatioFallback(stat.tossMean, env_temp);
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
