#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../device.h"
#include "../interface/audio.h"
#include "../interface/infrared.h"
#include "../protocol/wave_station.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

// WaveStation edge device (ESP32) — WSP1 TCP client.
// ESP32 listens on wsp::kTcpPort; wave-server connects as client.
//   - Microphone : MicPCM / MicComp (Opus) from device  -> IAudioSource
//   - Speaker    : SpkPCM / SpkComp to device           -> IAudioSink
class RadaiWs :
    public Device,
    public Queryable,
    public Actionable,
    public IAudioSource,
    public IAudioSink,
    public IIrReceiver,
    public IIrTransmitter
{
    struct Impl;
    friend struct Impl;

public:
    static constexpr const char* kClass = "wave_station";

    struct InterfaceConfig
    {
        std::string host;
        std::string mac;
        uint16_t port = wsp::kTcpPort;
    };

    struct AudioConfig
    {
        uint32_t sampleRate = wsp::kDefaultSampleRate;
        uint8_t channels = 1;
        uint8_t sampleSize = 16;
        uint8_t frameDurationMs = wsp::kDefaultFrameMs;
        uint32_t opusBitrate = 28000;
        bool preferCompressedMic = true;
        bool preferCompressedSpk = true;
    };

    struct SessionConfig
    {
        uint32_t connectTimeoutMs = 4000;
        uint32_t requestTimeoutMs = 5000;
        uint32_t heartbeatIntervalMs = 5000;
        uint32_t reconnectInitialMs = 1000;
        uint32_t reconnectMaxMs = 30000;
        uint32_t maxPayloadSize = wsp::kMaxPayload;
    };

    struct Capabilities
    {
        bool micPcm = true;
        bool micOpus = true;
        bool speakerPcm = false;
        bool speakerOpus = true;
        bool irReceive = true;
        bool irTransmit = true;
        bool ambientLight = false;
        bool temperature = false;
        bool humidity = false;
    };

    struct SubscriptionState
    {
        bool micPcm = false;
        bool micOpus = false;
        bool irReceive = false;
        bool ambientLight = false;
        bool temperature = false;
        bool humidity = false;
        uint16_t sensorIntervalMs = 1000;
    };

    struct EnvSnapshot
    {
        bool luxValid = false;
        bool temperatureValid = false;
        bool humidityValid = false;
        float lux = 0.0f;
        float temperatureC = 0.0f;
        float humidityPercent = 0.0f;
        std::chrono::steady_clock::time_point updatedAt {};
    };

    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting,
    };

    RadaiWs();
    ~RadaiWs() override;

    const InterfaceConfig& getInterfaceConfig() const;
    const AudioConfig& getAudioConfig() const;
    const SessionConfig& getSessionConfig() const;
    const Capabilities& getCapabilities() const;
    const SubscriptionState& getSubscriptionState() const;
    ConnectionState getConnectionState() const;
    bool isLinkConnected() const;
    bool isIoActive() const;

    // Device
    int init(const json& config) override;
    void shutdown() override;

    std::string_view getClass() const override;

    // Queryable
    json query(std::string_view name, const json& params) override;
    std::future<json> queryAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    // Actionable
    int invoke(std::string_view name, const json& params) override;
    std::future<int> invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    // IAudioSource (microphone from WaveStation)
    AudioFormat getSourceFormat() const override;
    void setAudioQueueSize(size_t size) override;
    size_t getAudioQueueSize() const override;
    bool getLatestFrame(AudioFrame& outFrame) override;
    std::future<void> getLatestFrameAsync(AudioFrame& outFrame) override;

    // IAudioSink (speaker on WaveStation)
    AudioFormat getSinkFormat() const override;
    bool playFrame(const AudioFrame& frame) override;
    std::future<bool> playFrameAsync(const AudioFrame& frame) override;
    void stopPlayback() override;

    // IIrTransmitter
    int transmitTimings(
        const std::vector<uint16_t>& timingsUs,
        uint32_t carrierHz = 38000,
        uint16_t repeat = 0) override;
    std::future<int> transmitTimingsAsync(
        const std::vector<uint16_t>& timingsUs,
        uint32_t carrierHz = 38000,
        uint16_t repeat = 0) override;

    // IIrReceiver
    bool getLatestIr(IrTimingFrame& outFrame) override;
    bool waitForIr(IrTimingFrame& outFrame, uint32_t timeoutMs) override;
    std::future<bool> getLatestIrAsync(IrTimingFrame& outFrame) override;
    std::future<bool> waitForIrAsync(IrTimingFrame& outFrame, uint32_t timeoutMs) override;

private:
    void registerActionsAndQueries();

    int startClient();
    void stopClient();

    int subscribe(wsp::Type targetType, uint16_t intervalMs, uint32_t options);
    int unsubscribe(wsp::Type targetType);
    int ensureMicSubscription();
    int ensureIrSubscription();

    int sendHeartbeat();
    int sendIrRaw(const std::vector<uint16_t>& rawUs, uint32_t carrierHz, uint16_t repeat);
    int sendSpkOpus(const uint8_t* data, size_t size, bool keyFrame);
    int sendSpkPcm(const int16_t* samples, size_t sampleCount);

    void onPacketReceived(const uint8_t* data, size_t size);
    void onMicPcm(const wsp::AudioPCMBody& body, const uint8_t* pcmData, uint64_t timestampUs);
    void onMicComp(const wsp::AudioCompBody& body, const uint8_t* encodedData, uint64_t timestampUs, bool keyFrame);
    void enqueueMicFrame(AudioFrame frame);
    void updateMicLevel(float level);
    void updateEnv(const EnvSnapshot& env);
    void onClientConnected();
    void onSensor(wsp::Type type, const wsp::SensorBody& sensor);
    void onIrReceived(const wsp::IrReceiveBody& body, const std::vector<uint16_t>& timings);

    std::unique_ptr<Impl> m_impl;
    InterfaceConfig m_interface;
    AudioConfig m_audio;
    SessionConfig m_session;
    Capabilities m_capabilities;
    SubscriptionState m_subscriptions;
    EnvSnapshot m_env;
    IrTimingFrame m_lastIr;
    uint64_t m_irGeneration = 0;
    float m_micLevel = 0.0f;
    ConnectionState m_connectionState = ConnectionState::Disconnected;
    std::atomic<bool> m_ioActive{false};
    mutable std::mutex m_mutex;
};

/*
Queries:
  capabilities  — mic/speaker/IR/sensor flags
  session       — host, port, TCP link state, audio format
  status        — connection, subscriptions, mic queue/level
  mic_level     — recent microphone RMS level (0..1)
  env           — lux / temperature / humidity snapshot
  last_ir       — most recent IrReceive: timings, matched commandId, overflow

Actions:
  send_ir       { "commandId": "<ir_list.json id>", "repeat": 0 }
                — IrTransmit (host → device → IR LED)
  subscribe     { "target": "mic_opus"|"mic_pcm"|"ir_receive"|"ambient_light"|...,
                  "intervalMs": 0, "compressed": false, "on_change_only": false }
  unsubscribe   { "target": "mic_opus"|"ir_receive"|... }
  speak         { "text": "..." }  — TTS → IAudioSink / SpkComp (planned)

Audio (IAudioSource / IAudioSink):
  Source — MicPCM or MicComp (Opus→PCM); lazy subscribe on getLatestFrame
  Sink   — playFrame → SpkComp or SpkPCM; stopPlayback → LastFrame marker

IR (IIrReceiver / IIrTransmitter):
  Receiver — getLatestIr / waitForIr; lazy subscribe on first use
  Transmitter — transmitTimings (raw μs mark/space array)

IR receive (device → host, WSP1 IrReceive):
  ESP32 pushes raw mark/space timings (μs) when a remote is detected.
  On connect, auto-subscribes IrReceive when capabilities.ir_receive is true.
  Timings are matched against settings.ir_list_path (default bin/device/ir_list.json).
  Matched commandId is exposed via last_ir; when wave-server automation is
  running, ir_recv rule triggers are fired via TriggerManager.

IR transmit (host → device, WSP1 IrTransmit):
  send_ir loads timings from ir_list.json and sends IrTransmitBody + rawData.

WSP1 control (host → device):
  Subscribe, Unsubscribe, Heartbeat

WSP1 data (device → host):
  MicPCM, MicComp, IrReceive, AmbientLight, Temperature, Humidity, Ack, Error
*/

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
