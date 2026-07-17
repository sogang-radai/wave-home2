#include "droid_cam.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "../../app/app_state.h"
#include "../../core/logger.h"
#include "network/net_util.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    constexpr std::string_view kClass = DroidCam::kClass;

    void validate_config(const json& config)
    {
        if (config.at("class").get<std::string>() != kClass)
            throw std::invalid_argument("droid_cam config field 'class' must be 'droid_cam'");

        if (!config.contains("interface") || !config["interface"].is_object())
            throw std::invalid_argument("droid_cam requires object field 'interface'");

        const auto& iface = config["interface"];
        if (!iface.contains("host") || !iface["host"].is_string() || iface["host"].get<std::string>().empty())
            throw std::invalid_argument("droid_cam interface requires non-empty string 'host'");
    }

    DroidCam::InterfaceConfig parse_interface_config(const json& config)
    {
        const auto& iface = config.at("interface");
        DroidCam::InterfaceConfig out;
        out.host = iface.at("host").get<std::string>();
        out.mac = iface.value("mac", "");
        out.port = static_cast<uint16_t>(iface.value("port", static_cast<int>(DroidCam::kDefaultPort)));
        out.videoPath = iface.value("video_path", "/video");
        return out;
    }

    DroidCam::AudioConfig parse_audio_config(const json& config)
    {
        DroidCam::AudioConfig out;
        if (!config.contains("audio") || !config["audio"].is_object())
            return out;

        const auto& audio = config["audio"];
        out.enabled = audio.value("enabled", false);
        out.audioPath = audio.value("path", "/audio");
        out.sampleRate = audio.value("sample_rate", 16000);
        out.channels = static_cast<uint8_t>(audio.value("channels", 1));
        out.sampleSize = static_cast<uint8_t>(audio.value("sample_size", 16));
        return out;
    }

  // Verifies the host still maps to the MAC pinned in the config. Returns -7
  // on mismatch, 0 when it matches or when no MAC is pinned / not resolvable.
    int verify_mac(const json& config, const std::string& host)
    {
        const std::string expected = config.at("interface").value("mac", "");
        if (expected.empty())
            return 0;

        std::string actual;
        if (!net::resolveMacForIp(host, actual))
        {
            WLOG_WARN("droid_cam: could not resolve MAC for {} (skipping check)", host);
            return 0;
        }
        if (!net::macEquals(expected, actual))
        {
            WLOG_ERROR("droid_cam: MAC mismatch for {} (expected {}, got {})", host, expected, actual);
            return -7;
        }
        WLOG_INFO("droid_cam: MAC verified for {} ({})", host, actual);
        return 0;
    }

    bool read_http_response_line(int fd, std::string& line, uint32_t timeout_ms)
    {
        line.clear();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline)
        {
            char ch = 0;
            const ssize_t n = ::recv(fd, &ch, 1, 0);
            if (n == 0)
                return false;
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    pollfd pfd {};
                    pfd.fd = fd;
                    pfd.events = POLLIN;
                    const int poll_rc = ::poll(&pfd, 1, 200);
                    if (poll_rc <= 0)
                        return false;
                    continue;
                }
                return false;
            }

            line.push_back(ch);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n')
            {
                line.resize(line.size() - 2);
                return true;
            }
        }
        return false;
    }

    int connect_http(const std::string& host, uint16_t port, uint32_t timeout_ms)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            ::close(fd);
            return -1;
        }

        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        const int connect_rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (connect_rc < 0 && errno != EINPROGRESS)
        {
            ::close(fd);
            return -1;
        }

        if (connect_rc != 0)
        {
            pollfd pfd {};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int poll_rc = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
            if (poll_rc <= 0)
            {
                ::close(fd);
                return -1;
            }

            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 || socket_error != 0)
            {
                ::close(fd);
                return -1;
            }
        }

        timeval tv {};
        tv.tv_sec = static_cast<int>(timeout_ms / 1000);
        tv.tv_usec = static_cast<int>((timeout_ms % 1000) * 1000);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (flags >= 0)
            ::fcntl(fd, F_SETFL, flags);
        return fd;
    }

    bool http_get_status(const std::string& host, uint16_t port, const std::string& path, int& status_code)
    {
        status_code = 0;
        const int fd = connect_http(host, port, 3000);
        if (fd < 0)
            return false;

        std::string request;
        request += "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        request += "Connection: close\r\n";
        request += "\r\n";

        size_t sent = 0;
        while (sent < request.size())
        {
            const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
            if (n <= 0)
            {
                ::close(fd);
                return false;
            }
            sent += static_cast<size_t>(n);
        }

        std::string status_line;
        if (!read_http_response_line(fd, status_line, 3000))
        {
            ::close(fd);
            return false;
        }

        while (true)
        {
            std::string line;
            if (!read_http_response_line(fd, line, 3000))
            {
                ::close(fd);
                return false;
            }
            if (line.empty())
                break;
        }

        ::close(fd);

        if (status_line.size() < 12 || status_line.rfind("HTTP/", 0) != 0)
            return false;

        const auto space = status_line.find(' ');
        if (space == std::string::npos)
            return false;
        const auto next_space = status_line.find(' ', space + 1);
        try
        {
            status_code = std::stoi(status_line.substr(
                space + 1,
                next_space == std::string::npos ? std::string::npos : next_space - space - 1));
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    bool read_bytes(int fd, std::vector<uint8_t>& out, size_t count, uint32_t timeout_ms)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (out.size() < count)
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;

            char buffer[4096];
            const size_t want = std::min(sizeof(buffer), count - out.size());
            const ssize_t n = ::recv(fd, buffer, want, 0);
            if (n == 0)
                return false;
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                return false;
            }
            out.insert(out.end(), buffer, buffer + n);
        }
        return true;
    }

    bool fetch_phone_jpeg(
        const std::string& host,
        uint16_t port,
        const std::string& path,
        std::vector<uint8_t>& out_jpeg)
    {
        out_jpeg.clear();
        const int fd = connect_http(host, port, 4000);
        if (fd < 0)
            return false;

        std::string request;
        request += "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        request += "Connection: close\r\n";
        request += "\r\n";

        size_t sent = 0;
        while (sent < request.size())
        {
            const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
            if (n <= 0)
            {
                ::close(fd);
                return false;
            }
            sent += static_cast<size_t>(n);
        }

        std::string status_line;
        if (!read_http_response_line(fd, status_line, 4000))
        {
            ::close(fd);
            return false;
        }

        size_t content_length = 0;
        while (true)
        {
            std::string line;
            if (!read_http_response_line(fd, line, 4000))
            {
                ::close(fd);
                return false;
            }
            if (line.empty())
                break;

            constexpr std::string_view kLength = "Content-Length:";
            if (line.rfind(kLength.data(), 0) == 0)
            {
                try
                {
                    content_length = static_cast<size_t>(std::stoul(line.substr(kLength.size())));
                }
                catch (...)
                {
                    content_length = 0;
                }
            }
        }

        std::vector<uint8_t> body;
        if (content_length > 0)
        {
            if (!read_bytes(fd, body, content_length, 4000))
            {
                ::close(fd);
                return false;
            }
        }
        else
        {
            char buffer[8192];
            while (true)
            {
                const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n == 0)
                    break;
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                body.insert(body.end(), buffer, buffer + n);
            }
        }
        ::close(fd);

        if (body.size() >= 2 && body[0] == 0xff && body[1] == 0xd8)
        {
            out_jpeg = std::move(body);
            return true;
        }

        for (size_t i = 0; i + 1 < body.size(); ++i)
        {
            if (body[i] != 0xff || body[i + 1] != 0xd8)
                continue;
            const auto end = std::find(body.begin() + static_cast<ptrdiff_t>(i + 2), body.end(), static_cast<uint8_t>(0xd9));
            if (end == body.end() || end == body.begin() || *(end - 1) != 0xff)
                continue;
            out_jpeg.assign(body.begin() + static_cast<ptrdiff_t>(i), end + 1);
            return true;
        }
        return false;
    }
}

DroidCam::DroidCam() :
    Device()
{
    registerQueries();
}

DroidCam::~DroidCam()
{
    shutdown();
}

const DroidCam::InterfaceConfig& DroidCam::getInterfaceConfig() const
{
    return m_interface;
}

const DroidCam::AudioConfig& DroidCam::getAudioConfig() const
{
    return m_audio;
}

const DroidCam::Capabilities& DroidCam::getCapabilities() const
{
    return m_capabilities;
}

std::string DroidCam::buildVideoUrl() const
{
    return "http://" + m_interface.host + ":" + std::to_string(m_interface.port) + m_interface.videoPath;
}

std::string DroidCam::buildAudioUrl() const
{
    return "http://" + m_interface.host + ":" + std::to_string(m_interface.port) + m_audio.audioPath;
}

int DroidCam::init(const json& config)
{
    validate_config(config);
    loadBaseConfig(config);
    m_interface = parse_interface_config(config);
    m_audio = parse_audio_config(config);

    m_capabilities.snapshot = true;
    m_capabilities.videoStream = true;
    m_capabilities.microphone = m_audio.enabled;

    if (!isEnabled())
        return -2;

    if (m_state == DeviceState::Running)
        return 0;

    if (m_state != DeviceState::Uninitialized && m_state != DeviceState::Stopped)
        return -3;

    m_state = DeviceState::Initializing;

    const int mac_rc = verify_mac(config, m_interface.host);
    if (mac_rc != 0)
    {
        m_state = DeviceState::Stopped;
        return mac_rc;
    }

    m_state = DeviceState::Running;
    WLOG_INFO("DroidCam initialized: {}:{}", m_interface.host, m_interface.port);

    m_appAlive.store(probeAppAlive(), std::memory_order_release);
    startHealthMonitor();

    return 0;
}

void DroidCam::shutdown()
{
    if (m_state == DeviceState::Uninitialized)
        return;

    m_state = DeviceState::ShuttingDown;
    stopHealthMonitor();
    m_appAlive.store(false, std::memory_order_release);
    m_state = DeviceState::Stopped;
}

std::string_view DroidCam::getClass() const
{
    return kClass;
}

bool DroidCam::isAppAlive() const
{
    return m_appAlive.load(std::memory_order_acquire);
}

void DroidCam::setStreamViewerCount(int viewers)
{
    m_streamViewers.store(std::max(0, viewers), std::memory_order_release);
}

bool DroidCam::probeAppAlive() const
{
    if (m_state != DeviceState::Running)
        return false;

    return isHostReachable();
}

void DroidCam::markPhoneOffline()
{
    m_streamViewers.store(0, std::memory_order_release);
    const bool was_alive = m_appAlive.exchange(false, std::memory_order_acq_rel);
    if (was_alive)
        WLOG_WARN("DroidCam: phone app offline ({}:{})", m_interface.host, m_interface.port);
    AppState::get().iot.resetCameraStreamSession(deviceIDToString(getId()));
    AppState::get().iot.stopDroidMjpegProxy(deviceIDToString(getId()));
}

void DroidCam::onAppWentOffline()
{
    markPhoneOffline();
}

void DroidCam::startHealthMonitor()
{
    stopHealthMonitor();
    m_healthStop.store(false, std::memory_order_release);

    m_healthThread = std::thread([this]()
    {
        while (!m_healthStop.load(std::memory_order_acquire))
        {
            for (int i = 0; i < 30; ++i)
            {
                if (m_healthStop.load(std::memory_order_acquire))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (m_state != DeviceState::Running)
                continue;

            const bool alive = probeAppAlive();
            const bool was_alive = m_appAlive.exchange(alive, std::memory_order_acq_rel);
            if (was_alive && !alive)
                onAppWentOffline();
            else if (!was_alive && alive)
            {
                WLOG_INFO("DroidCam: phone app online ({}:{})", m_interface.host, m_interface.port);
            }
        }
    });
}

void DroidCam::stopHealthMonitor()
{
    m_healthStop.store(true, std::memory_order_release);
    if (m_healthThread.joinable())
        m_healthThread.join();
}

bool DroidCam::isHostReachable() const
{
    if (m_state != DeviceState::Running)
        return false;

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_lastReachabilityCheck != std::chrono::steady_clock::time_point{}
            && now - m_lastReachabilityCheck < std::chrono::seconds(5))
        {
            return m_lastReachability;
        }
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_interface.port);
    if (::inet_pton(AF_INET, m_interface.host.c_str(), &addr.sin_addr) != 1)
    {
        ::close(fd);
        return false;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    const int connect_rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (connect_rc == 0)
    {
        ::close(fd);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastReachabilityCheck = now;
        m_lastReachability = true;
        return true;
    }

    if (connect_rc < 0 && errno != EINPROGRESS)
    {
        ::close(fd);
        return false;
    }

    pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int poll_rc = ::poll(&pfd, 1, 2000);
    if (poll_rc <= 0)
    {
        ::close(fd);
        return false;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0)
    {
        ::close(fd);
        return false;
    }

    ::close(fd);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastReachabilityCheck = now;
        m_lastReachability = socket_error == 0;
    }
    return socket_error == 0;
}

json DroidCam::query(std::string_view name, const json& params)
{
    (void)params;

    if (name == "capabilities")
    {
        json out = json::object();
        out["snapshot"] = m_capabilities.snapshot;
        out["video_stream"] = m_capabilities.videoStream;
        out["microphone"] = m_capabilities.microphone;
        return out;
    }

    if (name == "session")
    {
        json out = json::object();
        out["host"] = m_interface.host;
        out["port"] = m_interface.port;
        out["video_url"] = buildVideoUrl();
        if (m_audio.enabled)
            out["audio_url"] = buildAudioUrl();
        return out;
    }

    if (name == "status")
    {
        json out = json::object();
        out["reachable"] = isAppAlive();
        out["app_alive"] = isAppAlive();
        out["streaming"] = m_streamViewers.load(std::memory_order_acquire) > 0;
        out["micLevel"] = m_audio.enabled ? probeMicLevel() : 0.0;
        return out;
    }

    if (name == "stream")
    {
        json out = json::object();
        out["mjpeg"] = buildVideoUrl();
        return out;
    }

    json err = json::object();
    err["code"] = -8;
    return err;
}

std::future<json> DroidCam::queryAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return query(name, params);
    });
}

bool DroidCam::captureFrame(CameraFrame& outFrame)
{
    if (!isAppAlive())
        return false;

    std::vector<uint8_t> jpeg;
    if (!fetch_phone_jpeg(m_interface.host, m_interface.port, m_interface.videoPath, jpeg))
        return false;

    outFrame.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    outFrame.width = 0;
    outFrame.height = 0;
    outFrame.format = CameraFrame::Format::Jpeg;
    outFrame.data = std::move(jpeg);
    return true;
}

std::future<void> DroidCam::captureFrameAsync(CameraFrame& outFrame)
{
    return std::async(std::launch::async, [this, &outFrame]()
    {
        (void)captureFrame(outFrame);
    });
}

bool DroidCam::enumerateStreamProfiles(std::vector<CameraStreamProfile>& outProfiles)
{
    outProfiles.clear();

    CameraStreamProfile profile;
    profile.name = "mjpeg";
    profile.uri = buildVideoUrl();
    profile.codec = "mjpeg";
    outProfiles.push_back(std::move(profile));
    return true;
}

bool DroidCam::getStreamUri(std::string_view profile, std::string& outUri)
{
    if (profile != "mjpeg")
        return false;

    outUri = buildVideoUrl();
    return true;
}

AudioFormat DroidCam::getSourceFormat() const
{
    AudioFormat format;
    format.sampleRate = m_audio.sampleRate;
    format.sampleSize = m_audio.sampleSize;
    format.channels = m_audio.channels;
    return format;
}

void DroidCam::setAudioQueueSize(size_t size)
{
    (void)size;
}

size_t DroidCam::getAudioQueueSize() const
{
    return 0;
}

bool DroidCam::getLatestFrame(AudioFrame& outFrame)
{
    (void)outFrame;
    return false;
}

std::future<void> DroidCam::getLatestFrameAsync(AudioFrame& outFrame)
{
    return std::async(std::launch::async, [this, &outFrame]()
    {
        (void)getLatestFrame(outFrame);
    });
}

bool DroidCam::popFrame(AudioFrame& outFrame)
{
    (void)outFrame;
    return false;
}

void DroidCam::registerQueries()
{
    m_queries = {
        {Query::Json, "capabilities", "Snapshot / stream / microphone flags", json::object()},
        {Query::Json, "session", "HTTP endpoints", json::object()},
        {Query::Json, "status", "Reachability and streaming state", json::object()},
        {Query::Json, "stream", "MJPEG source URI", json::object()},
    };

    m_queryMap.clear();
    for (auto& query : m_queries)
        m_queryMap[query.name] = &query;
}

float DroidCam::probeMicLevel()
{
    return m_cachedMicLevel;
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
