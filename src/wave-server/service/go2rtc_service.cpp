#include "go2rtc_service.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <unistd.h>

extern char** environ;

#include "../core/logger.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    std::string default_binary_path()
    {
#ifdef WAVE_SOURCE_DIR
        return std::string(WAVE_SOURCE_DIR) + "/thirdparty/go2rtc/go2rtc";
#else
        return "go2rtc";
#endif
    }

    // Resolves an absolute ffmpeg path (PATH is stripped under sudo).
    std::string resolve_ffmpeg()
    {
        for (const char* p : {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"})
        {
            if (std::filesystem::exists(p))
                return p;
        }
        return "ffmpeg";
    }

    std::string url_encode(std::string_view value)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size() * 3);
        for (unsigned char c : value)
        {
            const bool unreserved =
                (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';
            if (unreserved)
            {
                out.push_back(static_cast<char>(c));
            }
            else
            {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0f]);
            }
        }
        return out;
    }

    std::string detect_lan_ip()
    {
        ifaddrs* interfaces = nullptr;
        if (getifaddrs(&interfaces) != 0)
            return {};

        std::string fallback;
        for (const ifaddrs* iface = interfaces; iface != nullptr; iface = iface->ifa_next)
        {
            if (iface->ifa_addr == nullptr || iface->ifa_addr->sa_family != AF_INET)
                continue;
            if ((iface->ifa_flags & IFF_UP) == 0 || (iface->ifa_flags & IFF_LOOPBACK) != 0)
                continue;

            char buffer[INET_ADDRSTRLEN] = {};
            const auto* addr = reinterpret_cast<const sockaddr_in*>(iface->ifa_addr);
            if (::inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer)) == nullptr)
                continue;

            const std::string ip(buffer);
            if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0)
            {
                freeifaddrs(interfaces);
                return ip;
            }
            if (fallback.empty())
                fallback = ip;
        }

        freeifaddrs(interfaces);
        return fallback;
    }

    int connect_tcp(const std::string& host, uint16_t port, int timeout_ms = 5000)
    {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* result = nullptr;
        const std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0 || result == nullptr)
            return -1;

        int fd = -1;
        for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next)
        {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
                continue;

            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            const int connect_rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (connect_rc == 0)
                break;

            if (connect_rc < 0 && errno != EINPROGRESS)
            {
                ::close(fd);
                fd = -1;
                continue;
            }

            pollfd pfd {};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int poll_rc = ::poll(&pfd, 1, timeout_ms);
            if (poll_rc <= 0)
            {
                ::close(fd);
                fd = -1;
                continue;
            }

            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 || socket_error != 0)
            {
                ::close(fd);
                fd = -1;
                continue;
            }

            if (flags >= 0)
                ::fcntl(fd, F_SETFL, flags);
            break;
        }
        freeaddrinfo(result);
        return fd;
    }

    bool parse_http_response(int fd, std::string& body, int& status_code)
    {
        body.clear();
        status_code = 0;

        std::string buffer;
        buffer.reserve(4096);
        char chunk[1024];
        while (buffer.find("\r\n\r\n") == std::string::npos)
        {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return false;
            buffer.append(chunk, static_cast<size_t>(n));
            if (buffer.size() > 65536)
                return false;
        }

        const size_t header_end = buffer.find("\r\n\r\n");
        const std::string headers = buffer.substr(0, header_end);
        body = buffer.substr(header_end + 4);

        if (std::sscanf(headers.c_str(), "HTTP/%*s %d", &status_code) < 1)
            return false;
        return status_code == 200;
    }

    // Minimal blocking HTTP/1.1 client for localhost control calls.
    int http_request(
        const std::string& host,
        uint16_t port,
        const std::string& method,
        const std::string& path_and_query,
        std::string* out_body = nullptr,
        const std::string* request_body = nullptr,
        const char* content_type = nullptr)
    {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* result = nullptr;
        const std::string portStr = std::to_string(port);
        if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0 || result == nullptr)
            return -1;

        int fd = -1;
        for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next)
        {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
                continue;
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
                break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(result);

        if (fd < 0)
            return -1;

        const std::string body = request_body ? *request_body : std::string();
        std::string request;
        request += method + " " + path_and_query + " HTTP/1.1\r\n";
        request += "Host: " + host + ":" + portStr + "\r\n";
        request += "Connection: close\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        if (content_type != nullptr)
            request += std::string("Content-Type: ") + content_type + "\r\n";
        request += "\r\n";
        request += body;

        size_t sent = 0;
        while (sent < request.size())
        {
            const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
            if (n <= 0)
            {
                ::close(fd);
                return -1;
            }
            sent += static_cast<size_t>(n);
        }

        // We send "Connection: close", so read until the peer closes.
        std::string response;
        char buffer[8192];
        while (true)
        {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n < 0)
            {
                ::close(fd);
                return -1;
            }
            if (n == 0)
                break;
            response.append(buffer, static_cast<size_t>(n));
        }
        ::close(fd);

        if (response.empty())
            return -1;

        int major = 0;
        int minor = 0;
        int code = 0;
        if (std::sscanf(response.c_str(), "HTTP/%d.%d %d", &major, &minor, &code) < 3)
            return -1;

        if (out_body != nullptr)
        {
            const size_t sep = response.find("\r\n\r\n");
            if (sep != std::string::npos)
                out_body->assign(response, sep + 4, std::string::npos);
        }

        return code;
    }
}

Go2RtcService& Go2RtcService::get()
{
    static Go2RtcService s_instance;
    return s_instance;
}

Go2RtcService::Go2RtcService()
{
    m_config.binaryPath = default_binary_path();
    m_config.ffmpegPath = resolve_ffmpeg();
}

Go2RtcService::~Go2RtcService()
{
    shutdownAll();
}

void Go2RtcService::shutdownAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_streams.clear();
    terminateProcess();
}

void Go2RtcService::configure(const Config& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pid > 0)
    {
        WLOG_WARN("Go2RtcService: configure() ignored while process is running");
        return;
    }
    m_config = config;
    if (m_config.binaryPath.empty())
        m_config.binaryPath = default_binary_path();
    if (m_config.ffmpegPath.empty())
        m_config.ffmpegPath = resolve_ffmpeg();
}

bool Go2RtcService::acquireStream(const std::string& name, const std::string& source)
{
    return acquireStream(name, std::vector<std::string>{source});
}

bool Go2RtcService::acquireStream(const std::string& name, const std::vector<std::string>& sources)
{
    if (name.empty() || sources.empty())
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);

    const bool freshStart = (m_pid < 0);
    m_streams[name] = sources;

    if (!ensureProcess())
    {
        m_streams.erase(name);
        return false;
    }

    if (!apiPutStream(name, sources))
    {
        m_streams.erase(name);
        if (m_pid > 0)
            apiDeleteStream(name);
        if (m_streams.empty())
            terminateProcess();
        return false;
    }

    if (freshStart)
        WLOG_INFO("Go2RtcService: started with {} stream(s)", m_streams.size());
    return true;
}

void Go2RtcService::releaseStream(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const auto it = m_streams.find(name);
    if (it == m_streams.end())
        return;

    m_streams.erase(it);

    if (m_pid > 0)
        apiDeleteStream(name);

    if (m_streams.empty())
    {
        terminateProcess();
        WLOG_INFO("Go2RtcService: last stream released, process stopped");
    }
}

bool Go2RtcService::isRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pid > 0;
}

size_t Go2RtcService::streamCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_streams.size();
}

std::string Go2RtcService::apiUrl() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return "http://" + m_config.apiHost + ":" + std::to_string(m_config.apiPort);
}

std::string Go2RtcService::streamRtspUrl(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return "rtsp://" + m_config.apiHost + ":" + std::to_string(m_config.rtspPort) + "/" + name;
}

bool Go2RtcService::fetchSnapshot(const std::string& name, std::vector<uint8_t>& out_jpeg, uint32_t width)
{
    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pid <= 0)
            return false;
        host = m_config.apiHost;
        port = m_config.apiPort;
    }

    std::string query = "/api/frame.jpeg?src=" + url_encode(name);
    if (width > 0)
        query += "&width=" + std::to_string(width);

    std::string body;
    const int code = http_request(host, port, "GET", query, &body);
    if (code != 200 || body.empty())
    {
        WLOG_ERROR("Go2RtcService: snapshot '{}' failed (http {}, {} bytes)", name, code, body.size());
        return false;
    }

    out_jpeg.assign(body.begin(), body.end());
    return true;
}

bool Go2RtcService::exchangeWebRtc(const std::string& name, const std::string& offer_sdp, std::string& answer_sdp)
{
    if (offer_sdp.empty())
        return false;

    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pid <= 0 || m_streams.find(name) == m_streams.end())
            return false;
        host = m_config.apiHost;
        port = m_config.apiPort;
    }

    const std::string query = "/api/webrtc?src=" + url_encode(name);
    const int code = http_request(
        host,
        port,
        "POST",
        query,
        &answer_sdp,
        &offer_sdp,
        "application/sdp");
    if ((code != 200 && code != 201) || answer_sdp.empty())
    {
        WLOG_ERROR("Go2RtcService: WebRTC exchange for '{}' failed (http {}, {} bytes)", name, code, answer_sdp.size());
        return false;
    }
    return true;
}

bool Go2RtcService::hasStream(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_streams.find(name) != m_streams.end();
}

bool Go2RtcService::getStreamEndpoint(const std::string& name, std::string& host, uint16_t& port) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pid <= 0 || m_streams.find(name) == m_streams.end())
        return false;
    host = m_config.apiHost;
    port = m_config.apiPort;
    return true;
}

bool Go2RtcService::streamToCamera(const std::string& name, const std::string& source)
{
    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pid <= 0)
            return false;
        host = m_config.apiHost;
        port = m_config.apiPort;
    }

    // Empty source stops any active playback for this destination.
    const std::string query =
        "/api/streams?dst=" + url_encode(name) + "&src=" + url_encode(source);
    std::string respBody;
    const int code = http_request(host, port, "POST", query, &respBody);
    if (code != 200)
    {
        WLOG_ERROR("Go2RtcService: streamToCamera '{}' failed (http {}): {}", name, code, respBody);
        return false;
    }

    if (source.empty())
        WLOG_INFO("Go2RtcService: stopped talk playback on '{}'", name);
    else
        WLOG_INFO("Go2RtcService: talk '{}' <- {}", name, source);
    return true;
}

bool Go2RtcService::ensureProcess()
{
    if (m_pid > 0)
        return true;

    if (!std::filesystem::exists(m_config.binaryPath))
    {
        WLOG_ERROR("Go2RtcService: binary not found at {}", m_config.binaryPath);
        return false;
    }

    if (!writeConfigFile())
        return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    const std::string logPath =
        (std::filesystem::temp_directory_path() / "wave_go2rtc.log").string();
    posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    const std::string binary = m_config.binaryPath;
    char* argv[] = {
        const_cast<char*>(binary.c_str()),
        const_cast<char*>("-config"),
        const_cast<char*>(m_configPath.c_str()),
        nullptr,
    };

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0)
    {
        WLOG_ERROR("Go2RtcService: posix_spawn failed: {}", std::strerror(rc));
        return false;
    }

    m_pid = pid;

    if (!waitForApi(5000))
    {
        WLOG_ERROR("Go2RtcService: API did not become ready");
        terminateProcess();
        return false;
    }

    WLOG_INFO(
        "Go2RtcService: process started (pid={}, api=http://{}:{})",
        m_pid, m_config.apiHost, m_config.apiPort);
    return true;
}

void Go2RtcService::terminateProcess()
{
    if (m_pid <= 0)
        return;

    const pid_t pid = static_cast<pid_t>(m_pid);
    ::kill(pid, SIGTERM);

    for (int i = 0; i < 30; ++i)
    {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0)
        {
            m_pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    m_pid = -1;
}

bool Go2RtcService::writeConfigFile()
{
    m_configPath = (std::filesystem::temp_directory_path() / "wave_go2rtc.yaml").string();

    std::ofstream file(m_configPath, std::ios::trunc);
    if (!file)
    {
        WLOG_ERROR("Go2RtcService: cannot write config at {}", m_configPath);
        return false;
    }

    file << "api:\n";
    file << "  listen: \"" << m_config.apiHost << ":" << m_config.apiPort << "\"\n";
    file << "rtsp:\n";
    file << "  listen: \":" << m_config.rtspPort << "\"\n";
    const std::string lan_ip = detect_lan_ip();
    file << "webrtc:\n";
    file << "  listen: \":8555\"\n";
    if (!lan_ip.empty())
    {
        file << "  candidates:\n";
        file << "    - " << lan_ip << "\n";
        WLOG_INFO("Go2RtcService: WebRTC candidate {}", lan_ip);
    }
    if (!m_config.ffmpegPath.empty())
    {
        file << "ffmpeg:\n";
        file << "  bin: \"" << m_config.ffmpegPath << "\"\n";
    }
    file << "log:\n";
    file << "  level: " << m_config.logLevel << "\n";
    return true;
}

bool Go2RtcService::waitForApi(uint32_t timeout_ms)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline)
    {
        const int code = http_request(m_config.apiHost, m_config.apiPort, "GET", "/api");
        if (code > 0)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool Go2RtcService::apiPutStream(const std::string& name, const std::vector<std::string>& sources)
{
    std::string query = "/api/streams?name=" + url_encode(name);
    for (const auto& source : sources)
        query += "&src=" + url_encode(source);

    const int code = http_request(m_config.apiHost, m_config.apiPort, "PUT", query);
    if (code != 200)
    {
        WLOG_ERROR("Go2RtcService: failed to add stream '{}' (http {})", name, code);
        return false;
    }

    for (const auto& source : sources)
        WLOG_INFO("Go2RtcService: stream '{}' -> {}", name, source);
    return true;
}

bool Go2RtcService::apiDeleteStream(const std::string& name)
{
    const std::string query = "/api/streams?src=" + url_encode(name);
    const int code = http_request(m_config.apiHost, m_config.apiPort, "DELETE", query);
    return code == 200;
}

struct Go2RtcService::LiveMp4Stream::Impl
{
    int fd = -1;
    std::string raw;
    std::string pending;
    size_t chunk_remaining = 0;
    enum class ChunkState
    {
        Size,
        Data,
        End,
    };
    ChunkState chunk_state = ChunkState::Size;

    enum class ChunkResult
    {
        NeedMore,
        End,
        Error,
    };

    ChunkResult pull_chunked()
    {
        while (true)
        {
            if (chunk_state == ChunkState::Size)
            {
                const size_t line_end = raw.find("\r\n");
                if (line_end == std::string::npos)
                    return ChunkResult::NeedMore;

                const std::string size_line = raw.substr(0, line_end);
                raw.erase(0, line_end + 2);

                char* end = nullptr;
                const unsigned long chunk_size = std::strtoul(size_line.c_str(), &end, 16);
                if (size_line.empty() || end == size_line.c_str())
                    return ChunkResult::Error;
                if (chunk_size == 0)
                {
                    if (raw.size() >= 2 && raw[0] == '\r' && raw[1] == '\n')
                        raw.erase(0, 2);
                    else if (!raw.empty() && raw[0] == '\n')
                        raw.erase(0, 1);
                    return ChunkResult::End;
                }

                chunk_remaining = static_cast<size_t>(chunk_size);
                chunk_state = ChunkState::Data;
            }

            if (chunk_state == ChunkState::Data)
            {
                if (raw.size() < chunk_remaining)
                    return ChunkResult::NeedMore;

                pending.append(raw, 0, chunk_remaining);
                raw.erase(0, chunk_remaining);
                chunk_remaining = 0;
                chunk_state = ChunkState::End;
            }

            if (chunk_state == ChunkState::End)
            {
                if (raw.empty())
                    return ChunkResult::NeedMore;
                if (raw[0] == '\r')
                {
                    if (raw.size() < 2)
                        return ChunkResult::NeedMore;
                    if (raw[1] != '\n')
                        return ChunkResult::Error;
                    raw.erase(0, 2);
                }
                else if (raw[0] == '\n')
                {
                    raw.erase(0, 1);
                }
                else
                {
                    return ChunkResult::Error;
                }
                chunk_state = ChunkState::Size;
            }
        }
    }

    bool fill_pending()
    {
        while (pending.empty())
        {
            if (!raw.empty())
            {
                switch (pull_chunked())
                {
                case ChunkResult::NeedMore:
                    break;
                case ChunkResult::End:
                    return false;
                case ChunkResult::Error:
                    WLOG_ERROR("Go2RtcService: invalid chunked stream.mp4 body");
                    return false;
                }
                if (!pending.empty())
                    return true;
            }

            char buffer[16384];
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0)
                return false;

            raw.append(buffer, static_cast<size_t>(n));
        }
        return true;
    }
};

Go2RtcService::LiveMp4Stream::LiveMp4Stream() :
    m_impl(std::make_unique<Impl>())
{
}

Go2RtcService::LiveMp4Stream::~LiveMp4Stream()
{
    close();
}

bool Go2RtcService::LiveMp4Stream::open(const std::string& name)
{
    close();
    if (name.empty())
        return false;

    std::string host;
    uint16_t port = 0;
    if (!Go2RtcService::get().getStreamEndpoint(name, host, port))
        return false;

    constexpr int kMaxAttempts = 8;
    constexpr auto kRetryDelay = std::chrono::milliseconds(250);

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        const int fd = connect_tcp(host, port);
        if (fd < 0)
        {
            if (attempt + 1 < kMaxAttempts)
                std::this_thread::sleep_for(kRetryDelay);
            continue;
        }

        const std::string path = "/api/stream.mp4?src=" + url_encode(name);
        std::string request;
        request += "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        request += "Connection: keep-alive\r\n";
        request += "\r\n";

        size_t sent = 0;
        while (sent < request.size())
        {
            const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
            if (n <= 0)
            {
                ::close(fd);
                break;
            }
            sent += static_cast<size_t>(n);
        }

        if (sent < request.size())
        {
            if (attempt + 1 < kMaxAttempts)
                std::this_thread::sleep_for(kRetryDelay);
            continue;
        }

        int status_code = 0;
        m_impl->raw.clear();
        if (!parse_http_response(fd, m_impl->raw, status_code))
        {
            WLOG_ERROR(
                "Go2RtcService: stream.mp4 for '{}' failed (http {}, attempt {}/{})",
                name,
                status_code,
                attempt + 1,
                kMaxAttempts);
            ::close(fd);
            if (attempt + 1 < kMaxAttempts)
                std::this_thread::sleep_for(kRetryDelay);
            continue;
        }

        m_impl->fd = fd;

        timeval tv {};
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        return true;
    }

    return false;
}

ssize_t Go2RtcService::LiveMp4Stream::read(char* buffer, size_t capacity)
{
    if (!m_impl || m_impl->fd < 0 || capacity == 0)
        return 0;

    if (!m_impl->fill_pending())
        return 0;

    const size_t n = std::min(capacity, m_impl->pending.size());
    std::memcpy(buffer, m_impl->pending.data(), n);
    m_impl->pending.erase(0, n);
    return static_cast<ssize_t>(n);
}

void Go2RtcService::LiveMp4Stream::close()
{
    if (!m_impl || m_impl->fd < 0)
        return;
    ::close(m_impl->fd);
    m_impl->fd = -1;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
