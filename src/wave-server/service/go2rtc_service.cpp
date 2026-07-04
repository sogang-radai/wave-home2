#include "go2rtc_service.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../core/logger.h"

extern char** environ;

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

    // Minimal blocking HTTP/1.1 client for localhost control calls.
    // Returns the HTTP status code, or -1 on a connection/transport failure.
    // When out_body is non-null, the response body is captured into it.
    int http_request(
        const std::string& host,
        uint16_t port,
        const std::string& method,
        const std::string& path_and_query,
        std::string* out_body = nullptr)
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

        std::string request;
        request += method + " " + path_and_query + " HTTP/1.1\r\n";
        request += "Host: " + host + ":" + portStr + "\r\n";
        request += "Connection: close\r\n";
        request += "Content-Length: 0\r\n";
        request += "\r\n";

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
    std::lock_guard<std::mutex> lock(m_mutex);
    terminateProcess();
}

void Go2RtcService::configure(const Config& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pid > 0)
    {
        LOG_WARN("Go2RtcService: configure() ignored while process is running");
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

    m_streams[name] = sources;
    const bool freshStart = (m_pid < 0);

    if (!ensureProcess())
    {
        m_streams.erase(name);
        return false;
    }

    if (freshStart)
    {
        bool ok = true;
        for (const auto& [streamName, streamSources] : m_streams)
            ok = apiPutStream(streamName, streamSources) && ok;
        LOG_INFO("Go2RtcService: started with {} stream(s)", m_streams.size());
        return ok;
    }

    return apiPutStream(name, sources);
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
        LOG_INFO("Go2RtcService: last stream released, process stopped");
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
        LOG_ERROR("Go2RtcService: snapshot '{}' failed (http {}, {} bytes)", name, code, body.size());
        return false;
    }

    out_jpeg.assign(body.begin(), body.end());
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
        LOG_ERROR("Go2RtcService: streamToCamera '{}' failed (http {}): {}", name, code, respBody);
        return false;
    }

    if (source.empty())
        LOG_INFO("Go2RtcService: stopped talk playback on '{}'", name);
    else
        LOG_INFO("Go2RtcService: talk '{}' <- {}", name, source);
    return true;
}

bool Go2RtcService::ensureProcess()
{
    if (m_pid > 0)
        return true;

    if (!std::filesystem::exists(m_config.binaryPath))
    {
        LOG_ERROR("Go2RtcService: binary not found at {}", m_config.binaryPath);
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
        LOG_ERROR("Go2RtcService: posix_spawn failed: {}", std::strerror(rc));
        return false;
    }

    m_pid = pid;

    if (!waitForApi(5000))
    {
        LOG_ERROR("Go2RtcService: API did not become ready");
        terminateProcess();
        return false;
    }

    LOG_INFO(
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
        LOG_ERROR("Go2RtcService: cannot write config at {}", m_configPath);
        return false;
    }

    file << "api:\n";
    file << "  listen: \"" << m_config.apiHost << ":" << m_config.apiPort << "\"\n";
    file << "rtsp:\n";
    file << "  listen: \":" << m_config.rtspPort << "\"\n";
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
        LOG_ERROR("Go2RtcService: failed to add stream '{}' (http {})", name, code);
        return false;
    }

    for (const auto& source : sources)
        LOG_INFO("Go2RtcService: stream '{}' -> {}", name, source);
    return true;
}

bool Go2RtcService::apiDeleteStream(const std::string& name)
{
    const std::string query = "/api/streams?src=" + url_encode(name);
    const int code = http_request(m_config.apiHost, m_config.apiPort, "DELETE", query);
    return code == 200;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
