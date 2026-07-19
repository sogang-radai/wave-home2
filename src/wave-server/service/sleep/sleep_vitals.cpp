#include "sleep_vitals.h"

#include "sleep_audio.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

#include "kiss_fftr.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kWavelengthM = 3.89e-3;
    constexpr double kPhaseToDisplacement = kWavelengthM / (4.0 * kPi);

    constexpr double kBreathLoHz = 0.10;
    constexpr double kBreathHiHz = 0.70;
    constexpr double kHeartBandLoHz = 0.80;
    constexpr double kHeartBandHiHz = 3.33;
    constexpr double kHeartSearchLoHz = 0.80;
    constexpr double kHeartSearchHiHz = 2.50;

    constexpr double kBreathWeights[3] = {0.45, 0.35, 0.20};
    constexpr double kHeartWeights[6] = {0.20, 0.15, 0.15, 0.20, 0.15, 0.15};

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
        const float range_xy = std::hypot(x, y);
        const float range = std::sqrt(x * x + y * y + z * z);
        if (range < 0.05f)
            return target;

        target.distance = range;
        target.azimuth = std::atan2(x, y);
        target.elevation = std::atan2(z, range_xy);
        target.valid = true;
        return target;
    }

    std::pair<float, float> compute_phase_unwrap(float phase, float phase_prev, float diff_cum)
    {
        float diff_phase = phase - phase_prev;
        float mod_factor = 0.0f;
        if (diff_phase > static_cast<float>(kPi))
            mod_factor = 1.0f;
        else if (diff_phase < -static_cast<float>(kPi))
            mod_factor = -1.0f;

        float diff_phase_mod = diff_phase - mod_factor * 2.0f * static_cast<float>(kPi);
        if (diff_phase_mod == -static_cast<float>(kPi) && diff_phase > 0.0f)
            diff_phase_mod = static_cast<float>(kPi);

        float diff_phase_correction = diff_phase_mod - diff_phase;
        if ((diff_phase_correction > 0.0f && diff_phase_correction < static_cast<float>(kPi))
            || (diff_phase_correction < 0.0f && diff_phase_correction > -static_cast<float>(kPi)))
        {
            diff_phase_correction = 0.0f;
        }

        diff_cum += diff_phase_correction;
        return {phase + diff_cum, diff_cum};
    }

    void moving_average_inplace(std::vector<double>& x, int window, int passes)
    {
        if (x.size() < 2 || window < 2 || passes < 1)
            return;
        std::vector<double> tmp(x.size());
        for (int pass = 0; pass < passes; ++pass)
        {
            for (size_t i = 0; i < x.size(); ++i)
            {
                double sum = 0.0;
                int count = 0;
                const int half = window / 2;
                for (int k = -half; k <= half; ++k)
                {
                    const int j = static_cast<int>(i) + k;
                    if (j < 0 || j >= static_cast<int>(x.size()))
                        continue;
                    sum += x[static_cast<size_t>(j)];
                    ++count;
                }
                tmp[i] = count > 0 ? sum / count : x[i];
            }
            x.swap(tmp);
        }
    }

    int hz_to_bin(double hz, double scale_hz, int half)
    {
        if (scale_hz <= 0.0)
            return 0;
        return std::clamp(static_cast<int>(std::lround(hz / scale_hz)), 1, half - 1);
    }

    std::vector<std::complex<double>> rfft(const std::vector<double>& x)
    {
        const int n = static_cast<int>(x.size());
        std::vector<std::complex<double>> out(static_cast<size_t>(n / 2 + 1), {0.0, 0.0});
        if (n < 2)
            return out;

        kiss_fftr_cfg cfg = kiss_fftr_alloc(n, 0, nullptr, nullptr);
        if (!cfg)
            return out;

        std::vector<kiss_fft_scalar> timedata(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            timedata[static_cast<size_t>(i)] = static_cast<kiss_fft_scalar>(x[static_cast<size_t>(i)]);

        std::vector<kiss_fft_cpx> freqdata(static_cast<size_t>(n / 2 + 1));
        kiss_fftr(cfg, timedata.data(), freqdata.data());
        for (size_t i = 0; i < freqdata.size(); ++i)
            out[i] = {freqdata[i].r, freqdata[i].i};
        kiss_fftr_free(cfg);
        return out;
    }

    std::vector<double> irfft(const std::vector<std::complex<double>>& spec, int n)
    {
        std::vector<double> out(static_cast<size_t>(n), 0.0);
        if (n < 2 || spec.empty())
            return out;

        kiss_fftr_cfg cfg = kiss_fftr_alloc(n, 1, nullptr, nullptr);
        if (!cfg)
            return out;

        std::vector<kiss_fft_cpx> freqdata(static_cast<size_t>(n / 2 + 1));
        for (size_t i = 0; i < freqdata.size(); ++i)
        {
            if (i < spec.size())
            {
                freqdata[i].r = static_cast<kiss_fft_scalar>(spec[i].real());
                freqdata[i].i = static_cast<kiss_fft_scalar>(spec[i].imag());
            }
        }
        std::vector<kiss_fft_scalar> timedata(static_cast<size_t>(n));
        kiss_fftri(cfg, freqdata.data(), timedata.data());
        for (int i = 0; i < n; ++i)
            out[static_cast<size_t>(i)] = timedata[static_cast<size_t>(i)] / static_cast<double>(n);
        kiss_fftr_free(cfg);
        return out;
    }

    std::vector<double> bandpass_fft(
        const std::vector<double>& x,
        double frame_rate_hz,
        double lo_hz,
        double hi_hz)
    {
        if (x.size() < 8 || frame_rate_hz <= 0.0)
            return x;
        auto spec = rfft(x);
        const int n = static_cast<int>(x.size());
        for (size_t k = 0; k < spec.size(); ++k)
        {
            const double hz = static_cast<double>(k) * frame_rate_hz / static_cast<double>(n);
            if (hz < lo_hz || hz > hi_hz)
                spec[k] = {0.0, 0.0};
        }
        return irfft(spec, n);
    }

    std::vector<double> power_spectrum(const std::vector<double>& x, int fft_size)
    {
        std::vector<double> padded(static_cast<size_t>(fft_size), 0.0);
        const size_t n = std::min(x.size(), static_cast<size_t>(fft_size));
        for (size_t i = 0; i < n; ++i)
            padded[i] = x[i];

        kiss_fft_cfg cfg = kiss_fft_alloc(fft_size, 0, nullptr, nullptr);
        std::vector<double> power(static_cast<size_t>(fft_size), 0.0);
        if (!cfg)
            return power;

        std::vector<kiss_fft_cpx> tin(static_cast<size_t>(fft_size));
        std::vector<kiss_fft_cpx> tout(static_cast<size_t>(fft_size));
        for (int i = 0; i < fft_size; ++i)
        {
            tin[static_cast<size_t>(i)].r = static_cast<kiss_fft_scalar>(padded[static_cast<size_t>(i)]);
            tin[static_cast<size_t>(i)].i = 0;
        }
        kiss_fft(cfg, tin.data(), tout.data());
        for (int i = 0; i < fft_size; ++i)
        {
            const double re = tout[static_cast<size_t>(i)].r;
            const double im = tout[static_cast<size_t>(i)].i;
            power[static_cast<size_t>(i)] = re * re + im * im;
        }
        kiss_fft_free(cfg);
        return power;
    }

    std::vector<int> top_peak_bins(const std::vector<double>& spectrum, int start, int end, int count)
    {
        start = std::max(1, start);
        end = std::min(static_cast<int>(spectrum.size()) - 1, end);
        std::vector<int> peaks;
        if (end <= start)
            return {start};

        std::vector<double> region(spectrum.begin() + start, spectrum.begin() + end);
        for (int c = 0; c < std::max(1, count); ++c)
        {
            if (region.empty())
                break;
            const auto it = std::max_element(region.begin(), region.end());
            const int local = static_cast<int>(std::distance(region.begin(), it));
            peaks.push_back(start + local);
            const int lo = std::max(0, local - 1);
            const int hi = std::min(static_cast<int>(region.size()), local + 2);
            for (int i = lo; i < hi; ++i)
                region[static_cast<size_t>(i)] = 0.0;
        }
        if (peaks.empty())
            peaks.push_back(start);
        return peaks;
    }

    double fft_rate_bin(const std::vector<double>& spectrum, int start, int end, int top_k)
    {
        const auto peaks = top_peak_bins(spectrum, start, end, top_k);
        double num = 0.0;
        double den = 0.0;
        for (int p : peaks)
        {
            const double w = std::max(spectrum[static_cast<size_t>(p)], 1e-12);
            num += static_cast<double>(p) * w;
            den += w;
        }
        return den > 0.0 ? num / den : static_cast<double>(start);
    }

    double spectrum_scale(double frame_rate_hz, int fft_size)
    {
        if (fft_size <= 0 || frame_rate_hz <= 0.0)
            return 0.0;
        return frame_rate_hz * 60.0 / static_cast<double>(fft_size);
    }

    double autocorr_rate_bin(
        const std::vector<double>& x,
        double frame_rate_hz,
        double lo_hz,
        double hi_hz,
        int fft_size)
    {
        if (x.size() < 16 || frame_rate_hz <= 0.0)
            return 0.0;
        const double mean = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
        std::vector<double> xc(x.size());
        for (size_t i = 0; i < x.size(); ++i)
            xc[i] = x[i] - mean;

        const int n = static_cast<int>(xc.size());
        std::vector<double> corr(static_cast<size_t>(n), 0.0);
        for (int lag = 0; lag < n; ++lag)
        {
            double sum = 0.0;
            for (int i = 0; i + lag < n; ++i)
                sum += xc[static_cast<size_t>(i)] * xc[static_cast<size_t>(i + lag)];
            corr[static_cast<size_t>(lag)] = sum;
        }

        const int min_lag = std::max(1, static_cast<int>(frame_rate_hz / hi_hz));
        const int max_lag = std::min(n - 1, std::max(min_lag + 1, static_cast<int>(frame_rate_hz / lo_hz)));
        if (max_lag <= min_lag)
            return 0.0;

        int best = min_lag;
        double best_v = corr[static_cast<size_t>(min_lag)];
        for (int lag = min_lag + 1; lag <= max_lag; ++lag)
        {
            if (corr[static_cast<size_t>(lag)] > best_v)
            {
                best_v = corr[static_cast<size_t>(lag)];
                best = lag;
            }
        }
        if (best <= 0)
            return 0.0;
        const double hz = frame_rate_hz / static_cast<double>(best);
        const double scale = spectrum_scale(frame_rate_hz, fft_size);
        return scale > 0.0 ? hz * 60.0 / scale : 0.0;
    }

    double peak_count_rate_bin(
        const std::vector<double>& x,
        double frame_rate_hz,
        double lo_hz,
        double hi_hz,
        int fft_size)
    {
        if (x.size() < 16 || frame_rate_hz <= 0.0)
            return 0.0;
        const double mean = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
        std::vector<double> xc(x.size());
        double max_abs = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            xc[i] = x[i] - mean;
            max_abs = std::max(max_abs, std::abs(xc[i]));
        }
        const double thr = 0.2 * (max_abs + 1e-12);
        int peaks = 0;
        for (size_t i = 1; i + 1 < xc.size(); ++i)
        {
            if (xc[i] > xc[i - 1] && xc[i] >= xc[i + 1] && xc[i] > thr)
                ++peaks;
        }
        const double duration_s = static_cast<double>(x.size()) / frame_rate_hz;
        if (duration_s <= 0.0)
            return 0.0;
        double hz = static_cast<double>(peaks) / duration_s;
        hz = std::clamp(hz, lo_hz, hi_hz);
        const double scale = spectrum_scale(frame_rate_hz, fft_size);
        return scale > 0.0 ? hz * 60.0 / scale : 0.0;
    }

    std::vector<double> comb_notch_time(
        const std::vector<double>& x,
        double breath_hz,
        double frame_rate_hz,
        int harmonics = 3)
    {
        if (x.size() < 8 || breath_hz <= 0.0 || frame_rate_hz <= 0.0)
            return x;
        auto spec = rfft(x);
        const int n = static_cast<int>(x.size());
        const double width = std::max(breath_hz * 0.15, frame_rate_hz / static_cast<double>(n));
        for (int h = 1; h <= harmonics; ++h)
        {
            const double f0 = breath_hz * h;
            for (size_t k = 0; k < spec.size(); ++k)
            {
                const double hz = static_cast<double>(k) * frame_rate_hz / static_cast<double>(n);
                if (hz >= f0 - width && hz <= f0 + width)
                    spec[k] = {0.0, 0.0};
            }
        }
        return irfft(spec, n);
    }

    std::vector<double> comb_notch_spectrum(
        const std::vector<double>& spectrum,
        int breath_bin,
        int harmonics = 3)
    {
        auto out = spectrum;
        if (breath_bin <= 0)
            return out;
        for (int h = 1; h <= harmonics; ++h)
        {
            const int center = breath_bin * h;
            const int lo = std::max(0, center - 2);
            const int hi = std::min(static_cast<int>(out.size()), center + 3);
            for (int i = lo; i < hi; ++i)
                out[static_cast<size_t>(i)] = 0.0;
        }
        return out;
    }

    double band_peak_dominance(const std::vector<double>& spectrum, int start, int end)
    {
        start = std::max(1, start);
        end = std::min(static_cast<int>(spectrum.size()) - 1, end);
        if (end <= start)
            return 0.0;
        double best = 0.0;
        double sum = 0.0;
        for (int i = start; i < end; ++i)
        {
            sum += spectrum[static_cast<size_t>(i)];
            best = std::max(best, spectrum[static_cast<size_t>(i)]);
        }
        return sum > 1e-18 ? std::clamp(best / sum, 0.0, 1.0) : 0.0;
    }
}

void ScalarKalman1D::reset()
{
    m_initialized = false;
    m_x = 0.0f;
    m_v = 0.0f;
    m_p00 = 1.0f;
    m_p01 = 0.0f;
    m_p10 = 0.0f;
    m_p11 = 1.0f;
}

float ScalarKalman1D::update(float measurement, float dt_s)
{
    // Process noise / measurement noise tuned for ~20 Hz radar centroids.
    constexpr float q_pos = 1e-4f;
    constexpr float q_vel = 1e-3f;
    constexpr float r_meas = 2.5e-3f;

    if (!m_initialized)
    {
        m_x = measurement;
        m_v = 0.0f;
        m_initialized = true;
        return m_x;
    }

    const float dt = std::clamp(dt_s, 1e-3f, 0.5f);

    // Predict.
    const float x_pred = m_x + m_v * dt;
    const float v_pred = m_v;
    const float p00 = m_p00 + dt * (m_p10 + m_p01) + dt * dt * m_p11 + q_pos;
    const float p01 = m_p01 + dt * m_p11;
    const float p10 = m_p10 + dt * m_p11;
    const float p11 = m_p11 + q_vel;

    // Update (H = [1, 0]).
    const float y = measurement - x_pred;
    const float s = p00 + r_meas;
    const float k0 = p00 / s;
    const float k1 = p10 / s;

    m_x = x_pred + k0 * y;
    m_v = v_pred + k1 * y;
    m_p00 = (1.0f - k0) * p00;
    m_p01 = (1.0f - k0) * p01;
    m_p10 = p10 - k1 * p00;
    m_p11 = p11 - k1 * p01;
    return m_x;
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
    if (!measured.valid)
    {
        m_target.valid = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float dt = 0.05f;
    if (m_haveStamp)
    {
        dt = std::chrono::duration<float>(now - m_lastStamp).count();
        if (dt < 1e-3f)
            dt = 1e-3f;
    }
    m_lastStamp = now;
    m_haveStamp = true;

    if (!m_target.valid)
    {
        m_azFilter.reset();
        m_elFilter.reset();
        m_distFilter.reset();
    }

    m_target.azimuth = m_azFilter.update(measured.azimuth, dt);
    m_target.elevation = m_elFilter.update(measured.elevation, dt);
    m_target.distance = m_distFilter.update(measured.distance, dt);
    m_target.valid = true;
}

void VitalSignsProcessor::setFrameRateHz(double hz)
{
    m_frameRateHz = hz > 1.0 ? hz : 20.0;
    refreshAnalysisBins();
}

void VitalSignsProcessor::refreshAnalysisBins()
{
    m_bpmPerBin = spectrum_scale(m_frameRateHz, kFftSize);
    const double scale_hz = m_frameRateHz / static_cast<double>(kFftSize);
    const int half = kFftSize / 2;
    m_breathStart = hz_to_bin(kBreathLoHz, scale_hz, half);
    m_breathEnd = std::max(m_breathStart + 1, hz_to_bin(kBreathHiHz, scale_hz, half));
    m_heartBandStart = hz_to_bin(kHeartBandLoHz, scale_hz, half);
    m_heartBandEnd = std::max(m_heartBandStart + 1, hz_to_bin(kHeartBandHiHz, scale_hz, half));
    m_heartSearchStart = std::max(m_heartBandStart, hz_to_bin(kHeartSearchLoHz, scale_hz, half));
    m_heartSearchEnd = std::min(m_heartBandEnd, std::max(m_heartSearchStart + 1, hz_to_bin(kHeartSearchHiHz, scale_hz, half)));
}

void VitalSignsProcessor::reset()
{
    m_bins.clear();
    m_frameCount = 0;
    m_vsLoop = 0;
    refreshAnalysisBins();
}

void VitalSignsProcessor::pushSample(int range_bin, const dev::RadarIQ& iq)
{
    if (m_bpmPerBin <= 0.0)
        refreshAnalysisBins();

    auto& bin = m_bins[range_bin];
    const float phase = std::atan2(iq.imag, iq.real);
    auto& tracker = bin.tracker;
    if (!tracker.initialized)
    {
        tracker.phasePrev = phase;
        tracker.diffCum = 0.0f;
        tracker.lastUnwrapped = phase;
        tracker.initialized = true;
        ++m_frameCount;
        return;
    }

    const auto [unwrapped, new_cum] = compute_phase_unwrap(phase, tracker.phasePrev, tracker.diffCum);
    tracker.diffCum = new_cum;
    const float delta_psi = unwrapped - tracker.lastUnwrapped;
    tracker.lastUnwrapped = unwrapped;
    tracker.phasePrev = phase;
    bin.phaseDeltas.push_back(delta_psi);
    while (bin.phaseDeltas.size() > kBufferFrames - 1)
        bin.phaseDeltas.pop_front();
    ++m_frameCount;
}

std::vector<double> VitalSignsProcessor::displacementSeries(const BinState& bin, int passes) const
{
    if (bin.phaseDeltas.size() < 2)
        return {};

    std::vector<double> delta(bin.phaseDeltas.begin(), bin.phaseDeltas.end());
    const double mean = std::accumulate(delta.begin(), delta.end(), 0.0) / static_cast<double>(delta.size());
    for (double& v : delta)
        v -= mean;
    moving_average_inplace(delta, 5, passes);
    for (double& v : delta)
        v *= kPhaseToDisplacement;
    return delta;
}

bool VitalSignsProcessor::estimateOnBin(
    const BinState& bin,
    double& out_hr,
    double& out_br,
    double& out_hr_conf,
    double& out_br_conf,
    double& out_br_stab) const
{
    out_hr = 0.0;
    out_br = 0.0;
    out_hr_conf = 0.0;
    out_br_conf = 0.0;
    out_br_stab = 0.0;

    const auto displacement = displacementSeries(bin, 8);
    if (displacement.size() < 64)
        return false;

    const auto breath_wave = bandpass_fft(displacement, m_frameRateHz, kBreathLoHz, kBreathHiHz);
    const auto heart_wave = bandpass_fft(displacement, m_frameRateHz, kHeartBandLoHz, kHeartBandHiHz);
    const auto breath_spec = power_spectrum(breath_wave, kFftSize);

    const double breath_bin = kBreathWeights[0] * fft_rate_bin(breath_spec, m_breathStart, m_breathEnd, 4)
        + kBreathWeights[1] * autocorr_rate_bin(breath_wave, m_frameRateHz, kBreathLoHz, kBreathHiHz, kFftSize)
        + kBreathWeights[2] * peak_count_rate_bin(breath_wave, m_frameRateHz, kBreathLoHz, kBreathHiHz, kFftSize);

    const double breath_hz = breath_bin * m_bpmPerBin / 60.0;
    out_br = breath_bin * m_bpmPerBin;
    out_br_conf = band_peak_dominance(breath_spec, m_breathStart, m_breathEnd);
    out_br_stab = out_br_conf;

    const auto spec_pre = power_spectrum(heart_wave, kFftSize);
    const double fft_pre = fft_rate_bin(spec_pre, m_heartSearchStart, m_heartSearchEnd, 6);
    const double ac_pre = autocorr_rate_bin(heart_wave, m_frameRateHz, kHeartSearchLoHz, kHeartSearchHiHz, kFftSize);
    const double pk_pre = peak_count_rate_bin(heart_wave, m_frameRateHz, kHeartSearchLoHz, kHeartSearchHiHz, kFftSize);

    const auto notched = comb_notch_time(heart_wave, breath_hz, m_frameRateHz);
    const int breath_bin_i = static_cast<int>(std::lround(breath_hz * 60.0 / std::max(m_bpmPerBin, 1e-9)));
    const auto spec_post = comb_notch_spectrum(power_spectrum(notched, kFftSize), breath_bin_i);
    const double fft_post = fft_rate_bin(spec_post, m_heartSearchStart, m_heartSearchEnd, 6);
    const double ac_post = autocorr_rate_bin(notched, m_frameRateHz, kHeartSearchLoHz, kHeartSearchHiHz, kFftSize);
    const double pk_post = peak_count_rate_bin(notched, m_frameRateHz, kHeartSearchLoHz, kHeartSearchHiHz, kFftSize);

    const double heart_bin = kHeartWeights[0] * fft_pre + kHeartWeights[1] * ac_pre + kHeartWeights[2] * pk_pre
        + kHeartWeights[3] * fft_post + kHeartWeights[4] * ac_post + kHeartWeights[5] * pk_post;
    out_hr = heart_bin * m_bpmPerBin;
    out_hr_conf = band_peak_dominance(spec_post, m_heartSearchStart, m_heartSearchEnd);
    return true;
}

VitalEstimate VitalSignsProcessor::estimate()
{
    VitalEstimate result;
    if (m_bins.empty())
        return result;

    size_t min_fill = kBufferFrames;
    for (const auto& [rb, bin] : m_bins)
    {
        (void)rb;
        min_fill = std::min(min_fill, bin.phaseDeltas.size());
    }
    // Need ~full buffer fill across tracked bins before publishing rates.
    if (min_fill + 1 < kBufferFrames / 2)
        return result;

    double best_score = -1.0;
    for (const auto& [rb, bin] : m_bins)
    {
        double hr = 0.0;
        double br = 0.0;
        double hr_conf = 0.0;
        double br_conf = 0.0;
        double br_stab = 0.0;
        if (!estimateOnBin(bin, hr, br, hr_conf, br_conf, br_stab))
            continue;

        const double score = br_conf * 0.55 + hr_conf * 0.45;
        if (score <= best_score)
            continue;
        best_score = score;
        result.rangeBin = rb;
        result.brStability = br_stab;
        result.brConfidence = br_conf;
        result.hrConfidence = hr_conf * 0.8;

        if (br >= 6.0 && br <= 42.0 && br_conf >= 0.12)
            result.brRpm = br;
        else
            result.brRpm.reset();

        if (hr >= 48.0 && hr <= 150.0 && hr_conf >= 0.18)
            result.hrBpm = hr;
        else
            result.hrBpm.reset();
    }

    ++m_vsLoop;
    // Warmup: suppress first few published estimates (matches test-iq WARMUP_LOOPS spirit).
    if (m_vsLoop < 7)
    {
        result.hrBpm.reset();
        result.brRpm.reset();
        result.hrConfidence = 0.0;
        result.brConfidence = 0.0;
    }
    return result;
}

double estimateSnoreRatio(const ThirtyMinStat& stat, const std::optional<double>& env_temp)
{
    return estimateSnoreRatioFallback(stat.tossMean, env_temp);
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
