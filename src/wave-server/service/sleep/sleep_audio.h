#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "../../device/interface/audio.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct SnoreAudioSample
{
    double snoreScore = 0.0;  // 0~1 band-energy ratio in snore band
    double noiseDb = 0.0;     // RMS dBFS-ish (negative..0)
    bool snoreActive = false;
};

// Wave Station PCM -> snore band energy + noise level (no PCM persistence).
class SnoreAudioAnalyzer
{
public:
    void reset();
    void pushFrame(const dev::AudioFrame& frame, uint32_t sample_rate_hz);
    // Drain analysis windows accumulated since last flush (for 1m ratio).
    void beginMinute();
    SnoreAudioSample flushMinute();

    // Latest instantaneous sample (for debugging / 30m fallback).
    SnoreAudioSample latest() const { return m_latest; }
    bool hasData() const { return m_windowCount > 0 || !m_pcm.empty(); }

private:
    void analyzeWindow();

    std::vector<float> m_pcm;
    SnoreAudioSample m_latest;
    double m_snoreSum = 0.0;
    double m_noiseSum = 0.0;
    int32_t m_windowCount = 0;
    int32_t m_activeCount = 0;
    uint32_t m_sampleRate = 16000;

    static constexpr size_t kWindowSamples = 512;
    static constexpr size_t kMaxPcmSamples = 16000 * 5;  // 5 s @ 16 kHz
    static constexpr double kSnoreHzLo = 100.0;
    static constexpr double kSnoreHzHi = 800.0;
    static constexpr double kActiveThreshold = 0.18;
};

double estimateSnoreRatioFallback(double toss_mean, const std::optional<double>& env_temp);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
