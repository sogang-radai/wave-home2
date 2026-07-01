#include "srs_r4sn.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>

#include <asio.hpp>

#include "r4sn/iq_protocol.h"
#include "../../core/logger.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    constexpr uint16_t kDefaultPointCloudPort = 29172;

    enum : uint32_t
    {
        kPacketMagic = 0xABCD4321,
    };

    enum : uint64_t
    {
        kFrameMagic = 0x0807060504030201,
        kTargetOffset = 48056,
    };

    struct PacketHeaderRaw
    {
        uint32_t reserved0;
        uint32_t magic;
        uint32_t reserved1;
        uint32_t reserved2;
        uint32_t packageSize;
        uint32_t reserved3;
        uint32_t reserved4;
        uint32_t reserved5;
        uint32_t reserved6;
    };

    struct FrameHeaderRaw
    {
        uint64_t magic;
        uint32_t frameCount;
        uint32_t targetNumber;
    };

    struct TargetRaw
    {
        float x;
        float y;
        uint32_t status;
        uint32_t targetId;
        float reserved0;
        float reserved1;
        float reserved2;
    };

    struct ParsedPoint
    {
        float x;
        float y;
        float z;
        float doppler;
        float power;
        int32_t targetId;
    };

    struct ParsedTarget
    {
        float x;
        float y;
        uint32_t targetId;
        float minX;
        float maxX;
        float minY;
        float maxY;
        float minZ;
        float maxZ;
    };

    uint64_t steadyUs()
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    }

    class ByteReader
    {
    public:
        ByteReader(const uint8_t* data, size_t size) :
            m_data(data),
            m_size(size),
            m_offset(0)
        {
        }

        size_t remaining() const
        {
            return m_size - m_offset;
        }

        bool seek(size_t offset)
        {
            if (offset > m_size)
                return false;

            m_offset = offset;
            return true;
        }

        template <typename T>
        bool read(T& out)
        {
            if (m_offset + sizeof(T) > m_size)
                return false;

            std::memcpy(&out, m_data + m_offset, sizeof(T));
            m_offset += sizeof(T);
            return true;
        }

        template <typename T>
        bool peek(T& out)
        {
            if (m_offset + sizeof(T) > m_size)
                return false;

            std::memcpy(&out, m_data + m_offset, sizeof(T));
            return true;
        }

    private:
        const uint8_t* m_data;
        size_t m_size;
        size_t m_offset;
    };

    bool findPacketMagic(const std::vector<uint8_t>& buffer, size_t& outOffset)
    {
        if (buffer.size() < 4)
            return false;

        for (size_t i = 0; i + 4 <= buffer.size(); ++i)
        {
            uint32_t value = 0;
            std::memcpy(&value, buffer.data() + i, sizeof(value));
            if (value == kPacketMagic)
            {
                outOffset = i;
                return true;
            }
        }

        return false;
    }

    bool tryExtractPacket(std::vector<uint8_t>& streamBuf, std::vector<uint8_t>& outPacketBuf)
    {
        size_t packetMagicOffset = 0;
        if (!findPacketMagic(streamBuf, packetMagicOffset))
        {
            if (streamBuf.size() > 7)
                streamBuf.erase(streamBuf.begin(), streamBuf.end() - 7);
            return false;
        }

        if (packetMagicOffset < 4)
            return false;

        if (packetMagicOffset > 4)
            streamBuf.erase(streamBuf.begin(), streamBuf.begin() + packetMagicOffset - 4);

        if (streamBuf.size() < sizeof(PacketHeaderRaw))
            return false;

        PacketHeaderRaw packetHeader{};
        std::memcpy(&packetHeader, streamBuf.data(), sizeof(PacketHeaderRaw));
        if (packetHeader.magic != kPacketMagic)
            return false;

        const size_t packetSize = sizeof(PacketHeaderRaw) + packetHeader.packageSize;
        if (streamBuf.size() < packetSize)
            return false;

        outPacketBuf.assign(streamBuf.begin(), streamBuf.begin() + packetSize);
        streamBuf.erase(streamBuf.begin(), streamBuf.begin() + packetSize);
        return true;
    }

    bool parsePacket(const std::vector<uint8_t>& packetBuf, RadarPointCloud& outFrame, uint64_t& lastTimestampUs)
    {
        const uint64_t now = steadyUs();
        if (lastTimestampUs == 0)
            outFrame.timestamp = now;
        else
            outFrame.timestamp = now;
        lastTimestampUs = now;

        ByteReader reader(packetBuf.data(), packetBuf.size());

        PacketHeaderRaw packetHeader{};
        FrameHeaderRaw frameHeader{};
        if (!reader.read(packetHeader))
            return false;

        if (packetHeader.magic != kPacketMagic)
            return false;

        if (!reader.read(frameHeader))
            return false;

        if (frameHeader.magic != kFrameMagic)
            return false;

        const uint32_t pointNumber = frameHeader.targetNumber;
        outFrame.frameIndex = frameHeader.frameCount;
        outFrame.points.clear();
        outFrame.targets.clear();
        outFrame.points.reserve(pointNumber);

        std::vector<ParsedPoint> parsedPoints;
        parsedPoints.reserve(pointNumber);

        for (uint32_t i = 0; i < pointNumber; ++i)
        {
            ParsedPoint point{};
            if (!reader.read(point.x) || !reader.read(point.y) || !reader.read(point.z) ||
                !reader.read(point.doppler) || !reader.read(point.power))
            {
                return false;
            }
            point.targetId = -1;
            parsedPoints.push_back(point);
        }

        for (uint32_t i = 0; i < pointNumber; ++i)
        {
            if (!reader.read(parsedPoints[i].targetId))
                return false;
        }

        for (const ParsedPoint& point : parsedPoints)
        {
            RadarPointCloud::Point outPoint{};
            outPoint.x = point.x;
            outPoint.y = point.y;
            outPoint.z = point.z;
            outPoint.doppler = point.doppler;
            outPoint.power = point.power;
            outFrame.points.push_back(outPoint);
        }

        if (reader.remaining() == 0)
            return true;

        if (!reader.peek(frameHeader))
            return true;

        if (frameHeader.magic != kFrameMagic)
        {
            if (!reader.seek(kTargetOffset))
                return true;

            if (!reader.read(frameHeader))
                return true;

            if (frameHeader.magic != kFrameMagic)
                return true;

            const uint32_t targetNumber = frameHeader.targetNumber;
            for (uint32_t i = 0; i < targetNumber; ++i)
            {
                TargetRaw targetRaw{};
                if (!reader.read(targetRaw))
                    return false;

                RadarPointCloud::Target target{};
                target.targetId = targetRaw.targetId;
                target.minX = target.minY = target.minZ = std::numeric_limits<float>::max();
                target.maxX = target.maxY = target.maxZ = std::numeric_limits<float>::lowest();

                for (size_t pointIndex = 0; pointIndex < parsedPoints.size(); ++pointIndex)
                {
                    if (parsedPoints[pointIndex].targetId != static_cast<int32_t>(target.targetId))
                        continue;

                    const auto& point = parsedPoints[pointIndex];
                    target.minX = std::min(target.minX, point.x);
                    target.maxX = std::max(target.maxX, point.x);
                    target.minY = std::min(target.minY, point.y);
                    target.maxY = std::max(target.maxY, point.y);
                    target.minZ = std::min(target.minZ, point.z);
                    target.maxZ = std::max(target.maxZ, point.z);
                    target.pointIndices.push_back(static_cast<uint16_t>(pointIndex));
                }

                outFrame.targets.push_back(target);
            }
        }

        return true;
    }

    SRSR4SN::Config parseConfig(const json& config)
    {
        const auto& iface = config.at("interface");

        SRSR4SN::Config out;
        out.host = iface.at("host").get<std::string>();

        if (iface.contains("point_cloud"))
        {
            const auto& pointCloud = iface.at("point_cloud");
            if (pointCloud.contains("enabled"))
                out.pointCloudEnabled = pointCloud.at("enabled").get<bool>();
            if (pointCloud.contains("port"))
                out.pointCloudPort = static_cast<uint16_t>(pointCloud.at("port").get<uint32_t>());
        }
        else
        {
            out.pointCloudPort = kDefaultPointCloudPort;
        }

        if (iface.contains("iq"))
        {
            const auto& iq = iface.at("iq");
            if (iq.contains("enabled"))
                out.iqEnabled = iq.at("enabled").get<bool>();
            if (iq.contains("port"))
                out.iqPort = static_cast<uint16_t>(iq.at("port").get<uint32_t>());
        }

        return out;
    }

    SRSR4SN::Settings parseSettings(const json& config)
    {
        SRSR4SN::Settings settings;
        if (!config.contains("settings"))
            return settings;

        const auto& src = config.at("settings");
        if (src.contains("angle_z"))
            settings.angleZ = src.at("angle_z").get<float>();
        if (src.contains("angle_y"))
            settings.angleY = src.at("angle_y").get<float>();
        if (src.contains("min_x"))
            settings.minX = src.at("min_x").get<float>();
        if (src.contains("max_x"))
            settings.maxX = src.at("max_x").get<float>();
        if (src.contains("min_y"))
            settings.minY = src.at("min_y").get<float>();
        if (src.contains("max_y"))
            settings.maxY = src.at("max_y").get<float>();
        if (src.contains("min_z"))
            settings.minZ = src.at("min_z").get<float>();
        if (src.contains("max_z"))
            settings.maxZ = src.at("max_z").get<float>();

        return settings;
    }

    void validateSrsR4snConfig(const json& config)
    {
        if (config.at("class").get<std::string>() != "srs_r4sn")
            throw std::invalid_argument("srs_r4sn config field 'class' must be 'srs_r4sn'");

        if (!config.contains("interface") || !config["interface"].is_object())
            throw std::invalid_argument("srs_r4sn requires object field 'interface'");

        const auto& iface = config["interface"];
        if (!iface.contains("host") || !iface["host"].is_string() || iface["host"].get<std::string>().empty())
            throw std::invalid_argument("srs_r4sn interface requires non-empty string 'host'");

        if (iface.contains("point_cloud"))
        {
            if (!iface["point_cloud"].is_object())
                throw std::invalid_argument("srs_r4sn interface field 'point_cloud' must be an object");

            const auto& pointCloud = iface["point_cloud"];
            if (pointCloud.contains("enabled") && !pointCloud["enabled"].is_boolean())
                throw std::invalid_argument("srs_r4sn point_cloud field 'enabled' must be a boolean");
            if (pointCloud.contains("port") && !pointCloud["port"].is_number_unsigned())
                throw std::invalid_argument("srs_r4sn point_cloud field 'port' must be an unsigned integer");
        }

        if (iface.contains("iq"))
        {
            if (!iface["iq"].is_object())
                throw std::invalid_argument("srs_r4sn interface field 'iq' must be an object");

            const auto& iq = iface["iq"];
            if (iq.contains("enabled") && !iq["enabled"].is_boolean())
                throw std::invalid_argument("srs_r4sn iq field 'enabled' must be a boolean");
            if (iq.contains("port") && !iq["port"].is_number_unsigned())
                throw std::invalid_argument("srs_r4sn iq field 'port' must be an unsigned integer");
        }

        if (config.contains("settings"))
        {
            if (!config["settings"].is_object())
                throw std::invalid_argument("srs_r4sn field 'settings' must be an object");

            const auto& settings = config["settings"];
            for (const char* key : {"angle_z", "angle_y", "min_x", "max_x", "min_y", "max_y", "min_z", "max_z"})
            {
                if (settings.contains(key) && !settings[key].is_number())
                    throw std::invalid_argument(std::string("srs_r4sn settings field '") + key + "' must be a number");
            }
        }
    }
}

struct SRSR4SN::Impl
{
    using tcp = asio::ip::tcp;

    using workGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    Impl(std::string host, uint16_t port, size_t queueSize) :
        m_host(std::move(host)),
        m_port(port),
        m_queueSize(queueSize > 0 ? queueSize : 1),
        m_resolver(m_io),
        m_socket(m_io)
    {
    }

    void start()
    {
        m_work = std::make_unique<workGuard>(asio::make_work_guard(m_io));
        m_ioThread = std::thread([this]()
        {
            m_io.run();
        });

        m_resolver.async_resolve(
            m_host,
            std::to_string(m_port),
            [this](const std::error_code& ec, tcp::resolver::results_type endpoints)
            {
                if (ec)
                {
                    LOG_ERROR("srs_r4sn resolve failed: {}", ec.message());
                    m_connected.store(false);
                    return;
                }

                asio::async_connect(
                    m_socket,
                    endpoints,
                    [this](const std::error_code& connectEc, const tcp::endpoint& endpoint)
                    {
                        if (connectEc)
                        {
                            LOG_ERROR("srs_r4sn connect failed: {}", connectEc.message());
                            m_connected.store(false);
                            return;
                        }

                        asio::socket_base::keep_alive option(true);
                        m_socket.set_option(option);
                        m_connected.store(true);

                        LOG_INFO(
                            "srs_r4sn connected to {}:{}",
                            endpoint.address().to_string(),
                            endpoint.port());

                        m_readBuf.resize(8192);
                        doRead();
                    });
            });
    }

    void stop()
    {
        std::error_code ec;
        m_resolver.cancel();
        m_socket.cancel(ec);
        m_socket.shutdown(tcp::socket::shutdown_both, ec);
        m_socket.close(ec);

        m_work.reset();
        if (m_ioThread.joinable())
            m_ioThread.join();

        m_connected.store(false);
    }

    bool isConnected() const
    {
        return m_connected.load();
    }

    void setQueueSize(size_t size)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queueSize = size > 0 ? size : 1;
        while (m_frames.size() > m_queueSize)
            m_frames.pop_front();
    }

    size_t getQueueSize() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queueSize;
    }

    void enumerateFrameIndices(std::vector<uint64_t>& indices) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        indices.clear();
        indices.reserve(m_frames.size());
        for (const auto& frame : m_frames)
            indices.push_back(frame.frameIndex);
    }

    bool hasFrame(uint64_t frameIdx) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& frame : m_frames)
        {
            if (frame.frameIndex == frameIdx)
                return true;
        }
        return false;
    }

    bool getFrame(uint64_t frameIdx, RadarPointCloud& outFrame) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& frame : m_frames)
        {
            if (frame.frameIndex == frameIdx)
            {
                outFrame = frame;
                return true;
            }
        }
        return false;
    }

    bool getLatestFrame(RadarPointCloud& outFrame) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frames.empty())
            return false;

        outFrame = m_frames.back();
        return true;
    }

private:
    void doRead()
    {
        m_socket.async_read_some(
            asio::buffer(m_readBuf),
            [this](const std::error_code& ec, std::size_t bytes)
            {
                if (ec)
                {
                    LOG_ERROR("srs_r4sn read failed: {}", ec.message());
                    m_connected.store(false);
                    return;
                }

                m_streamBuf.insert(m_streamBuf.end(), m_readBuf.begin(), m_readBuf.begin() + static_cast<ptrdiff_t>(bytes));

                while (true)
                {
                    std::vector<uint8_t> packetBuf;
                    if (!tryExtractPacket(m_streamBuf, packetBuf))
                        break;

                    RadarPointCloud frame;
                    if (parsePacket(packetBuf, frame, m_lastFrameTimestampUs))
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        while (m_frames.size() >= m_queueSize)
                            m_frames.pop_front();
                        m_frames.push_back(std::move(frame));
                    }
                    else
                    {
                        LOG_ERROR("srs_r4sn failed to parse point cloud frame");
                    }
                }

                doRead();
            });
    }

    std::string m_host;
    uint16_t m_port;
    size_t m_queueSize;

    asio::io_context m_io;
    std::unique_ptr<workGuard> m_work;
    std::thread m_ioThread;

    tcp::resolver m_resolver;
    tcp::socket m_socket;
    std::atomic<bool> m_connected{false};

    std::vector<uint8_t> m_readBuf;
    std::vector<uint8_t> m_streamBuf;
    uint64_t m_lastFrameTimestampUs = 0;

    mutable std::mutex m_mutex;
    std::deque<RadarPointCloud> m_frames;
};

struct SRSR4SN::IqImpl
{
    using tcp = asio::ip::tcp;

    IqImpl(std::string host, uint16_t port) :
        m_host(std::move(host)),
        m_port(port),
        m_socket(m_io)
    {
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        disconnectLocked();
    }

    bool request(
        const std::vector<RadarIQRequest>& requests,
        std::vector<RadarIQResponse>& outResponses,
        std::string& error)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (requests.empty())
        {
            outResponses.clear();
            return true;
        }

        if (!ensureConnectedLocked(error))
            return false;

        iq::IqRequestMsg message{};
        message.request.type = iq::RequestType::Iq;
        message.request.chirp_mode = iq::ChirpMode::Average;
        message.request.chirp_select_mode = iq::ChirpSelectMode::All;
        message.request.va_select_mode = iq::VaSelectMode::Single;
        message.request.va_combine_mode = iq::VaCombineMode::Beamform;
        message.request.tile = 0;
        message.request.sub_ant = 0;
        message.request.target_count = static_cast<uint16_t>(requests.size());
        message.request.azimuth = requests.front().azimuth;
        message.request.elevation = requests.front().elevation;
        message.distances.reserve(requests.size());
        for (const RadarIQRequest& request : requests)
            message.distances.push_back(request.distance);

        const std::string validationError = iq::validateIqRequest(message.request);
        if (!validationError.empty())
        {
            error = validationError;
            return false;
        }

        const std::vector<uint8_t> wire = iq::writeIqRequest(message);
        std::error_code ec;
        asio::write(m_socket, asio::buffer(wire), ec);
        if (ec)
        {
            error = "iq write failed: " + ec.message();
            disconnectLocked();
            return false;
        }

        std::vector<uint8_t> prefix(iq::kResponseHeaderSize + iq::kIqResponseInfoSize);
        asio::read(m_socket, asio::buffer(prefix), ec);
        if (ec)
        {
            error = "iq read header failed: " + ec.message();
            disconnectLocked();
            return false;
        }

        iq::ResponseHeader header{};
        std::memcpy(&header, prefix.data(), iq::kResponseHeaderSize);
        if (header.magic != iq::kResponseMagic)
        {
            error = "iq response has invalid magic";
            disconnectLocked();
            return false;
        }

        iq::IqResponseInfo info{};
        std::memcpy(&info, prefix.data() + iq::kResponseHeaderSize, iq::kIqResponseInfoSize);

        const size_t bodyBytes = iq::iqResponsePayloadBytes(info, iq::ChirpMode::Average);
        std::vector<uint8_t> body(bodyBytes);
        if (bodyBytes > 0)
        {
            asio::read(m_socket, asio::buffer(body), ec);
            if (ec)
            {
                error = "iq read payload failed: " + ec.message();
                disconnectLocked();
                return false;
            }
        }

        std::vector<uint8_t> responseData;
        responseData.reserve(prefix.size() + body.size());
        responseData.insert(responseData.end(), prefix.begin(), prefix.end());
        responseData.insert(responseData.end(), body.begin(), body.end());

        iq::IqResponse response{};
        if (!iq::readIqResponse(std::span<const uint8_t>(responseData), iq::ChirpMode::Average, response, error))
        {
            disconnectLocked();
            return false;
        }

        if (response.header.status != static_cast<uint32_t>(iq::IqStatus::Ok))
        {
            error = "iq server returned status " + std::to_string(response.header.status);
            return false;
        }

        const size_t samplesPerTarget = iq::chirpSampleCount(iq::ChirpMode::Average);
        outResponses.clear();
        outResponses.reserve(response.info.target_count);
        for (uint16_t targetIndex = 0; targetIndex < response.info.target_count; ++targetIndex)
        {
            const size_t sampleIndex = static_cast<size_t>(targetIndex) * samplesPerTarget;
            if (sampleIndex >= response.payload.size())
            {
                error = "iq response payload shorter than target_count";
                return false;
            }

            RadarIQResponse out{};
            out.iq.real = response.payload[sampleIndex].i;
            out.iq.imag = response.payload[sampleIndex].q;
            outResponses.push_back(out);
        }

        return true;
    }

private:
    bool ensureConnectedLocked(std::string& error)
    {
        if (m_connected && m_socket.is_open())
            return true;

        disconnectLocked();

        tcp::resolver resolver(m_io);
        std::error_code ec;
        const auto endpoints = resolver.resolve(m_host, std::to_string(m_port), ec);
        if (ec)
        {
            error = "iq resolve failed: " + ec.message();
            return false;
        }

        asio::connect(m_socket, endpoints, ec);
        if (ec)
        {
            error = "iq connect failed: " + ec.message();
            disconnectLocked();
            return false;
        }

        asio::ip::tcp::no_delay noDelay(true);
        m_socket.set_option(noDelay, ec);
        m_connected = true;
        return true;
    }

    void disconnectLocked()
    {
        std::error_code ec;
        if (m_socket.is_open())
        {
            m_socket.shutdown(tcp::socket::shutdown_both, ec);
            m_socket.close(ec);
        }
        m_connected = false;
    }

    std::string m_host;
    uint16_t m_port;
    asio::io_context m_io;
    tcp::socket m_socket;
    bool m_connected = false;
    std::mutex m_mutex;
};

// ============================================================================
// SRSR4SN
// ============================================================================

SRSR4SN::SRSR4SN() :
    Device()
{
    registerQueries();
}

SRSR4SN::~SRSR4SN()
{
    shutdown();
}

const SRSR4SN::Config& SRSR4SN::getConfig() const
{
    return m_config;
}

const SRSR4SN::Settings& SRSR4SN::getSettings() const
{
    return m_settings;
}

// ============================================================================
// Device
// ============================================================================

int SRSR4SN::init(const json& config)
{
    validateSrsR4snConfig(config);
    loadBaseConfig(config);
    m_config = parseConfig(config);
    m_settings = parseSettings(config);

    if (!isEnabled())
        return -2;

    if (m_state == DeviceState::Running)
        return 0;

    if (m_state != DeviceState::Uninitialized && m_state != DeviceState::Stopped)
        return -3;

    if (!m_config.pointCloudEnabled)
        return -8;

    m_state = DeviceState::Initializing;

    try
    {
        m_impl = std::make_unique<Impl>(
            m_config.host,
            m_config.pointCloudPort,
            m_pointCloudQueueSize);
        m_impl->start();
        m_state = DeviceState::Running;
        return 0;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("srs_r4sn init failed: {}", ex.what());
        m_impl.reset();
        m_state = DeviceState::Stopped;
        return -5;
    }
}

void SRSR4SN::shutdown()
{
    if (m_state == DeviceState::Uninitialized)
        return;

    m_state = DeviceState::ShuttingDown;

    if (m_impl)
    {
        m_impl->stop();
        m_impl.reset();
    }

    if (m_iqImpl)
    {
        m_iqImpl->close();
        m_iqImpl.reset();
    }

    m_state = DeviceState::Stopped;
}

std::string_view SRSR4SN::getClass() const
{
    return "srs_r4sn";
}

// ============================================================================
// Queryable
// ============================================================================

json SRSR4SN::query(std::string_view name, const json& params)
{
    (void)params;

    const Query* entry = findQuery(name);
    if (!entry)
        return json{{"code", -8}};

    if (entry->type == Query::Interface)
        return json{{"code", -8}, {"message", "use provider interface"}};

    return json{{"code", -8}};
}

std::future<json> SRSR4SN::queryAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return query(name, params);
    });
}

// ============================================================================
// IRadarPointCloudProvider
// ============================================================================

void SRSR4SN::setPointCloudQueueSize(size_t size)
{
    m_pointCloudQueueSize = size > 0 ? size : 1;
    if (m_impl)
        m_impl->setQueueSize(m_pointCloudQueueSize);
}

size_t SRSR4SN::getPointCloudQueueSize() const
{
    if (m_impl)
        return m_impl->getQueueSize();
    return m_pointCloudQueueSize;
}

void SRSR4SN::enumeratePointCloudFrameIndices(std::vector<uint64_t>& indices)
{
    if (!m_impl)
    {
        indices.clear();
        return;
    }

    m_impl->enumerateFrameIndices(indices);
}

bool SRSR4SN::isPointCloudFrameAvailable(uint64_t frameIdx) const
{
    if (!m_impl)
        return false;

    return m_impl->hasFrame(frameIdx);
}

bool SRSR4SN::getPointCloudFrame(uint64_t frameIdx, RadarPointCloud& outFrame)
{
    if (!m_impl)
        return false;

    return m_impl->getFrame(frameIdx, outFrame);
}

std::future<void> SRSR4SN::getLatestPointCloudFrameAsync(RadarPointCloud& outFrame)
{
    return std::async(std::launch::async, [this, &outFrame]()
    {
        if (!m_impl)
            return;

        m_impl->getLatestFrame(outFrame);
    });
}

// ============================================================================
// IRadarIQProvider
// ============================================================================

std::future<void> SRSR4SN::requestIQAsync(
    const std::vector<RadarIQRequest>& requests,
    std::vector<RadarIQResponse>& outResponses)
{
    return std::async(std::launch::async, [this, requests, &outResponses]()
    {
        outResponses.clear();
        if (!m_config.iqEnabled)
            return;

        if (!m_iqImpl)
            m_iqImpl = std::make_unique<IqImpl>(m_config.host, m_config.iqPort);

        std::string error;
        if (!m_iqImpl->request(requests, outResponses, error))
            LOG_ERROR("srs_r4sn IQ request failed: {}", error);
    });
}

void SRSR4SN::registerQueries()
{
    m_queries = {
        {Query::Interface, "point_cloud", "Radar point cloud stream", json::object()},
        {Query::Interface, "iq", "On-demand IQ samples", json::object()},
    };

    m_queryMap.clear();
    for (auto& query : m_queries)
        m_queryMap[query.name] = &query;
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
