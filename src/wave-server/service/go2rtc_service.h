#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/coredefs.h"

#define SERVICE_NAMESPACE_BEGIN namespace service {
#define SERVICE_NAMESPACE_END }

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

// Manages a single shared go2rtc process used for camera video and two-way
// audio. Cameras register their stream on startup and release it on shutdown;
// the process is spawned lazily on the first stream and terminated once the
// last stream is released. Thread-safe singleton.
class Go2RtcService
{
public:
    struct Config
    {
        std::string binaryPath;      // go2rtc executable path
        std::string ffmpegPath;      // ffmpeg executable (for snapshot/talk transcoding)
        std::string apiHost = "127.0.0.1";
        uint16_t apiPort = 1984;     // go2rtc REST/WebUI port
        uint16_t rtspPort = 8554;    // go2rtc re-published RTSP port
        std::string logLevel = "warning";
    };

    static Go2RtcService& get();

    // Applied only while the process is stopped; ignored otherwise.
    void configure(const Config& config);

    // Registers/updates a stream, starting the process if needed. A stream may
    // carry several sources (e.g. an RTSP video source plus an onvif:// source
    // that provides the two-way audio backchannel).
    bool acquireStream(const std::string& name, const std::string& source);
    bool acquireStream(const std::string& name, const std::vector<std::string>& sources);

    // Releases a stream, stopping the process when the last one is gone.
    void releaseStream(const std::string& name);

    bool isRunning() const;
    size_t streamCount() const;

    std::string apiUrl() const;
    std::string streamRtspUrl(const std::string& name) const;

    // Grabs a JPEG snapshot of a registered stream (go2rtc transcodes via ffmpeg).
    bool fetchSnapshot(const std::string& name, std::vector<uint8_t>& out_jpeg, uint32_t width = 0);

    // Streams an audio source to a camera speaker (two-way audio). source is a
    // go2rtc source URI, e.g. "ffmpeg:/path/file.wav#audio=pcma#input=file".
    // Passing an empty source stops the active playback.
    bool streamToCamera(const std::string& name, const std::string& source);

    // Proxies a WebRTC offer to go2rtc and returns the answer SDP.
    bool exchangeWebRtc(const std::string& name, const std::string& offer_sdp, std::string& answer_sdp);

    bool hasStream(const std::string& name) const;

    bool getStreamEndpoint(const std::string& name, std::string& host, uint16_t& port) const;

    // Stops all streams and the child process before static destruction.
    void shutdownAll();

    // Opens a live fMP4 stream from go2rtc for HTTP proxying to browsers.
    class LiveMp4Stream
    {
    public:
        LiveMp4Stream();
        ~LiveMp4Stream();

        LiveMp4Stream(const LiveMp4Stream&) = delete;
        LiveMp4Stream& operator=(const LiveMp4Stream&) = delete;

        bool open(const std::string& name);
        ssize_t read(char* buffer, size_t capacity);
        void close();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
    Go2RtcService();
    ~Go2RtcService();
    Go2RtcService(const Go2RtcService&) = delete;
    Go2RtcService& operator=(const Go2RtcService&) = delete;

    bool ensureProcess();     // caller holds m_mutex
    void terminateProcess();  // caller holds m_mutex
    bool writeConfigFile();
    bool waitForApi(uint32_t timeout_ms);
    bool apiPutStream(const std::string& name, const std::vector<std::string>& sources);
    bool apiDeleteStream(const std::string& name);

    Config m_config;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<std::string>> m_streams;
    int m_pid = -1;
    std::string m_configPath;
    bool m_shutdown = false;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
