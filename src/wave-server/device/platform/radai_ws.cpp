#include "radai_ws.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <asio.hpp>

#ifdef WAVE_HAS_OPUS
#include <opus/opus.h>
#endif

#include "../../core/logger.h"
#ifndef WAVE_STANDALONE_DEVICE_TEST
#include "../../service/ir_trigger_bridge.h"
#endif
#include "network/net_util.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    json make_query_error(int code, std::string_view message = {})
    {
        json out = json::object();
        out["code"] = code;
        if (!message.empty())
            out["message"] = std::string(message);
        return out;
    }

    bool is_control_type(uint16_t rawType)
    {
        return rawType >= 0xF000;
    }

    float compute_rms_level(const int16_t* samples, size_t count)
    {
        if (count == 0)
            return 0.0f;

        double sumSq = 0.0;
        for (size_t i = 0; i < count; ++i)
        {
            const double v = static_cast<double>(samples[i]) / 32768.0;
            sumSq += v * v;
        }
        const double rms = std::sqrt(sumSq / static_cast<double>(count));
        return static_cast<float>(std::min(1.0, rms * 4.0));
    }

    std::string connection_state_to_string(RadaiWs::ConnectionState state)
    {
        switch (state)
        {
        case RadaiWs::ConnectionState::Disconnected: return "disconnected";
        case RadaiWs::ConnectionState::Connecting: return "connecting";
        case RadaiWs::ConnectionState::Connected: return "connected";
        case RadaiWs::ConnectionState::Reconnecting: return "reconnecting";
        default: return "unknown";
        }
    }

    bool parse_target_type(std::string_view name, wsp::Type& outType)
    {
        if (name == "mic_pcm")
            outType = wsp::Type::MicPCM;
        else if (name == "mic_opus" || name == "mic_comp")
            outType = wsp::Type::MicComp;
        else if (name == "ambient_light" || name == "lux")
            outType = wsp::Type::AmbientLight;
        else if (name == "temperature" || name == "temp")
            outType = wsp::Type::Temperature;
        else if (name == "humidity")
            outType = wsp::Type::Humidity;
        else if (name == "ir_receive" || name == "ir")
            outType = wsp::Type::IrReceive;
        else
            return false;
        return true;
    }

    bool io_loop_should_run(const RadaiWs& owner)
    {
        return owner.isIoActive();
    }

    void copy_ir_timing_frame(const IrTimingFrame& src, IrTimingFrame& dst)
    {
        dst = src;
    }

    void validate_radai_ws_config(const json& config)
    {
        if (config.at("class").get<std::string>() != RadaiWs::kClass)
            throw std::invalid_argument("wave_station config field 'class' must be 'wave_station'");

        if (!config.contains("interface") || !config["interface"].is_object())
            throw std::invalid_argument("wave_station requires object field 'interface'");

        const auto& iface = config["interface"];
        if (!iface.contains("host") || !iface["host"].is_string() || iface["host"].get<std::string>().empty())
            throw std::invalid_argument("wave_station interface requires non-empty string 'host'");

        if (iface.contains("port") && !iface["port"].is_number_integer())
            throw std::invalid_argument("wave_station interface field 'port' must be an integer");

        if (iface.contains("mac") && !iface["mac"].is_string())
            throw std::invalid_argument("wave_station interface field 'mac' must be a string");
    }

    RadaiWs::InterfaceConfig parse_interface_config(const json& config)
    {
        const auto& iface = config.at("interface");
        RadaiWs::InterfaceConfig out;
        out.host = iface.at("host").get<std::string>();
        out.mac = iface.value("mac", "");
        out.port = static_cast<uint16_t>(iface.value("port", static_cast<int>(wsp::kTcpPort)));
        return out;
    }

    RadaiWs::AudioConfig parse_audio_config(const json& config)
    {
        RadaiWs::AudioConfig out;
        if (!config.contains("settings") || !config["settings"].is_object())
            return out;

        const auto& settings = config["settings"];
        if (settings.contains("sample_rate"))
            out.sampleRate = settings["sample_rate"].get<uint32_t>();
        if (settings.contains("channels"))
            out.channels = static_cast<uint8_t>(settings["channels"].get<uint32_t>());
        if (settings.contains("sample_size"))
            out.sampleSize = static_cast<uint8_t>(settings["sample_size"].get<uint32_t>());
        if (settings.contains("frame_duration_ms"))
            out.frameDurationMs = static_cast<uint8_t>(settings["frame_duration_ms"].get<uint32_t>());
        if (settings.contains("opus_bitrate"))
            out.opusBitrate = settings["opus_bitrate"].get<uint32_t>();
        if (settings.contains("prefer_compressed_mic"))
            out.preferCompressedMic = settings["prefer_compressed_mic"].get<bool>();
        if (settings.contains("prefer_compressed_spk"))
            out.preferCompressedSpk = settings["prefer_compressed_spk"].get<bool>();
        return out;
    }

    RadaiWs::SessionConfig parse_session_config(const json& config)
    {
        RadaiWs::SessionConfig out;
        if (!config.contains("settings") || !config["settings"].is_object())
            return out;

        const auto& settings = config["settings"];
        if (settings.contains("connect_timeout_ms"))
            out.connectTimeoutMs = settings["connect_timeout_ms"].get<uint32_t>();
        if (settings.contains("request_timeout_ms"))
            out.requestTimeoutMs = settings["request_timeout_ms"].get<uint32_t>();
        if (settings.contains("heartbeat_interval_ms"))
            out.heartbeatIntervalMs = settings["heartbeat_interval_ms"].get<uint32_t>();
        if (settings.contains("reconnect_initial_ms"))
            out.reconnectInitialMs = settings["reconnect_initial_ms"].get<uint32_t>();
        if (settings.contains("reconnect_max_ms"))
            out.reconnectMaxMs = settings["reconnect_max_ms"].get<uint32_t>();
        return out;
    }

    RadaiWs::Capabilities parse_capabilities(const json& config)
    {
        RadaiWs::Capabilities out;
        if (!config.contains("settings") || !config["settings"].is_object())
            return out;

        const auto& settings = config["settings"];
        if (!settings.contains("capabilities") || !settings["capabilities"].is_object())
            return out;

        const auto& caps = settings["capabilities"];
        if (caps.contains("mic_pcm"))
            out.micPcm = caps["mic_pcm"].get<bool>();
        if (caps.contains("mic_opus"))
            out.micOpus = caps["mic_opus"].get<bool>();
        if (caps.contains("speaker_pcm"))
            out.speakerPcm = caps["speaker_pcm"].get<bool>();
        if (caps.contains("speaker_opus"))
            out.speakerOpus = caps["speaker_opus"].get<bool>();
        if (caps.contains("ir_receive"))
            out.irReceive = caps["ir_receive"].get<bool>();
        if (caps.contains("ir_transmit"))
            out.irTransmit = caps["ir_transmit"].get<bool>();
        if (caps.contains("ambient_light"))
            out.ambientLight = caps["ambient_light"].get<bool>();
        if (caps.contains("temperature"))
            out.temperature = caps["temperature"].get<bool>();
        if (caps.contains("humidity"))
            out.humidity = caps["humidity"].get<bool>();
        return out;
    }

    std::string default_ir_list_path()
    {
#ifdef WAVE_SOURCE_DIR
        return std::string(WAVE_SOURCE_DIR) + "/bin/device/ir_list.json";
#else
        return "device/ir_list.json";
#endif
    }

    double timing_distance(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b)
    {
        const size_t count = std::min(a.size(), b.size());
        if (count == 0)
            return 1e9;

        double sum = 0.0;
        for (size_t i = 0; i < count; ++i)
            sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));

        sum += static_cast<double>(std::max(a.size(), b.size()) - count) * 1000.0;
        return sum / static_cast<double>(count);
    }

    int verify_mac(const std::string& expected, const std::string& host)
    {
        if (expected.empty())
            return 0;

        std::string actual;
        if (!net::resolveMacForIp(host, actual))
        {
            LOG_WARN("RadaiWs: could not resolve MAC for {} (skipping check)", host);
            return 0;
        }
        if (!net::macEquals(expected, actual))
        {
            LOG_ERROR("RadaiWs: MAC mismatch for {} (expected {}, got {})", host, expected, actual);
            return -7;
        }
        LOG_INFO("RadaiWs: MAC verified for {} ({})", host, actual);
        return 0;
    }

#ifdef WAVE_HAS_OPUS
    class OpusCodec
    {
    public:
        OpusCodec(uint32_t sampleRate, uint8_t channels, uint32_t bitrate, uint8_t frameDurationMs) :
            m_channels(channels),
            m_frameSamples(sampleRate * frameDurationMs / 1000)
        {
            int err = 0;
            m_decoder = opus_decoder_create(static_cast<int>(sampleRate), channels, &err);
            if (err != OPUS_OK)
                throw std::runtime_error("opus_decoder_create failed: " + std::to_string(err));

            m_encoder = opus_encoder_create(static_cast<int>(sampleRate), channels, OPUS_APPLICATION_VOIP, &err);
            if (err != OPUS_OK)
            {
                opus_decoder_destroy(m_decoder);
                m_decoder = nullptr;
                throw std::runtime_error("opus_encoder_create failed: " + std::to_string(err));
            }

            opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(static_cast<int>(bitrate)));
            opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        }

        ~OpusCodec()
        {
            if (m_decoder)
                opus_decoder_destroy(m_decoder);
            if (m_encoder)
                opus_encoder_destroy(m_encoder);
        }

        OpusCodec(const OpusCodec&) = delete;
        OpusCodec& operator=(const OpusCodec&) = delete;

        bool decode(const uint8_t* data, size_t size, std::vector<int16_t>& outPcm, bool keyFrame)
        {
            if (!m_decoder || size == 0)
                return false;

            if (keyFrame)
                opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);

            outPcm.resize(m_frameSamples * m_channels);
            const int decoded = opus_decode(
                m_decoder,
                data,
                static_cast<int>(size),
                outPcm.data(),
                static_cast<int>(m_frameSamples),
                0);
            if (decoded < 0)
            {
                LOG_WARN("RadaiWs: opus_decode failed ({})", decoded);
                return false;
            }

            outPcm.resize(static_cast<size_t>(decoded) * m_channels);
            return true;
        }

        bool encode(const int16_t* pcm, size_t sampleCount, std::vector<uint8_t>& outEncoded, bool keyFrame)
        {
            if (!m_encoder || sampleCount == 0)
                return false;

            if (keyFrame)
                opus_encoder_ctl(m_encoder, OPUS_RESET_STATE);

            outEncoded.resize(4096);
            const int encoded = opus_encode(
                m_encoder,
                pcm,
                static_cast<int>(sampleCount / m_channels),
                outEncoded.data(),
                static_cast<int>(outEncoded.size()));
            if (encoded < 0)
            {
                LOG_WARN("RadaiWs: opus_encode failed ({})", encoded);
                return false;
            }

            outEncoded.resize(static_cast<size_t>(encoded));
            return true;
        }

    private:
        uint8_t m_channels;
        size_t m_frameSamples;
        OpusDecoder* m_decoder = nullptr;
        OpusEncoder* m_encoder = nullptr;
    };
#endif

    bool try_extract_wsp_packet(std::vector<uint8_t>& streamBuf, std::vector<uint8_t>& outPacket)
    {
        while (streamBuf.size() >= 4)
        {
            uint32_t magic = 0;
            std::memcpy(&magic, streamBuf.data(), sizeof(magic));
            if (magic == wsp::kMagic)
                break;

            streamBuf.erase(streamBuf.begin());
        }

        if (streamBuf.size() < wsp::kHeaderSize)
            return false;

        uint32_t payloadSize = 0;
        std::memcpy(&payloadSize, streamBuf.data() + 8, sizeof(payloadSize));
        if (payloadSize > wsp::kMaxPayload)
        {
            LOG_ERROR("RadaiWs: invalid payloadSize {} — resyncing", payloadSize);
            streamBuf.erase(streamBuf.begin());
            return false;
        }

        const size_t packetSize = wsp::kHeaderSize + payloadSize;
        if (streamBuf.size() < packetSize)
            return false;

        outPacket.assign(streamBuf.begin(), streamBuf.begin() + static_cast<ptrdiff_t>(packetSize));
        streamBuf.erase(streamBuf.begin(), streamBuf.begin() + static_cast<ptrdiff_t>(packetSize));
        return true;
    }
}

struct RadaiWs::Impl
{
    using tcp = asio::ip::tcp;
    using workGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    explicit Impl(RadaiWs& owner) :
        m_owner(owner),
        m_resolver(m_io),
        m_socket(m_io),
        m_heartbeatTimer(m_io)
    {
    }

    ~Impl()
    {
        stop();
    }

    void configure(
        RadaiWs::InterfaceConfig interfaceConfig,
        RadaiWs::SessionConfig sessionConfig,
        RadaiWs::AudioConfig audioConfig,
        std::string irListPath)
    {
        m_interface = std::move(interfaceConfig);
        m_session = sessionConfig;
        m_audio = audioConfig;
        m_irListPath = std::move(irListPath);
        initOpus();
    }

    void initOpus()
    {
#ifdef WAVE_HAS_OPUS
        try
        {
            m_opus = std::make_unique<OpusCodec>(
                m_audio.sampleRate,
                m_audio.channels,
                m_audio.opusBitrate,
                m_audio.frameDurationMs);
        }
        catch (const std::exception& ex)
        {
            LOG_WARN("RadaiWs: Opus init failed ({}); compressed audio disabled", ex.what());
            m_opus.reset();
        }
#endif
    }

    bool decodeOpus(const uint8_t* data, size_t size, std::vector<int16_t>& outPcm, bool keyFrame)
    {
#ifdef WAVE_HAS_OPUS
        return m_opus && m_opus->decode(data, size, outPcm, keyFrame);
#else
        (void)data;
        (void)size;
        (void)outPcm;
        (void)keyFrame;
        return false;
#endif
    }

    bool encodeOpus(const int16_t* pcm, size_t sampleCount, std::vector<uint8_t>& outEncoded, bool keyFrame)
    {
#ifdef WAVE_HAS_OPUS
        return m_opus && m_opus->encode(pcm, sampleCount, outEncoded, keyFrame);
#else
        (void)pcm;
        (void)sampleCount;
        (void)outEncoded;
        (void)keyFrame;
        return false;
#endif
    }

    void start(bool waitForInitialConnect)
    {
        if (m_work)
            return;

        m_initConnectPending.store(waitForInitialConnect, std::memory_order_release);

        m_work = std::make_unique<workGuard>(asio::make_work_guard(m_io));
        m_ioThread = std::thread([this]()
        {
            m_ioThreadId = std::this_thread::get_id();
            m_io.run();
        });

        m_reconnectDelayMs = m_session.reconnectInitialMs;
        asio::post(m_io, [this]()
        {
            if (m_work)
                openConnection();
        });
    }

    bool waitForInitialConnect(uint32_t timeoutMs)
    {
        if (m_connected.load(std::memory_order_acquire))
            return true;

        std::unique_lock lock(m_initConnectMutex);
        m_initConnectCv.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [this]()
            {
                return !m_initConnectPending.load(std::memory_order_acquire)
                    || m_connected.load(std::memory_order_acquire);
            });
        return m_connected.load(std::memory_order_acquire);
    }

    void stop()
    {
        m_heartbeatTimer.cancel();
        m_reconnectTimer.cancel();
        m_connectTimer.cancel();
        m_resolver.cancel();

        std::error_code ec;
        m_socket.cancel(ec);
        m_socket.close(ec);

        m_work.reset();
        m_io.stop();

        if (m_ioThread.joinable())
            m_ioThread.join();

        m_ioThreadId = {};
        m_connected.store(false);
        m_connecting.store(false);
        m_initConnectPending.store(false, std::memory_order_release);
    }

    bool isConnected() const
    {
        return m_connected.load();
    }

    bool onIoThread() const
    {
        return m_ioThread.joinable() && std::this_thread::get_id() == m_ioThreadId;
    }

    int writePacketNow(const std::vector<uint8_t>& packet)
    {
        if (!m_connected.load())
            return -5;

        std::error_code ec;
        asio::write(m_socket, asio::buffer(packet), ec);
        return ec ? -5 : 0;
    }

    // Never block the IO thread waiting for a posted write (or Ack). That
    // deadlocks asio::io_context and stalls IrReceive / all packet reads.
    int postSend(std::vector<uint8_t> packet)
    {
        if (!m_work)
            return -5;

        if (onIoThread())
            return writePacketNow(packet);

        auto prom = std::make_shared<std::promise<int>>();
        auto fut = prom->get_future();
        asio::post(m_io, [this, packet = std::move(packet), prom]()
        {
            prom->set_value(writePacketNow(packet));
        });

        if (fut.wait_for(std::chrono::milliseconds(m_session.requestTimeoutMs)) != std::future_status::ready)
            return -6;

        return fut.get();
    }

    int sendControl(wsp::Type type, const void* body, size_t bodySize, bool waitAck)
    {
        const uint32_t requestId = ++m_requestId;

        wsp::ControlHeader header {};
        header.magic = wsp::kMagic;
        header.version = wsp::kProtoVer;
        header.reserved = 0;
        header.type = static_cast<uint16_t>(type);
        header.payloadSize = static_cast<uint32_t>(bodySize);
        header.requestId = requestId;

        std::vector<uint8_t> packet(sizeof(header) + bodySize);
        std::memcpy(packet.data(), &header, sizeof(header));
        if (bodySize > 0 && body)
            std::memcpy(packet.data() + sizeof(header), body, bodySize);

        // Waiting for Ack on the IO thread would prevent doRead from delivering it.
        const bool waitForAck = waitAck && !onIoThread();
        if (waitAck && !waitForAck)
        {
            LOG_WARN(
                "RadaiWs: sending type=0x{:04X} without Ack wait (called from IO thread)",
                static_cast<unsigned>(static_cast<uint16_t>(type)));
        }

        std::shared_ptr<std::promise<int>> ackPromise;
        if (waitForAck)
        {
            ackPromise = std::make_shared<std::promise<int>>();
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingAcks[requestId] = ackPromise;
        }

        const int rc = postSend(std::move(packet));
        if (rc != 0)
        {
            if (ackPromise)
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingAcks.erase(requestId);
            }
            return rc;
        }

        if (!ackPromise)
            return 0;

        auto fut = ackPromise->get_future();
        if (fut.wait_for(std::chrono::milliseconds(m_session.requestTimeoutMs)) != std::future_status::ready)
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingAcks.erase(requestId);
            return -6;
        }

        return fut.get();
    }

    int sendData(wsp::Type type, uint8_t flags, const void* body, size_t bodySize, const void* tail, size_t tailSize)
    {
        const uint32_t sequence = ++m_sequences[static_cast<uint16_t>(type)];

        wsp::DataHeader header {};
        header.magic = wsp::kMagic;
        header.version = wsp::kProtoVer;
        header.flags = flags;
        header.type = static_cast<uint16_t>(type);
        header.payloadSize = static_cast<uint32_t>(bodySize + tailSize);
        header.sequence = sequence;

        std::vector<uint8_t> packet(sizeof(header) + bodySize + tailSize);
        std::memcpy(packet.data(), &header, sizeof(header));
        if (bodySize > 0 && body)
            std::memcpy(packet.data() + sizeof(header), body, bodySize);
        if (tailSize > 0 && tail)
            std::memcpy(packet.data() + sizeof(header) + bodySize, tail, tailSize);

        return postSend(std::move(packet));
    }

    int sendHeartbeat()
    {
        return sendControl(wsp::Type::Heartbeat, nullptr, 0, false);
    }

    int sendSubscribe(wsp::Type targetType, uint16_t intervalMs, uint32_t options)
    {
        wsp::SubscribeBody body {};
        body.targetType = static_cast<uint16_t>(targetType);
        body.intervalMs = intervalMs;
        body.options = options;
        return sendControl(wsp::Type::Subscribe, &body, sizeof(body), true);
    }

    int sendUnsubscribe(wsp::Type targetType)
    {
        wsp::UnsubscribeBody body {};
        body.targetType = static_cast<uint16_t>(targetType);
        return sendControl(wsp::Type::Unsubscribe, &body, sizeof(body), true);
    }

    int sendIrRaw(const std::vector<uint16_t>& rawUs, uint32_t carrierHz, uint16_t repeat)
    {
        wsp::IrTransmitBody body {};
        body.length = static_cast<uint16_t>(rawUs.size());
        body.carrierHz = carrierHz;
        body.repeat = repeat;

        return sendData(
            wsp::Type::IrTransmit,
            wsp::HeaderFlag_None,
            &body,
            sizeof(body),
            rawUs.data(),
            rawUs.size() * sizeof(uint16_t));
    }

    int sendSpkPcm(const int16_t* samples, size_t sampleCount)
    {
        wsp::AudioPCMBody body {};
        body.sampleRate = m_audio.sampleRate;
        body.channels = m_audio.channels;
        body.bitsPerSample = m_audio.sampleSize;
        body.sampleCount = static_cast<uint16_t>(sampleCount / m_audio.channels);

        return sendData(
            wsp::Type::SpkPCM,
            wsp::HeaderFlag_None,
            &body,
            sizeof(body),
            samples,
            sampleCount * sizeof(int16_t));
    }

    int sendSpkOpus(const uint8_t* data, size_t size, bool keyFrame)
    {
        wsp::AudioCompBody body {};
        body.codec = static_cast<uint8_t>(wsp::AudioCodec::Opus);
        body.sampleRate = m_audio.sampleRate;
        body.channels = m_audio.channels;
        body.frameDurationMs = m_audio.frameDurationMs;
        body.encodedSize = static_cast<uint16_t>(size);

        uint8_t flags = wsp::HeaderFlag_None;
        if (keyFrame)
            flags |= wsp::HeaderFlag_KeyFrameBit;

        return sendData(
            wsp::Type::SpkComp,
            flags,
            &body,
            sizeof(body),
            data,
            size);
    }

    bool loadIrCommand(std::string_view commandId, std::vector<uint16_t>& outRawUs, uint32_t& outCarrierHz) const
    {
        std::ifstream in(m_irListPath);
        if (!in)
        {
            LOG_ERROR("RadaiWs: cannot open IR list {}", m_irListPath);
            return false;
        }

        json root;
        in >> root;
        if (!root.contains("commands") || !root["commands"].is_array())
        {
            LOG_ERROR("RadaiWs: IR list missing commands array ({})", m_irListPath);
            return false;
        }

        for (const auto& entry : root["commands"])
        {
            if (!entry.contains("id") || entry["id"].get<std::string>() != commandId)
                continue;

            if (!entry.contains("timings") || !entry["timings"].is_array())
                return false;

            outRawUs.clear();
            for (const auto& value : entry["timings"])
                outRawUs.push_back(static_cast<uint16_t>(value.get<uint32_t>()));

            outCarrierHz = entry.value("carrier_hz", 38000u);
            return !outRawUs.empty();
        }

        LOG_ERROR("RadaiWs: IR command '{}' not found in {}", commandId, m_irListPath);
        return false;
    }

    std::string matchIrCommandId(const std::vector<uint16_t>& timings) const
    {
        if (timings.empty())
            return {};

        std::ifstream in(m_irListPath);
        if (!in)
            return {};

        json root;
        in >> root;
        if (!root.contains("commands") || !root["commands"].is_array())
            return {};

        std::string bestId;
        double bestDistance = 1e9;
        for (const auto& entry : root["commands"])
        {
            if (!entry.contains("id") || !entry.contains("timings") || !entry["timings"].is_array())
                continue;

            std::vector<uint16_t> known;
            for (const auto& value : entry["timings"])
            {
                if (value.is_number_unsigned())
                    known.push_back(value.get<uint16_t>());
            }

            const double distance = timing_distance(timings, known);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestId = entry["id"].get<std::string>();
            }
        }

        return bestDistance <= 250.0 ? bestId : std::string {};
    }

    void enqueueMicFrame(AudioFrame frame)
    {
        std::lock_guard<std::mutex> lock(m_micMutex);
        if (m_micQueueSize == 0)
            return;

        while (m_micFrames.size() >= m_micQueueSize)
            m_micFrames.pop_front();
        m_micFrames.push_back(std::move(frame));
    }

    void setMicQueueSize(size_t size)
    {
        std::lock_guard<std::mutex> lock(m_micMutex);
        m_micQueueSize = size > 0 ? size : 1;
        while (m_micFrames.size() > m_micQueueSize)
            m_micFrames.pop_front();
    }

    size_t getMicQueueSize() const
    {
        std::lock_guard<std::mutex> lock(m_micMutex);
        return m_micQueueSize;
    }

    size_t getMicFrameCount() const
    {
        std::lock_guard<std::mutex> lock(m_micMutex);
        return m_micFrames.size();
    }

    bool getLatestMicFrame(AudioFrame& outFrame)
    {
        std::lock_guard<std::mutex> lock(m_micMutex);
        if (m_micFrames.empty())
            return false;
        outFrame = m_micFrames.back();
        return true;
    }

    bool popMicFrame(AudioFrame& outFrame)
    {
        std::lock_guard<std::mutex> lock(m_micMutex);
        if (m_micFrames.empty())
            return false;
        outFrame = std::move(m_micFrames.front());
        m_micFrames.pop_front();
        return true;
    }

    void handlePacket(const std::vector<uint8_t>& packet)
    {
        if (packet.size() < wsp::kHeaderSize)
            return;

        uint32_t magic = 0;
        std::memcpy(&magic, packet.data(), sizeof(magic));
        if (magic != wsp::kMagic)
            return;

        uint16_t rawType = 0;
        std::memcpy(&rawType, packet.data() + 6, sizeof(rawType));
        const uint32_t payloadSize = *reinterpret_cast<const uint32_t*>(packet.data() + 8);
        const uint8_t* payload = packet.data() + wsp::kHeaderSize;

        if (is_control_type(rawType))
            handleControl(rawType, payload, payloadSize);
        else
            handleData(rawType, packet[5], payload, payloadSize);
    }

    void handleControl(uint16_t rawType, const uint8_t* payload, uint32_t payloadSize)
    {
        const auto type = static_cast<wsp::Type>(rawType);

        if (type == wsp::Type::Ack)
        {
            if (payloadSize < wsp::kAckBodySize)
                return;

            wsp::AckBody body {};
            std::memcpy(&body, payload, sizeof(body));

            std::shared_ptr<std::promise<int>> prom;
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                const auto it = m_pendingAcks.find(body.requestId);
                if (it != m_pendingAcks.end())
                {
                    prom = it->second;
                    m_pendingAcks.erase(it);
                }
            }

            if (prom)
                prom->set_value(body.status == 0 ? 0 : -5);
            return;
        }

        if (type == wsp::Type::Error)
        {
            if (payloadSize < wsp::kErrorBodyFixedSize)
                return;

            wsp::ErrorBody body {};
            std::memcpy(&body, payload, sizeof(body));
            const size_t msgLen = wsp::error_msg_size(payloadSize);
            std::string message;
            if (msgLen > 0)
                message.assign(reinterpret_cast<const char*>(payload + sizeof(body)), msgLen);

            LOG_WARN("RadaiWs: device error requestId={} code={} msg={}", body.requestId, body.code, message);

            std::shared_ptr<std::promise<int>> prom;
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                const auto it = m_pendingAcks.find(body.requestId);
                if (it != m_pendingAcks.end())
                {
                    prom = it->second;
                    m_pendingAcks.erase(it);
                }
            }
            if (prom)
                prom->set_value(-5);
        }
    }

    void handleData(uint16_t rawType, uint8_t flags, const uint8_t* payload, uint32_t payloadSize)
    {
        const auto type = static_cast<wsp::Type>(rawType);
        size_t offset = 0;
        uint64_t timestampUs = 0;

        if (flags & wsp::HeaderFlag_HasTimestampBit)
        {
            if (payloadSize < wsp::kTimestampPrefixSize)
                return;
            wsp::TimestampPrefix ts {};
            std::memcpy(&ts, payload, sizeof(ts));
            timestampUs = ts.timestampUs;
            offset += wsp::kTimestampPrefixSize;
        }

        const uint8_t* body = payload + offset;
        const uint32_t bodySize = payloadSize - static_cast<uint32_t>(offset);

        switch (type)
        {
        case wsp::Type::MicPCM:
            if (bodySize >= wsp::kAudioPCMBodySize)
            {
                wsp::AudioPCMBody pcmBody {};
                std::memcpy(&pcmBody, body, sizeof(pcmBody));
                const size_t pcmBytes = wsp::pcm_data_size(pcmBody);
                if (bodySize >= wsp::kAudioPCMBodySize + pcmBytes)
                    m_owner.onMicPcm(pcmBody, body + wsp::kAudioPCMBodySize, timestampUs);
            }
            break;

        case wsp::Type::MicComp:
            if (bodySize >= wsp::kAudioCompBodySize)
            {
                wsp::AudioCompBody compBody {};
                std::memcpy(&compBody, body, sizeof(compBody));
                const size_t encSize = compBody.encodedSize;
                if (bodySize >= wsp::kAudioCompBodySize + encSize)
                {
                    const bool keyFrame = (flags & wsp::HeaderFlag_KeyFrameBit) != 0;
                    m_owner.onMicComp(compBody, body + wsp::kAudioCompBodySize, timestampUs, keyFrame);
                }
            }
            break;

        case wsp::Type::IrReceive:
            if (bodySize >= wsp::kIrReceiveBodySize)
            {
                wsp::IrReceiveBody irBody {};
                std::memcpy(&irBody, body, sizeof(irBody));
                const size_t rawBytes = static_cast<size_t>(irBody.length) * sizeof(uint16_t);
                if (bodySize >= wsp::kIrReceiveBodySize + rawBytes && irBody.length > 0)
                {
                    std::vector<uint16_t> timings(irBody.length);
                    std::memcpy(timings.data(), body + wsp::kIrReceiveBodySize, rawBytes);
                    m_owner.onIrReceived(irBody, timings);
                }
            }
            break;

        case wsp::Type::AmbientLight:
        case wsp::Type::Temperature:
        case wsp::Type::Humidity:
            if (bodySize >= wsp::kSensorBodySize)
            {
                wsp::SensorBody sensor {};
                std::memcpy(&sensor, body, sizeof(sensor));
                m_owner.onSensor(type, sensor);
            }
            else
            {
                LOG_WARN(
                    "RadaiWs: sensor type=0x{:04X} payload too small (bodySize={}, need {})",
                    rawType,
                    bodySize,
                    wsp::kSensorBodySize);
            }
            break;

        default:
            break;
        }
    }

    void onConnected()
    {
        m_owner.m_connectionState = RadaiWs::ConnectionState::Connected;
        sendHeartbeat();
        m_owner.onClientConnected();
        scheduleHeartbeat();
    }

    void scheduleHeartbeat()
    {
        if (!m_work || !m_connected.load())
            return;

        m_heartbeatTimer.expires_after(std::chrono::milliseconds(m_session.heartbeatIntervalMs));
        m_heartbeatTimer.async_wait([this](const std::error_code& ec)
        {
            if (ec || !m_work || !m_connected.load())
                return;

            sendHeartbeat();
            scheduleHeartbeat();
        });
    }

    bool shouldIgnoreIoError(const std::error_code& ec) const
    {
        return !m_work || ec == asio::error::operation_aborted;
    }

    void resolveInitialConnect()
    {
        if (!m_initConnectPending.exchange(false, std::memory_order_acq_rel))
            return;

        std::lock_guard lock(m_initConnectMutex);
        m_initConnectCv.notify_all();
    }

    bool failInitialConnectIfPending()
    {
        if (!m_initConnectPending.load(std::memory_order_acquire))
            return false;

        resolveInitialConnect();
        return true;
    }

    void openConnection()
    {
        if (!m_work || !io_loop_should_run(m_owner))
            return;

        m_connecting.store(true);
        m_owner.m_connectionState = RadaiWs::ConnectionState::Connecting;

        std::error_code ec;
        m_reconnectTimer.cancel();
        m_heartbeatTimer.cancel();
        m_connectTimer.cancel();
        m_resolver.cancel();
        m_socket.cancel(ec);
        m_socket.close(ec);

        m_connectTimer.expires_after(std::chrono::milliseconds(m_session.connectTimeoutMs));
        m_connectTimer.async_wait([this](const std::error_code& timerEc)
        {
            if (timerEc || !m_connecting.load() || !m_work)
                return;

            std::error_code cancelEc;
            m_socket.cancel(cancelEc);
            m_socket.close(cancelEc);
            m_connecting.store(false);
            LOG_WARN("RadaiWs connect timed out after {} ms", m_session.connectTimeoutMs);
            if (!failInitialConnectIfPending())
                scheduleReconnect("connect_timeout");
        });

        m_resolver.async_resolve(
            m_interface.host,
            std::to_string(m_interface.port),
            [this](const std::error_code& resolveEc, tcp::resolver::results_type endpoints)
            {
                if (shouldIgnoreIoError(resolveEc))
                {
                    m_connecting.store(false);
                    return;
                }

                if (resolveEc)
                {
                    m_connecting.store(false);
                    LOG_ERROR("RadaiWs resolve failed: {}", resolveEc.message());
                    if (!failInitialConnectIfPending())
                        scheduleReconnect("resolve_failed");
                    return;
                }

                asio::async_connect(
                    m_socket,
                    endpoints,
                    [this](const std::error_code& connectEc, const tcp::endpoint& endpoint)
                    {
                        m_connectTimer.cancel();

                        if (shouldIgnoreIoError(connectEc))
                        {
                            m_connecting.store(false);
                            return;
                        }

                        if (connectEc)
                        {
                            m_connecting.store(false);
                            if (!failInitialConnectIfPending())
                                scheduleReconnect("connect_failed");
                            return;
                        }

                        asio::socket_base::keep_alive keepAlive(true);
                        m_socket.set_option(keepAlive);
                        m_connected.store(true);
                        m_connecting.store(false);
                        if (m_reconnectAttempts > 0)
                        {
                            LOG_INFO(
                                "RadaiWs reconnected to {}:{} (backoff reset)",
                                endpoint.address().to_string(),
                                endpoint.port());
                        }
                        m_reconnectDelayMs = m_session.reconnectInitialMs;
                        m_reconnectAttempts = 0;
                        m_streamBuf.clear();

                        LOG_INFO(
                            "RadaiWs connected to {}:{}",
                            endpoint.address().to_string(),
                            endpoint.port());

                        resolveInitialConnect();
                        onConnected();
                        m_readBuf.resize(8192);
                        doRead();
                    });
            });
    }

    void scheduleReconnect(const char* reason)
    {
        if (!m_work || !io_loop_should_run(m_owner))
            return;

        m_connected.store(false);
        m_connecting.store(false);
        m_owner.m_connectionState = RadaiWs::ConnectionState::Reconnecting;

        m_heartbeatTimer.cancel();
        m_reconnectTimer.cancel();
        std::error_code ec;
        m_socket.cancel(ec);
        m_socket.close(ec);

        const uint32_t delay = m_reconnectDelayMs;
        m_reconnectDelayMs = std::min(m_reconnectDelayMs * 2u, m_session.reconnectMaxMs);
        ++m_reconnectAttempts;

        LOG_WARN("RadaiWs disconnected ({}) — retry #{} in {} ms", reason, m_reconnectAttempts, delay);

        m_reconnectTimer.expires_after(std::chrono::milliseconds(delay));
        m_reconnectTimer.async_wait([this, delay](const std::error_code& timerEc)
        {
            if (timerEc || !m_work || !io_loop_should_run(m_owner))
                return;

            LOG_INFO("RadaiWs reconnecting (delay was {} ms)", delay);
            openConnection();
        });
    }

    void doRead()
    {
        m_socket.async_read_some(
            asio::buffer(m_readBuf),
            [this](const std::error_code& ec, std::size_t bytes)
            {
                if (shouldIgnoreIoError(ec))
                    return;

                if (ec)
                {
                    LOG_ERROR("RadaiWs read failed: {}", ec.message());
                    scheduleReconnect("read_failed");
                    return;
                }

                m_streamBuf.insert(
                    m_streamBuf.end(),
                    m_readBuf.begin(),
                    m_readBuf.begin() + static_cast<ptrdiff_t>(bytes));

                while (true)
                {
                    std::vector<uint8_t> packet;
                    if (!try_extract_wsp_packet(m_streamBuf, packet))
                        break;
                    handlePacket(packet);
                }
                doRead();
            });
    }

    RadaiWs& m_owner;
    RadaiWs::InterfaceConfig m_interface;
    RadaiWs::SessionConfig m_session;
    RadaiWs::AudioConfig m_audio;
    std::string m_irListPath;

    asio::io_context m_io;
    std::unique_ptr<workGuard> m_work;
    std::thread m_ioThread;
    std::thread::id m_ioThreadId {};
    tcp::resolver m_resolver;
    tcp::socket m_socket;
    asio::steady_timer m_reconnectTimer{m_io};
    asio::steady_timer m_heartbeatTimer;
    asio::steady_timer m_connectTimer{m_io};

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_connecting{false};
    std::atomic<bool> m_initConnectPending{false};
    std::mutex m_initConnectMutex;
    std::condition_variable m_initConnectCv;
    uint32_t m_reconnectDelayMs = 1000;
    uint32_t m_reconnectAttempts = 0;
    std::atomic<uint32_t> m_requestId{0};
    std::unordered_map<uint16_t, uint32_t> m_sequences;

    std::mutex m_pendingMutex;
    std::unordered_map<uint32_t, std::shared_ptr<std::promise<int>>> m_pendingAcks;

    mutable std::mutex m_micMutex;
    std::deque<AudioFrame> m_micFrames;
    size_t m_micQueueSize = 8;

#ifdef WAVE_HAS_OPUS
    std::unique_ptr<OpusCodec> m_opus;
#endif

    std::vector<uint8_t> m_readBuf;
    std::vector<uint8_t> m_streamBuf;
};

RadaiWs::RadaiWs() :
    Device(),
    m_impl(std::make_unique<Impl>(*this))
{
    registerActionsAndQueries();
}

RadaiWs::~RadaiWs()
{
    shutdown();
}

const RadaiWs::InterfaceConfig& RadaiWs::getInterfaceConfig() const { return m_interface; }
const RadaiWs::AudioConfig& RadaiWs::getAudioConfig() const { return m_audio; }
const RadaiWs::SessionConfig& RadaiWs::getSessionConfig() const { return m_session; }
const RadaiWs::Capabilities& RadaiWs::getCapabilities() const { return m_capabilities; }
const RadaiWs::SubscriptionState& RadaiWs::getSubscriptionState() const { return m_subscriptions; }
RadaiWs::ConnectionState RadaiWs::getConnectionState() const { return m_connectionState; }

bool RadaiWs::isLinkConnected() const
{
    return m_impl && m_impl->isConnected();
}

bool RadaiWs::isIoActive() const
{
    return m_ioActive.load(std::memory_order_acquire);
}

int RadaiWs::init(const json& config)
{
    validate_radai_ws_config(config);
    loadBaseConfig(config);

    m_interface = parse_interface_config(config);
    m_audio = parse_audio_config(config);
    m_session = parse_session_config(config);
    m_capabilities = parse_capabilities(config);

    if (!isEnabled())
        return -2;

    if (m_state == DeviceState::Running)
        return 0;

    if (m_state != DeviceState::Uninitialized && m_state != DeviceState::Stopped)
        return -3;

    m_state = DeviceState::Initializing;

    const int macRc = verify_mac(m_interface.mac, m_interface.host);
    if (macRc != 0)
    {
        m_state = DeviceState::Stopped;
        return macRc;
    }

    if (!m_errorJson.contains("-4"))
        m_errorJson["-4"] = "connection failed";
    if (!m_errorJson.contains("-7"))
        m_errorJson["-7"] = "MAC mismatch";

    std::string irListPath = default_ir_list_path();
    if (config.contains("settings") && config["settings"].is_object())
        irListPath = config["settings"].value("ir_list_path", irListPath);

    m_impl->configure(m_interface, m_session, m_audio, irListPath);
    m_ioActive.store(true, std::memory_order_release);
    m_impl->start(true);

    if (!m_impl->waitForInitialConnect(m_session.connectTimeoutMs))
    {
        m_ioActive.store(false, std::memory_order_release);
        m_impl->stop();
        m_connectionState = ConnectionState::Disconnected;
        m_state = DeviceState::Stopped;
        return -4;
    }

    m_state = DeviceState::Running;
    LOG_INFO("RadaiWs initialized for {}:{}", m_interface.host, m_interface.port);
    return 0;
}

void RadaiWs::shutdown()
{
    if (m_state == DeviceState::Stopped || m_state == DeviceState::Uninitialized)
        return;

    m_state = DeviceState::ShuttingDown;
    m_ioActive.store(false, std::memory_order_release);
    m_impl->stop();
    m_connectionState = ConnectionState::Disconnected;
    m_state = DeviceState::Stopped;
}

std::string_view RadaiWs::getClass() const
{
    return kClass;
}

json RadaiWs::query(std::string_view name, const json& params)
{
    (void)params;

    if (name == "capabilities")
    {
        return json{
            {"mic_pcm", m_capabilities.micPcm},
            {"mic_opus", m_capabilities.micOpus},
            {"speaker_pcm", m_capabilities.speakerPcm},
            {"speaker_opus", m_capabilities.speakerOpus},
            {"ir_receive", m_capabilities.irReceive},
            {"ir_transmit", m_capabilities.irTransmit},
            {"ambient_light", m_capabilities.ambientLight},
            {"temperature", m_capabilities.temperature},
            {"humidity", m_capabilities.humidity},
        };
    }

    if (name == "session")
    {
        return json{
            {"host", m_interface.host},
            {"port", m_interface.port},
            {"connected", m_impl->isConnected()},
            {"state", connection_state_to_string(m_connectionState)},
            {"sample_rate", m_audio.sampleRate},
            {"channels", m_audio.channels},
            {"frame_duration_ms", m_audio.frameDurationMs},
        };
    }

    if (name == "status")
    {
        std::string lastIrCommandId;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_lastIr.valid)
                lastIrCommandId = m_lastIr.matchedCommandId;
        }

        return json{
            {"connected", m_impl->isConnected()},
            {"state", connection_state_to_string(m_connectionState)},
            {"mic_queue", m_impl->getMicFrameCount()},
            {"mic_level", m_micLevel},
            {"subscriptions", {
                {"mic_pcm", m_subscriptions.micPcm},
                {"mic_opus", m_subscriptions.micOpus},
                {"ir_receive", m_subscriptions.irReceive},
                {"ambient_light", m_subscriptions.ambientLight},
                {"temperature", m_subscriptions.temperature},
                {"humidity", m_subscriptions.humidity},
            }},
            {"last_ir_command_id", lastIrCommandId},
        };
    }

    if (name == "last_ir")
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_lastIr.valid)
            return json::object();

        json out = json::object();
        out["overflow"] = m_lastIr.overflow;
        out["length"] = m_lastIr.timingsUs.size();
        out["timings"] = m_lastIr.timingsUs;
        if (!m_lastIr.matchedCommandId.empty())
            out["commandId"] = m_lastIr.matchedCommandId;
        return out;
    }

    if (name == "mic_level")
        return json{{"level", m_micLevel}};

    if (name == "env")
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        json out = json::object();
        if (m_env.luxValid)
            out["lux"] = m_env.lux;
        if (m_env.temperatureValid)
            out["temperature_c"] = m_env.temperatureC;
        if (m_env.humidityValid)
            out["humidity_percent"] = m_env.humidityPercent;
        return out;
    }

    return make_query_error(-1, "unknown query");
}

std::future<json> RadaiWs::queryAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return query(name, params);
    });
}

int RadaiWs::invoke(std::string_view name, const json& params)
{
    if (name == "send_ir")
    {
        if (!params.contains("commandId") || !params["commandId"].is_string())
            return -1;

        std::vector<uint16_t> rawUs;
        uint32_t carrierHz = 38000;
        if (!m_impl->loadIrCommand(params["commandId"].get<std::string>(), rawUs, carrierHz))
            return -5;

        const uint16_t repeat = static_cast<uint16_t>(params.value("repeat", 0));
        return sendIrRaw(rawUs, carrierHz, repeat);
    }

    if (name == "subscribe")
    {
        if (!params.contains("target") || !params["target"].is_string())
            return -1;

        wsp::Type targetType {};
        if (!parse_target_type(params["target"].get<std::string>(), targetType))
            return -1;

        const bool isSensor =
            targetType == wsp::Type::AmbientLight
            || targetType == wsp::Type::Temperature
            || targetType == wsp::Type::Humidity;

        // Sensors default to 1000ms; audio/IR keep 0 (= max rate).
        const uint16_t defaultIntervalMs = isSensor ? m_subscriptions.sensorIntervalMs : 0;
        const uint16_t intervalMs = static_cast<uint16_t>(params.value("intervalMs", defaultIntervalMs));
        uint32_t options = wsp::SubscribeOptionFlag_None;
        if (params.value("compressed", false))
            options |= wsp::SubscribeOptionFlag_CompressedBits;
        if (params.value("on_change_only", false))
            options |= wsp::SubscribeOptionFlag_OnChangeOnlyBits;

        return subscribe(targetType, intervalMs, options);
    }

    if (name == "unsubscribe")
    {
        if (!params.contains("target") || !params["target"].is_string())
            return -1;

        wsp::Type targetType {};
        if (!parse_target_type(params["target"].get<std::string>(), targetType))
            return -1;

        return unsubscribe(targetType);
    }

    if (name == "speak")
    {
        LOG_WARN("RadaiWs: speak action not implemented yet");
        return -1;
    }

    return -1;
}

std::future<int> RadaiWs::invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return invoke(name, params);
    });
}

AudioFormat RadaiWs::getSourceFormat() const
{
    AudioFormat fmt;
    fmt.sampleRate = m_audio.sampleRate;
    fmt.sampleSize = m_audio.sampleSize;
    fmt.channels = m_audio.channels;
    return fmt;
}

void RadaiWs::setAudioQueueSize(size_t size)
{
    m_impl->setMicQueueSize(size);
}

size_t RadaiWs::getAudioQueueSize() const
{
    return m_impl->getMicQueueSize();
}

bool RadaiWs::getLatestFrame(AudioFrame& outFrame)
{
    ensureMicSubscription();
    return m_impl->getLatestMicFrame(outFrame);
}

std::future<void> RadaiWs::getLatestFrameAsync(AudioFrame& outFrame)
{
    return std::async(std::launch::async, [this, &outFrame]()
    {
        (void)getLatestFrame(outFrame);
    });
}

bool RadaiWs::popFrame(AudioFrame& outFrame)
{
    ensureMicSubscription();
    return m_impl->popMicFrame(outFrame);
}

AudioFormat RadaiWs::getSinkFormat() const
{
    return getSourceFormat();
}

bool RadaiWs::playFrame(const AudioFrame& frame)
{
    if (frame.samples.empty())
        return false;

#ifdef WAVE_HAS_OPUS
    if (m_audio.preferCompressedSpk && m_capabilities.speakerOpus)
    {
        const size_t frameSamples =
            static_cast<size_t>(m_audio.sampleRate) * m_audio.frameDurationMs / 1000 * m_audio.channels;
        if (frameSamples == 0)
            return false;

        const int16_t* pcm = frame.samples.data();
        size_t pcmCount = frame.samples.size();
        std::vector<int16_t> padded;
        if (pcmCount != frameSamples)
        {
            // Opus requires an exact frame size; pad/truncate to match.
            padded.assign(frameSamples, 0);
            std::memcpy(
                padded.data(),
                frame.samples.data(),
                std::min(pcmCount, frameSamples) * sizeof(int16_t));
            pcm = padded.data();
            pcmCount = frameSamples;
        }

        std::vector<uint8_t> encoded;
        if (!m_impl->encodeOpus(pcm, pcmCount, encoded, false))
            return false;
        return sendSpkOpus(encoded.data(), encoded.size(), false) == 0;
    }
#endif

    if (m_capabilities.speakerPcm)
        return sendSpkPcm(frame.samples.data(), frame.samples.size()) == 0;

    return false;
}

std::future<bool> RadaiWs::playFrameAsync(const AudioFrame& frame)
{
    return std::async(std::launch::async, [this, frame]()
    {
        return playFrame(frame);
    });
}

void RadaiWs::stopPlayback()
{
    const size_t frameSamples =
        static_cast<size_t>(m_audio.sampleRate) * m_audio.frameDurationMs / 1000 * m_audio.channels;
    std::vector<int16_t> silence(frameSamples > 0 ? frameSamples : 1, 0);

#ifdef WAVE_HAS_OPUS
    if (m_audio.preferCompressedSpk && m_capabilities.speakerOpus)
    {
        std::vector<uint8_t> encoded;
        if (!m_impl->encodeOpus(silence.data(), silence.size(), encoded, false))
            return;

        wsp::AudioCompBody body {};
        body.codec = static_cast<uint8_t>(wsp::AudioCodec::Opus);
        body.sampleRate = m_audio.sampleRate;
        body.channels = m_audio.channels;
        body.frameDurationMs = m_audio.frameDurationMs;
        body.encodedSize = static_cast<uint16_t>(encoded.size());
        (void)m_impl->sendData(
            wsp::Type::SpkComp,
            wsp::HeaderFlag_LastFrameBit,
            &body,
            sizeof(body),
            encoded.data(),
            encoded.size());
        return;
    }
#endif

    if (!m_capabilities.speakerPcm)
        return;

    wsp::AudioPCMBody body {};
    body.sampleRate = m_audio.sampleRate;
    body.channels = m_audio.channels;
    body.bitsPerSample = m_audio.sampleSize;
    body.sampleCount = static_cast<uint16_t>(silence.size() / m_audio.channels);
    (void)m_impl->sendData(
        wsp::Type::SpkPCM,
        wsp::HeaderFlag_LastFrameBit,
        &body,
        sizeof(body),
        silence.data(),
        silence.size() * sizeof(int16_t));
}

int RadaiWs::transmitTimings(
    const std::vector<uint16_t>& timingsUs,
    uint32_t carrierHz,
    uint16_t repeat)
{
    if (timingsUs.empty())
        return -1;

    return sendIrRaw(timingsUs, carrierHz, repeat);
}

std::future<int> RadaiWs::transmitTimingsAsync(
    const std::vector<uint16_t>& timingsUs,
    uint32_t carrierHz,
    uint16_t repeat)
{
    return std::async(std::launch::async, [this, timingsUs, carrierHz, repeat]()
    {
        return transmitTimings(timingsUs, carrierHz, repeat);
    });
}

bool RadaiWs::getLatestIr(IrTimingFrame& outFrame)
{
    if (ensureIrSubscription() != 0)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_lastIr.valid)
        return false;

    copy_ir_timing_frame(m_lastIr, outFrame);
    return true;
}

bool RadaiWs::waitForIr(IrTimingFrame& outFrame, uint32_t timeoutMs)
{
    if (ensureIrSubscription() != 0)
        return false;

    uint64_t generationBefore = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        generationBefore = m_irGeneration;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_lastIr.valid && m_irGeneration != generationBefore)
            {
                copy_ir_timing_frame(m_lastIr, outFrame);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return false;
}

std::future<bool> RadaiWs::getLatestIrAsync(IrTimingFrame& outFrame)
{
    return std::async(std::launch::async, [this, &outFrame]()
    {
        return getLatestIr(outFrame);
    });
}

std::future<bool> RadaiWs::waitForIrAsync(IrTimingFrame& outFrame, uint32_t timeoutMs)
{
    return std::async(std::launch::async, [this, &outFrame, timeoutMs]()
    {
        return waitForIr(outFrame, timeoutMs);
    });
}

void RadaiWs::registerActionsAndQueries()
{
    m_actions = {
        {
            Action::Json,
            Action::None,
            "send_ir",
            "Send a registered IR command",
            json{
                {"type", "object"},
                {"properties", {{"commandId", {{"type", "string"}}}}},
                {"required", json::array({"commandId"})},
            },
        },
        {
            Action::Json,
            Action::None,
            "subscribe",
            "Subscribe to a WSP1 stream",
            json{
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}}},
                    {"intervalMs", {{"type", "integer"}}},
                    {"compressed", {{"type", "boolean"}}},
                    {"on_change_only", {{"type", "boolean"}}},
                }},
                {"required", json::array({"target"})},
            },
        },
        {
            Action::Json,
            Action::None,
            "unsubscribe",
            "Unsubscribe from a WSP1 stream",
            json{
                {"type", "object"},
                {"properties", {{"target", {{"type", "string"}}}}},
                {"required", json::array({"target"})},
            },
        },
    };

    m_queries = {
        {Query::Json, "capabilities", "Device capability flags", json::object()},
        {Query::Json, "session", "Connection and audio session info", json::object()},
        {Query::Json, "status", "Connection and subscription status", json::object()},
        {Query::Json, "mic_level", "Recent microphone level (0..1)", json::object()},
        {Query::Json, "env", "Ambient lux / temperature / humidity snapshot", json::object()},
        {Query::Json, "last_ir", "Most recent IR receive (timings + matched commandId)", json::object()},
    };

    m_actionMap.clear();
    for (auto& action : m_actions)
        m_actionMap[action.name] = &action;

    m_queryMap.clear();
    for (auto& query : m_queries)
        m_queryMap[query.name] = &query;
}

int RadaiWs::startClient() { m_impl->start(false); return 0; }
void RadaiWs::stopClient() { m_impl->stop(); }

int RadaiWs::subscribe(wsp::Type targetType, uint16_t intervalMs, uint32_t options)
{
    const int rc = m_impl->sendSubscribe(targetType, intervalMs, options);
    if (rc != 0)
        return rc;

    switch (targetType)
    {
    case wsp::Type::MicPCM: m_subscriptions.micPcm = true; break;
    case wsp::Type::MicComp: m_subscriptions.micOpus = true; break;
    case wsp::Type::IrReceive: m_subscriptions.irReceive = true; break;
    case wsp::Type::AmbientLight:
        m_subscriptions.ambientLight = true;
        m_subscriptions.sensorIntervalMs = intervalMs;
        break;
    case wsp::Type::Temperature:
        m_subscriptions.temperature = true;
        m_subscriptions.sensorIntervalMs = intervalMs;
        break;
    case wsp::Type::Humidity:
        m_subscriptions.humidity = true;
        m_subscriptions.sensorIntervalMs = intervalMs;
        break;
    default: break;
    }
    return 0;
}

int RadaiWs::unsubscribe(wsp::Type targetType)
{
    const int rc = m_impl->sendUnsubscribe(targetType);
    if (rc != 0)
        return rc;

    switch (targetType)
    {
    case wsp::Type::MicPCM: m_subscriptions.micPcm = false; break;
    case wsp::Type::MicComp: m_subscriptions.micOpus = false; break;
    case wsp::Type::IrReceive: m_subscriptions.irReceive = false; break;
    case wsp::Type::AmbientLight: m_subscriptions.ambientLight = false; break;
    case wsp::Type::Temperature: m_subscriptions.temperature = false; break;
    case wsp::Type::Humidity: m_subscriptions.humidity = false; break;
    default: break;
    }
    return 0;
}

int RadaiWs::ensureMicSubscription()
{
    if (m_subscriptions.micPcm || m_subscriptions.micOpus)
        return 0;

#ifdef WAVE_HAS_OPUS
    if (m_audio.preferCompressedMic && m_capabilities.micOpus)
        return subscribe(wsp::Type::MicComp, 0, wsp::SubscribeOptionFlag_CompressedBits);
#endif

    if (m_capabilities.micPcm)
        return subscribe(wsp::Type::MicPCM, 0, wsp::SubscribeOptionFlag_None);

    return -5;
}

int RadaiWs::ensureIrSubscription()
{
    if (m_subscriptions.irReceive)
        return 0;

    if (!m_capabilities.irReceive)
        return -5;

    return subscribe(wsp::Type::IrReceive, 0, wsp::SubscribeOptionFlag_None);
}

int RadaiWs::sendHeartbeat() { return m_impl->sendHeartbeat(); }
int RadaiWs::sendIrRaw(const std::vector<uint16_t>& rawUs, uint32_t carrierHz, uint16_t repeat)
{
    return m_impl->sendIrRaw(rawUs, carrierHz, repeat);
}
int RadaiWs::sendSpkOpus(const uint8_t* data, size_t size, bool keyFrame)
{
    return m_impl->sendSpkOpus(data, size, keyFrame);
}
int RadaiWs::sendSpkPcm(const int16_t* samples, size_t sampleCount)
{
    return m_impl->sendSpkPcm(samples, sampleCount);
}

void RadaiWs::onPacketReceived(const uint8_t* data, size_t size)
{
    m_impl->handlePacket(std::vector<uint8_t>(data, data + size));
}

void RadaiWs::onMicPcm(const wsp::AudioPCMBody& body, const uint8_t* pcmData, uint64_t timestampUs)
{
    const size_t pcmBytes = wsp::pcm_data_size(body);
    if (pcmBytes == 0)
        return;

    const size_t sampleCount = pcmBytes / sizeof(int16_t);
    AudioFrame frame;
    frame.timestamp = timestampUs;
    frame.samples.resize(sampleCount);
    std::memcpy(frame.samples.data(), pcmData, pcmBytes);

    updateMicLevel(compute_rms_level(frame.samples.data(), sampleCount));
    enqueueMicFrame(std::move(frame));
}

void RadaiWs::onMicComp(
    const wsp::AudioCompBody& body,
    const uint8_t* encodedData,
    uint64_t timestampUs,
    bool keyFrame)
{
#ifdef WAVE_HAS_OPUS
    std::vector<int16_t> pcm;
    if (!m_impl->decodeOpus(encodedData, body.encodedSize, pcm, keyFrame))
        return;

    AudioFrame frame;
    frame.timestamp = timestampUs;
    frame.samples = std::move(pcm);
    updateMicLevel(compute_rms_level(frame.samples.data(), frame.samples.size()));
    enqueueMicFrame(std::move(frame));
#else
    (void)body;
    (void)encodedData;
    (void)timestampUs;
    (void)keyFrame;
    LOG_WARN("RadaiWs: received MicComp but Opus support is not compiled in");
#endif
}

void RadaiWs::enqueueMicFrame(AudioFrame frame)
{
    m_impl->enqueueMicFrame(std::move(frame));
}

void RadaiWs::updateMicLevel(float level)
{
    m_micLevel = level;
}

void RadaiWs::updateEnv(const EnvSnapshot& env)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_env = env;
}

void RadaiWs::onClientConnected()
{
    if (m_capabilities.irReceive)
        subscribe(wsp::Type::IrReceive, 0, wsp::SubscribeOptionFlag_None);

    if (m_capabilities.ambientLight)
    {
        subscribe(
            wsp::Type::AmbientLight,
            m_subscriptions.sensorIntervalMs,
            wsp::SubscribeOptionFlag_OnChangeOnlyBits);
    }
    if (m_capabilities.temperature)
    {
        subscribe(
            wsp::Type::Temperature,
            m_subscriptions.sensorIntervalMs,
            wsp::SubscribeOptionFlag_OnChangeOnlyBits);
    }
    if (m_capabilities.humidity)
    {
        subscribe(
            wsp::Type::Humidity,
            m_subscriptions.sensorIntervalMs,
            wsp::SubscribeOptionFlag_OnChangeOnlyBits);
    }
}

void RadaiWs::onSensor(wsp::Type type, const wsp::SensorBody& sensor)
{
    const char* name = "unknown";
    switch (type)
    {
    case wsp::Type::AmbientLight: name = "ambient_light"; break;
    case wsp::Type::Temperature: name = "temperature"; break;
    case wsp::Type::Humidity: name = "humidity"; break;
    default: break;
    }

    if (sensor.quality == 0)
    {
        LOG_WARN(
            "RadaiWs: sensor {} discarded (quality=0, unit={}, value={})",
            name,
            static_cast<unsigned>(sensor.unit),
            sensor.value);
        return;
    }

    EnvSnapshot env;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        env = m_env;
    }
    env.updatedAt = std::chrono::steady_clock::now();

    switch (type)
    {
    case wsp::Type::AmbientLight:
        env.luxValid = true;
        env.lux = sensor.value;
        break;
    case wsp::Type::Temperature:
        env.temperatureValid = true;
        env.temperatureC = sensor.value;
        break;
    case wsp::Type::Humidity:
        env.humidityValid = true;
        env.humidityPercent = sensor.value;
        break;
    default:
        return;
    }

    updateEnv(env);
}

void RadaiWs::onIrReceived(const wsp::IrReceiveBody& body, const std::vector<uint16_t>& timings)
{
    IrTimingFrame snapshot;
    snapshot.valid = true;
    snapshot.overflow = body.overflow != 0;
    snapshot.timingsUs = timings;
    snapshot.matchedCommandId = m_impl->matchIrCommandId(timings);
    snapshot.receivedAt = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastIr = snapshot;
        ++m_irGeneration;
    }

    if (!snapshot.matchedCommandId.empty())
        LOG_INFO("RadaiWs: IR received matched command '{}'", snapshot.matchedCommandId);
    else if (snapshot.overflow)
        LOG_WARN("RadaiWs: IR received with overflow ({} pulses)", snapshot.timingsUs.size());
    else
        LOG_INFO("RadaiWs: IR received ({} pulses, no ir_list match)", snapshot.timingsUs.size());

#ifndef WAVE_STANDALONE_DEVICE_TEST
    service::notifyIrReceived(dev::deviceIDToString(getId()), timings);
#endif
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
