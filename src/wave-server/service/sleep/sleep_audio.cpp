#include "sleep_audio.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "kiss_fftr.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    double rms_db(const float* samples, size_t n)
    {
        if (n == 0)
            return -90.0;
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i)
            sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
        const double rms = std::sqrt(sum / static_cast<double>(n));
        if (rms <= 1e-9)
            return -90.0;
        return 20.0 * std::log10(rms);
    }

    /** Hann-windowed real FFT band power via kiss_fftr (same snore-band ratio as before). */
    void band_energy_kissfft(
        const float* samples,
        size_t n,
        uint32_t sample_rate,
        double lo_hz,
        double hi_hz,
        double& band_out,
        double& total_out)
    {
        band_out = 0.0;
        total_out = 0.0;
        if (n < 8 || sample_rate == 0 || (n & 1u) != 0)
            return;

        kiss_fftr_cfg cfg = kiss_fftr_alloc(static_cast<int>(n), 0, nullptr, nullptr);
        if (!cfg)
            return;

        std::vector<kiss_fft_scalar> timedata(n);
        const double two_pi_nm1 = 2.0 * M_PI / static_cast<double>(n - 1);
        for (size_t t = 0; t < n; ++t)
        {
            const double hann = 0.5 * (1.0 - std::cos(two_pi_nm1 * static_cast<double>(t)));
            timedata[t] = static_cast<kiss_fft_scalar>(static_cast<double>(samples[t]) * hann);
        }

        std::vector<kiss_fft_cpx> freqdata(n / 2 + 1);
        kiss_fftr(cfg, timedata.data(), freqdata.data());
        kiss_fftr_free(cfg);

        const double hz_per_bin = static_cast<double>(sample_rate) / static_cast<double>(n);
        // Skip DC (k=0); use positive frequencies only.
        for (size_t k = 1; k < freqdata.size(); ++k)
        {
            const double hz = static_cast<double>(k) * hz_per_bin;
            if (hz > static_cast<double>(sample_rate) * 0.5)
                break;
            const double re = static_cast<double>(freqdata[k].r);
            const double im = static_cast<double>(freqdata[k].i);
            const double power = re * re + im * im;
            total_out += power;
            if (hz >= lo_hz && hz <= hi_hz)
                band_out += power;
        }
    }
}

void SnoreAudioAnalyzer::reset()
{
    m_pcm.clear();
    m_latest = {};
    m_snoreSum = 0.0;
    m_noiseSum = 0.0;
    m_windowCount = 0;
    m_activeCount = 0;
}

void SnoreAudioAnalyzer::beginMinute()
{
    m_snoreSum = 0.0;
    m_noiseSum = 0.0;
    m_windowCount = 0;
    m_activeCount = 0;
}

void SnoreAudioAnalyzer::pushFrame(const dev::AudioFrame& frame, uint32_t sample_rate_hz)
{
    if (frame.samples.empty())
        return;
    if (sample_rate_hz > 0)
        m_sampleRate = sample_rate_hz;

    m_pcm.reserve(m_pcm.size() + frame.samples.size());
    for (int16_t s : frame.samples)
        m_pcm.push_back(static_cast<float>(s) / 32768.0f);

    while (m_pcm.size() > kMaxPcmSamples)
        m_pcm.erase(m_pcm.begin(), m_pcm.begin() + static_cast<ptrdiff_t>(m_pcm.size() - kMaxPcmSamples));

    while (m_pcm.size() >= kWindowSamples)
    {
        analyzeWindow();
        // 50% hop
        m_pcm.erase(m_pcm.begin(), m_pcm.begin() + static_cast<ptrdiff_t>(kWindowSamples / 2));
    }
}

void SnoreAudioAnalyzer::analyzeWindow()
{
    if (m_pcm.size() < kWindowSamples)
        return;

    double band = 0.0;
    double total = 0.0;
    band_energy_kissfft(
        m_pcm.data(),
        kWindowSamples,
        m_sampleRate,
        kSnoreHzLo,
        kSnoreHzHi,
        band,
        total);

    const double score = (total > 1e-12) ? std::clamp(band / total, 0.0, 1.0) : 0.0;
    const double noise = rms_db(m_pcm.data(), kWindowSamples);
    const bool active = score >= kActiveThreshold && noise > -55.0;

    m_latest.snoreScore = score;
    m_latest.noiseDb = noise;
    m_latest.snoreActive = active;

    m_snoreSum += score;
    m_noiseSum += noise;
    ++m_windowCount;
    if (active)
        ++m_activeCount;
}

SnoreAudioSample SnoreAudioAnalyzer::flushMinute()
{
    SnoreAudioSample out;
    if (m_windowCount <= 0)
    {
        out = m_latest;
        return out;
    }

    out.snoreScore = m_snoreSum / static_cast<double>(m_windowCount);
    out.noiseDb = m_noiseSum / static_cast<double>(m_windowCount);
    out.snoreActive = (static_cast<double>(m_activeCount) / static_cast<double>(m_windowCount)) >= 0.25;

    // snore_ratio for DB = fraction of active windows (0~1)
    m_latest = out;
    const double active_ratio = static_cast<double>(m_activeCount) / static_cast<double>(m_windowCount);
    out.snoreScore = active_ratio;

    beginMinute();
    return out;
}

double estimateSnoreRatioFallback(double toss_mean, const std::optional<double>& env_temp)
{
    double base = 0.12;
    if (toss_mean > 0.2)
        base -= toss_mean * 0.3;

    if (env_temp)
        base += std::max(0.0, *env_temp - 25.0) * 0.08;

    return std::clamp(base, 0.0, 1.0);
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
