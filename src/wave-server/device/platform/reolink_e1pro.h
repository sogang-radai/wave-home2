#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <atomic>

#include "../device.h"
#include "../interface/audio.h"
#include "../interface/camera.h"
#include "../interface/ptz.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

// Reolink E1 Pro IoT camera.
//
// Control runs over ONVIF (SOAP on port 8000): device/media/ptz/imaging services,
// authenticated with a WS-Security UsernameToken digest.
//   - Snapshot   : ONVIF GetSnapshotUri -> HTTP GET      -> IImageProvider
//   - Live A/V   : ONVIF GetStreamUri (RTSP on 554)      -> IVideoStreamProvider
//   - Pan/Tilt   : ONVIF ContinuousMove/GotoPreset/Stop  -> IPtzController
//   - Microphone : audio track of the RTSP stream        -> IAudioSource
//   - Speaker    : two-way audio backchannel             -> IAudioSink
class ReolinkE1Pro :
    public Device,
    public Queryable,
    public Actionable,
    public IImageProvider,
    public IVideoStreamProvider,
    public IPtzController,
    public IAudioSource,
    public IAudioSink
{
public:
    struct Config
    {
        std::string host;
        uint16_t onvifPort = 8000;
        uint16_t rtspPort = 554;
        std::string user;
        std::string password;
        uint32_t channel = 0;

        // When enabled, video / two-way audio are published through the shared
        // go2rtc service (see service/go2rtc_service.h). go2rtcSource overrides
        // the source URL handed to go2rtc (defaults to the main RTSP stream).
        bool go2rtc = false;
        std::string go2rtcSource;
    };

    ReolinkE1Pro();
    ~ReolinkE1Pro() override;

    const Config& getConfig() const;

    // Obfuscates a secret for storage in the device config (stub cipher).
    // The matching decryption is internal to the implementation.
    static std::string encryptSecret(std::string_view plain);

    // Device
    int init(const json& config) override;
    void shutdown() override;

    std::string_view getClass() const override;

    // True when the camera host accepts a TCP connection on the ONVIF port.
    bool isHostReachable() const;

    // Lazily registers the go2rtc stream after verifying host reachability.
    bool ensureGo2rtcStream();
    bool ensureGo2rtcTalkStream();
    void releaseGo2rtcStream();
    bool isGo2rtcStreamActive() const;
    std::string_view getGo2rtcStreamName() const;

    // Queryable
    json query(std::string_view name, const json& params) override;
    std::future<json> queryAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    // Actionable
    int invoke(std::string_view name, const json& params) override;
    std::future<int> invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    // IImageProvider
    bool captureFrame(CameraFrame& outFrame) override;
    std::future<void> captureFrameAsync(CameraFrame& outFrame) override;

    // IVideoStreamProvider
    bool enumerateStreamProfiles(std::vector<CameraStreamProfile>& outProfiles) override;
    bool getStreamUri(std::string_view profile, std::string& outUri) override;

    // IPtzController
    PtzCapabilities getPtzCapabilities() const override;
    bool movePtz(const PtzVector& velocity, uint32_t durationMs = 0) override;
    bool stopPtz() override;
    bool movePtzTo(const PtzVector& position) override;
    bool enumeratePtzPresets(std::vector<PtzPreset>& outPresets) override;
    bool gotoPtzPreset(uint32_t presetId) override;
    bool savePtzPreset(uint32_t presetId, std::string_view name) override;
    bool movePtzHome() override;
    std::future<bool> movePtzAsync(const PtzVector& velocity, uint32_t durationMs = 0) override;
    std::future<bool> gotoPtzPresetAsync(uint32_t presetId) override;

    // IAudioSource (microphone)
    AudioFormat getSourceFormat() const override;
    void setAudioQueueSize(size_t size) override;
    size_t getAudioQueueSize() const override;
    bool getLatestFrame(AudioFrame& outFrame) override;
    std::future<void> getLatestFrameAsync(AudioFrame& outFrame) override;
    bool popFrame(AudioFrame& outFrame) override;

    // IAudioSink (speaker)
    AudioFormat getSinkFormat() const override;
    bool playFrame(const AudioFrame& frame) override;
    std::future<bool> playFrameAsync(const AudioFrame& frame) override;
    void stopPlayback() override;

    // File-based helpers (used by tooling / tests).
    // Records the camera microphone (RTSP audio) to a WAV file for the given
    // duration; plays a local audio file on the camera speaker via go2rtc.
    bool recordAudioToFile(const std::string& path, uint32_t seconds);
    bool playAudioFile(const std::string& path);

private:
    struct Impl;

    void registerActionsAndQueries();

    float probeMicLevel();

    std::unique_ptr<Impl> m_impl;
    Config m_config;
    mutable std::mutex m_mutex;
    mutable std::chrono::steady_clock::time_point m_lastReachabilityCheck {};
    mutable bool m_lastReachability = false;
    mutable std::chrono::steady_clock::time_point m_lastMicProbe {};
    mutable float m_cachedMicLevel = 0.0f;
    mutable std::atomic<bool> m_micProbeInFlight {false};
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
