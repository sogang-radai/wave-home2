#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../device.h"
#include "../interface/audio.h"
#include "../interface/camera.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

// DroidCam phone camera (WiFi MJPEG over HTTP).
//
// Connects to the DroidCam app on a phone at host:port (default :4747).
//   - Snapshot : HTTP GET videoPath (MJPEG frame) -> IImageProvider
//   - Live     : MJPEG via wave-server HTTP proxy -> IVideoStreamProvider
//   - Microphone : optional phone mic (audioPath)  -> IAudioSource
class DroidCam :
    public Device,
    public Queryable,
    public IImageProvider,
    public IVideoStreamProvider,
    public IAudioSource
{
public:
    static constexpr const char* kClass = "droid_cam";
    static constexpr uint16_t kDefaultPort = 4747;

    struct InterfaceConfig
    {
        std::string host;
        std::string mac;
        uint16_t port = kDefaultPort;
        std::string videoPath = "/video";
    };

    struct AudioConfig
    {
        bool enabled = false;
        std::string audioPath = "/audio";
        uint32_t sampleRate = 16000;
        uint8_t channels = 1;
        uint8_t sampleSize = 16;
    };

    struct Capabilities
    {
        bool snapshot = true;
        bool videoStream = true;
        bool microphone = false;
    };

    DroidCam();
    ~DroidCam() override;

    const InterfaceConfig& getInterfaceConfig() const;
    const AudioConfig& getAudioConfig() const;
    const Capabilities& getCapabilities() const;

    std::string buildVideoUrl() const;
    std::string buildAudioUrl() const;

    // Device
    int init(const json& config) override;
    void shutdown() override;

    std::string_view getClass() const override;

    bool isHostReachable() const;

    // True when the DroidCam phone app is serving HTTP video (updated by the
    // background health monitor).
    bool isAppAlive() const;
    void setStreamViewerCount(int viewers);
    void markPhoneOffline();

    // Queryable
    json query(std::string_view name, const json& params) override;
    std::future<json> queryAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    // IImageProvider
    bool captureFrame(CameraFrame& outFrame) override;
    std::future<void> captureFrameAsync(CameraFrame& outFrame) override;

    // IVideoStreamProvider
    bool enumerateStreamProfiles(std::vector<CameraStreamProfile>& outProfiles) override;
    bool getStreamUri(std::string_view profile, std::string& outUri) override;

    // IAudioSource (phone microphone, when AudioConfig::enabled)
    AudioFormat getSourceFormat() const override;
    void setAudioQueueSize(size_t size) override;
    size_t getAudioQueueSize() const override;
    bool getLatestFrame(AudioFrame& outFrame) override;
    std::future<void> getLatestFrameAsync(AudioFrame& outFrame) override;
    bool popFrame(AudioFrame& outFrame) override;

private:
    void registerQueries();

    float probeMicLevel();
    bool probeAppAlive() const;
    void startHealthMonitor();
    void stopHealthMonitor();
    void onAppWentOffline();

    std::atomic<bool> m_appAlive{false};
    std::atomic<int> m_streamViewers{0};
    std::atomic<bool> m_healthStop{false};
    std::thread m_healthThread;
    InterfaceConfig m_interface;
    AudioConfig m_audio;
    Capabilities m_capabilities;
    mutable std::mutex m_mutex;
    mutable std::chrono::steady_clock::time_point m_lastReachabilityCheck {};
    mutable bool m_lastReachability = false;
    mutable std::chrono::steady_clock::time_point m_lastMicProbe {};
    mutable float m_cachedMicLevel = 0.0f;
};

/*
Queries (no Actionable — control is via stream/snapshot interfaces):

  capabilities  — snapshot / video_stream / microphone flags
  session       — host, port, video_url, audio_url
  status        — host reachability, streaming state, mic_level (if enabled)
  stream        — MJPEG source URL

Transport:
  HTTP MJPEG at http://{host}:{port}{videoPath}  (default /video)
  Optional audio at http://{host}:{port}{audioPath} (default /audio)

Live view is proxied by wave-server at GET /api/v1/iot/devices/{id}/stream/mjpeg.
*/

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
