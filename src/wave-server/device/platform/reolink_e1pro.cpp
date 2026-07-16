#include "reolink_e1pro.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../core/logger.h"
#include "network/net_util.h"
#include "../../service/go2rtc_service.h"

extern char** environ;

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    constexpr std::string_view kClass = "reolink_e1_pro";
    constexpr std::string_view kEncPrefix = "enc:";
    constexpr std::string_view kCipherKey = "wave-home";

    // Codec used by go2rtc when transcoding audio for the camera speaker.
    // Reolink backchannel advertises PCMU/8000 only (see go2rtc stream probe).
    constexpr const char* kTalkCodec = "pcmu";

    // --- secret obfuscation (stub cipher) -----------------------------------

    std::string xor_cipher(std::string_view data)
    {
        std::string out;
        out.resize(data.size());
        for (size_t i = 0; i < data.size(); ++i)
            out[i] = static_cast<char>(data[i] ^ kCipherKey[i % kCipherKey.size()]);
        return out;
    }

    std::string to_hex(std::string_view data)
    {
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(data.size() * 2);
        for (unsigned char c : data)
        {
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
        return out;
    }

    bool from_hex(std::string_view hex, std::string& out)
    {
        if (hex.size() % 2 != 0)
            return false;

        const auto nibble = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        out.clear();
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            const int hi = nibble(hex[i]);
            const int lo = nibble(hex[i + 1]);
            if (hi < 0 || lo < 0)
                return false;
            out.push_back(static_cast<char>((hi << 4) | lo));
        }
        return true;
    }

    std::string decrypt_secret(std::string_view stored)
    {
        if (stored.substr(0, kEncPrefix.size()) != kEncPrefix)
            return std::string(stored);

        std::string bytes;
        if (!from_hex(stored.substr(kEncPrefix.size()), bytes))
            throw std::runtime_error("invalid encrypted secret");
        return xor_cipher(bytes);
    }

    // --- SHA1 + Base64 (for ONVIF WS-Security PasswordDigest) ----------------

    std::string sha1_raw(std::string_view data)
    {
        uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

        std::string msg(data);
        const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8;
        msg.push_back(static_cast<char>(0x80));
        while (msg.size() % 64 != 56)
            msg.push_back('\0');
        for (int i = 7; i >= 0; --i)
            msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xff));

        const auto rol = [](uint32_t v, int b) { return (v << b) | (v >> (32 - b)); };

        for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
        {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
            {
                const auto* p = reinterpret_cast<const uint8_t*>(msg.data() + chunk + i * 4);
                w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            }
            for (int i = 16; i < 80; ++i)
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

            uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f, k;
                if (i < 20)      { f = (b & c) | ((~b) & d);      k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d;                 k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else             { f = b ^ c ^ d;                 k = 0xCA62C1D6; }

                const uint32_t tmp = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = tmp;
            }
            h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
        }

        std::string out(20, '\0');
        const uint32_t hs[5] = {h0, h1, h2, h3, h4};
        for (int i = 0; i < 5; ++i)
        {
            out[i * 4 + 0] = static_cast<char>((hs[i] >> 24) & 0xff);
            out[i * 4 + 1] = static_cast<char>((hs[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<char>((hs[i] >> 8) & 0xff);
            out[i * 4 + 3] = static_cast<char>(hs[i] & 0xff);
        }
        return out;
    }

    std::string base64_encode(std::string_view in)
    {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3)
        {
            const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
            out.push_back(tbl[(n >> 18) & 63]);
            out.push_back(tbl[(n >> 12) & 63]);
            out.push_back(tbl[(n >> 6) & 63]);
            out.push_back(tbl[n & 63]);
        }
        if (i < in.size())
        {
            uint32_t n = uint8_t(in[i]) << 16;
            const bool two = (i + 1 < in.size());
            if (two)
                n |= uint8_t(in[i + 1]) << 8;
            out.push_back(tbl[(n >> 18) & 63]);
            out.push_back(tbl[(n >> 12) & 63]);
            out.push_back(two ? tbl[(n >> 6) & 63] : '=');
            out.push_back('=');
        }
        return out;
    }

    // --- ONVIF SOAP over HTTP ------------------------------------------------

    std::string iso8601_utc_now()
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm {};
        gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }

    std::string random_nonce(size_t n)
    {
        std::string out(n, '\0');
        std::random_device rd;
        for (size_t i = 0; i < n; ++i)
            out[i] = static_cast<char>(rd() & 0xff);
        return out;
    }

    std::string ws_security(const std::string& user, const std::string& password)
    {
        const std::string nonce = random_nonce(16);
        const std::string created = iso8601_utc_now();
        const std::string digest = base64_encode(sha1_raw(nonce + created + password));
        const std::string nonce64 = base64_encode(nonce);

        return
            "<wsse:Security s:mustUnderstand=\"1\" "
            "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
            "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
            "<wsse:UsernameToken><wsse:Username>" + user + "</wsse:Username>"
            "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
            + digest + "</wsse:Password>"
            "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"
            + nonce64 + "</wsse:Nonce>"
            "<wsu:Created>" + created + "</wsu:Created>"
            "</wsse:UsernameToken></wsse:Security>";
    }

    // Blocking HTTP POST/GET with timeouts; returns status code (-1 on failure)
    // and captures the response body.
    int http_call(
        const std::string& host,
        uint16_t port,
        const std::string& method,
        const std::string& path,
        const std::string& content_type,
        const std::string& body,
        std::string& out_body)
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

        timeval tv {};
        tv.tv_sec = 3;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
                break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(result);

        if (fd < 0)
            return -1;

        std::string req;
        req += method + " " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + ":" + portStr + "\r\n";
        req += "Connection: close\r\n";
        if (!content_type.empty())
            req += "Content-Type: " + content_type + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "\r\n";
        req += body;

        size_t sent = 0;
        while (sent < req.size())
        {
            const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
            if (n <= 0)
            {
                ::close(fd);
                return -1;
            }
            sent += static_cast<size_t>(n);
        }

        std::string resp;
        char buf[8192];
        while (true)
        {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            resp.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);

        if (resp.empty())
            return -1;

        int major = 0, minor = 0, code = 0;
        if (std::sscanf(resp.c_str(), "HTTP/%d.%d %d", &major, &minor, &code) < 3)
            return -1;

        const size_t sep = resp.find("\r\n\r\n");
        out_body = (sep != std::string::npos) ? resp.substr(sep + 4) : std::string();
        return code;
    }

    ReolinkE1Pro::Config parseConfig(const json& config)
    {
        const auto& iface = config.at("interface");

        ReolinkE1Pro::Config out;
        out.host = iface.at("host").get<std::string>();
        out.onvifPort = iface.value("onvif_port", 8000);
        out.rtspPort = iface.value("rtsp_port", 554);
        out.user = decrypt_secret(iface.value("user", ""));
        out.password = decrypt_secret(iface.value("password", ""));
        out.channel = iface.value("channel", 0);
        out.go2rtc = iface.value("go2rtc", false);
        out.go2rtcSource = iface.value("go2rtc_source", "");
        return out;
    }

    void validateConfig(const json& config)
    {
        if (config.at("class").get<std::string>() != kClass)
            throw std::invalid_argument("reolink_e1_pro config field 'class' must be 'reolink_e1_pro'");

        if (!config.contains("interface") || !config["interface"].is_object())
            throw std::invalid_argument("reolink_e1_pro requires object field 'interface'");

        const auto& iface = config["interface"];
        if (!iface.contains("host") || !iface["host"].is_string() || iface["host"].get<std::string>().empty())
            throw std::invalid_argument("reolink_e1_pro interface requires non-empty string 'host'");
    }

    // Verifies the host still maps to the MAC pinned in the config. Returns -7
    // on mismatch, 0 when it matches or when no MAC is pinned / not resolvable.
    int verifyMac(const json& config, const std::string& host)
    {
        const std::string expected = config.at("interface").value("mac", "");
        if (expected.empty())
            return 0;

        std::string actual;
        if (!net::resolveMacForIp(host, actual))
        {
            LOG_WARN("reolink_e1_pro: could not resolve MAC for {} (skipping check)", host);
            return 0;
        }
        if (!net::macEquals(expected, actual))
        {
            LOG_ERROR("reolink_e1_pro: MAC mismatch for {} (expected {}, got {})", host, expected, actual);
            return -7;
        }
        LOG_INFO("reolink_e1_pro: MAC verified for {} ({})", host, actual);
        return 0;
    }

    std::string rtsp_url(const ReolinkE1Pro::Config& config, bool main_stream)
    {
        char buffer[256];
        std::snprintf(
            buffer, sizeof(buffer),
            "rtsp://%s:%s@%s:%u/h264Preview_%02u_%s",
            config.user.c_str(),
            config.password.c_str(),
            config.host.c_str(),
            static_cast<unsigned>(config.rtspPort),
            static_cast<unsigned>(config.channel + 1),
            main_stream ? "main" : "sub");
        return buffer;
    }

    // go2rtc RTSP ingest for video/audio listen-in. Disable the RTSP
    // backchannel so talk-back is routed through the ONVIF source instead.
    std::string go2rtc_rtsp_url(const ReolinkE1Pro::Config& config, bool main_stream)
    {
        return rtsp_url(config, main_stream) + "#backchannel=0";
    }

    // ONVIF source for go2rtc; provides the two-way audio backchannel that a
    // plain RTSP source lacks.
    std::string onvif_url(const ReolinkE1Pro::Config& config)
    {
        char buffer[256];
        std::snprintf(
            buffer, sizeof(buffer),
            "onvif://%s:%s@%s:%u/",
            config.user.c_str(),
            config.password.c_str(),
            config.host.c_str(),
            static_cast<unsigned>(config.onvifPort));
        return buffer;
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

    // Runs a program (searched on PATH) to completion; returns its exit code
    // or a negative value on spawn failure.
    int run_process(const std::vector<std::string>& args, const std::string& log_path)
    {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);

        pid_t pid = -1;
        const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (rc != 0)
            return -rc;

        int status = 0;
        ::waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    float db_to_level(float db)
    {
        // Map roughly [-60, 0] dBFS to [0, 1].
        const float clamped = std::max(-60.0f, std::min(0.0f, db));
        return (clamped + 60.0f) / 60.0f;
    }

    float parse_mean_volume_db(const std::string& log_path)
    {
        std::ifstream in(log_path);
        if (!in)
            return -60.0f;

        std::string line;
        while (std::getline(in, line))
        {
            const auto pos = line.find("mean_volume:");
            if (pos == std::string::npos)
                continue;
            float db = -60.0f;
            if (std::sscanf(line.c_str() + pos, "mean_volume: %f dB", &db) == 1)
                return db;
        }
        return -60.0f;
    }
}

struct ReolinkE1Pro::Impl
{
    Config config;
    std::string streamName;
    std::string talkStreamName;
    bool go2rtcActive = false;
    bool go2rtcTalkActive = false;
    std::vector<std::string> go2rtcSources;
    std::string go2rtcTalkSource;
    std::string ptzProfileToken;
    mutable std::mutex ptzMutex;

    std::string onvifCall(const std::string& service_path, const std::string& inner)
    {
        const std::string env =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
            "<s:Header>" + ws_security(config.user, config.password) + "</s:Header>"
            "<s:Body>" + inner + "</s:Body></s:Envelope>";

        std::string body;
        const int code = http_call(
            config.host, config.onvifPort, "POST", service_path,
            "application/soap+xml; charset=utf-8", env, body);

        if (code != 200)
            LOG_WARN("ONVIF {} -> http {}", service_path, code);
        return body;
    }

    std::string resolveProfileToken()
    {
        {
            std::lock_guard<std::mutex> lock(ptzMutex);
            if (!ptzProfileToken.empty())
                return ptzProfileToken;
        }

        const std::string body = onvifCall(
            "/onvif/media_service",
            "<GetProfiles xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>");

        std::string token;
        const size_t tok = body.find("token=\"");
        if (tok != std::string::npos)
        {
            const size_t start = tok + 7;
            const size_t end = body.find('"', start);
            if (end != std::string::npos)
                token = body.substr(start, end - start);
        }
        if (token.empty())
            token = "000";

        std::lock_guard<std::mutex> lock(ptzMutex);
        if (ptzProfileToken.empty())
            ptzProfileToken = token;
        return ptzProfileToken;
    }
};

// ============================================================================
// ReolinkE1Pro
// ============================================================================

ReolinkE1Pro::ReolinkE1Pro() :
    Device(),
    m_impl(std::make_unique<Impl>())
{
    registerActionsAndQueries();
}

ReolinkE1Pro::~ReolinkE1Pro()
{
    shutdown();
}

const ReolinkE1Pro::Config& ReolinkE1Pro::getConfig() const
{
    return m_config;
}

std::string ReolinkE1Pro::encryptSecret(std::string_view plain)
{
    return std::string(kEncPrefix) + to_hex(xor_cipher(plain));
}

// ============================================================================
// Device
// ============================================================================

int ReolinkE1Pro::init(const json& config)
{
    validateConfig(config);
    loadBaseConfig(config);
    m_config = parseConfig(config);

    if (!isEnabled())
        return -2;

    if (m_state == DeviceState::Running)
        return 0;

    if (m_state != DeviceState::Uninitialized && m_state != DeviceState::Stopped)
        return -3;

    m_state = DeviceState::Initializing;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_impl->config = m_config;

    if (m_config.go2rtc)
    {
        m_impl->streamName = deviceIDToString(getId());
        m_impl->talkStreamName = m_impl->streamName + "_talk";
        if (!m_config.go2rtcSource.empty())
        {
            m_impl->go2rtcSources.push_back(m_config.go2rtcSource);
        }
        else
        {
            m_impl->go2rtcSources.push_back(go2rtc_rtsp_url(m_config, true));
            m_impl->go2rtcTalkSource = onvif_url(m_config);
        }
    }

    const int macRc = verifyMac(config, m_config.host);
    if (macRc != 0)
    {
        m_state = DeviceState::Stopped;
        return macRc;
    }

    m_state = DeviceState::Running;
    LOG_INFO("ReolinkE1Pro initialized: {}", m_config.host);

    std::thread([impl = m_impl.get()]()
    {
        (void)impl->resolveProfileToken();
    }).detach();

    return 0;
}

void ReolinkE1Pro::shutdown()
{
    if (m_state == DeviceState::Uninitialized)
        return;

    m_state = DeviceState::ShuttingDown;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl->go2rtcTalkActive)
    {
        service::Go2RtcService::get().releaseStream(m_impl->talkStreamName);
        m_impl->go2rtcTalkActive = false;
    }
    if (m_impl->go2rtcActive)
    {
        service::Go2RtcService::get().releaseStream(m_impl->streamName);
        m_impl->go2rtcActive = false;
    }

    m_state = DeviceState::Stopped;
}

std::string_view ReolinkE1Pro::getClass() const
{
    return kClass;
}

bool ReolinkE1Pro::ensureGo2rtcStream()
{
    bool video_active = false;
    bool need_talk = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_config.go2rtc || m_impl->go2rtcSources.empty())
            return false;
        video_active = m_impl->go2rtcActive;
        need_talk = !m_impl->go2rtcTalkSource.empty();
    }

    if (!video_active)
    {
        if (!isHostReachable())
            return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl->go2rtcActive)
        {
            video_active = true;
        }
        else if (!service::Go2RtcService::get().acquireStream(m_impl->streamName, m_impl->go2rtcSources))
        {
            LOG_ERROR("ReolinkE1Pro: go2rtc stream registration failed");
            return false;
        }
        else
        {
            m_impl->go2rtcActive = true;
            video_active = true;
            LOG_INFO("ReolinkE1Pro: go2rtc stream '{}' active", m_impl->streamName);
        }
    }

    if (need_talk)
        (void)ensureGo2rtcTalkStream();

    return video_active;
}

bool ReolinkE1Pro::ensureGo2rtcTalkStream()
{
    std::string talk_name;
    std::string talk_source;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_config.go2rtc || m_impl->go2rtcTalkSource.empty())
            return false;
        if (m_impl->go2rtcTalkActive)
            return true;
        talk_name = m_impl->talkStreamName;
        talk_source = m_impl->go2rtcTalkSource;
    }

    if (!isHostReachable())
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl->go2rtcTalkActive)
        return true;
    if (!service::Go2RtcService::get().acquireStream(talk_name, talk_source))
    {
        LOG_ERROR("ReolinkE1Pro: go2rtc talk stream registration failed");
        return false;
    }
    m_impl->go2rtcTalkActive = true;
    LOG_INFO("ReolinkE1Pro: go2rtc talk stream '{}' active", talk_name);
    return true;
}

void ReolinkE1Pro::releaseGo2rtcStream()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl->go2rtcTalkActive)
    {
        service::Go2RtcService::get().releaseStream(m_impl->talkStreamName);
        m_impl->go2rtcTalkActive = false;
        LOG_INFO("ReolinkE1Pro: go2rtc talk stream '{}' released", m_impl->talkStreamName);
    }
    if (!m_impl->go2rtcActive)
        return;
    service::Go2RtcService::get().releaseStream(m_impl->streamName);
    m_impl->go2rtcActive = false;
    LOG_INFO("ReolinkE1Pro: go2rtc stream '{}' released", m_impl->streamName);
}

bool ReolinkE1Pro::isGo2rtcStreamActive() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_impl->go2rtcActive;
}

std::string_view ReolinkE1Pro::getGo2rtcStreamName() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_impl->streamName;
}

// ============================================================================
// Queryable
// ============================================================================

json ReolinkE1Pro::query(std::string_view name, const json& params)
{
    (void)params;

    if (name == "stream")
    {
        json out = json::object();
        out["main"] = rtsp_url(m_config, true);
        out["sub"] = rtsp_url(m_config, false);
        if (m_impl->go2rtcActive)
            out["go2rtc"] = service::Go2RtcService::get().streamRtspUrl(m_impl->streamName);
        return out;
    }

    if (name == "status")
    {
        json out = json::object();
        out["streaming"] = m_impl->go2rtcActive;
        out["micLevel"] = probeMicLevel();
        return out;
    }

    json err = json::object();
    err["code"] = -8;
    return err;
}

float ReolinkE1Pro::probeMicLevel()
{
    if (!m_impl->go2rtcActive)
        return 0.0f;

    const auto now = std::chrono::steady_clock::now();
    float cached = 0.0f;
    bool should_probe = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cached = m_cachedMicLevel;
        if (m_lastMicProbe == std::chrono::steady_clock::time_point{}
            || now - m_lastMicProbe >= std::chrono::seconds(3))
        {
            should_probe = !m_micProbeInFlight.exchange(true);
        }
    }

    if (should_probe)
    {
        const std::string source = service::Go2RtcService::get().streamRtspUrl(m_impl->streamName);
        std::thread([this, source]()
        {
            const std::string log = (std::filesystem::temp_directory_path() / "wave_mic_probe.log").string();
            const std::vector<std::string> args = {
                resolve_ffmpeg(), "-nostdin", "-hide_banner", "-loglevel", "info",
                "-rtsp_transport", "tcp",
                "-i", source,
                "-t", "0.15",
                "-vn", "-af", "volumedetect",
                "-f", "null", "-",
            };
            (void)run_process(args, log);
            const float level = db_to_level(parse_mean_volume_db(log));

            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastMicProbe = std::chrono::steady_clock::now();
            m_cachedMicLevel = level;
            m_micProbeInFlight.store(false);
        }).detach();
    }

    return cached;
}

std::future<json> ReolinkE1Pro::queryAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return query(name, params);
    });
}

// ============================================================================
// Actionable
// ============================================================================

int ReolinkE1Pro::invoke(std::string_view name, const json& params)
{
    (void)name;
    (void)params;
    return -8;
}

std::future<int> ReolinkE1Pro::invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return invoke(name, params);
    });
}

// ============================================================================
// IImageProvider
// ============================================================================

bool ReolinkE1Pro::captureFrame(CameraFrame& outFrame)
{
    if (!ensureGo2rtcStream())
    {
        LOG_ERROR("ReolinkE1Pro: captureFrame requires go2rtc to be enabled");
        return false;
    }

    std::string stream_name;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stream_name = m_impl->streamName;
    }

    // The first frame after the stream is registered may be empty until
    // go2rtc has pulled a keyframe from the camera, so retry briefly.
    std::vector<uint8_t> jpeg;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt)
    {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
        ok = service::Go2RtcService::get().fetchSnapshot(stream_name, jpeg);
    }
    if (!ok)
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

std::future<void> ReolinkE1Pro::captureFrameAsync(CameraFrame& outFrame)
{
    return std::async(std::launch::async, [this, &outFrame]()
    {
        (void)captureFrame(outFrame);
    });
}

// ============================================================================
// IVideoStreamProvider
// ============================================================================

bool ReolinkE1Pro::enumerateStreamProfiles(std::vector<CameraStreamProfile>& outProfiles)
{
    outProfiles.clear();

    CameraStreamProfile main;
    main.name = "main";
    main.uri = rtsp_url(m_config, true);
    main.codec = "h264";
    outProfiles.push_back(std::move(main));

    CameraStreamProfile sub;
    sub.name = "sub";
    sub.uri = rtsp_url(m_config, false);
    sub.codec = "h264";
    outProfiles.push_back(std::move(sub));
    return true;
}

bool ReolinkE1Pro::getStreamUri(std::string_view profile, std::string& outUri)
{
    const bool wantMain = (profile.empty() || profile == "main");
    outUri = rtsp_url(m_config, wantMain);
    return true;
}

// ============================================================================
// IPtzController (ONVIF)
// ============================================================================

PtzCapabilities ReolinkE1Pro::getPtzCapabilities() const
{
    PtzCapabilities caps;
    caps.pan = true;
    caps.tilt = true;
    caps.zoom = false;
    caps.absolute = false;
    caps.presets = true;
    caps.home = false; // E1 Pro exposes no ONVIF home position; use presets
    caps.maxPresets = 64;
    return caps;
}

bool ReolinkE1Pro::movePtz(const PtzVector& velocity, uint32_t durationMs)
{
    const std::string token = m_impl->resolveProfileToken();

    char inner[512];
    std::snprintf(
        inner, sizeof(inner),
        "<ContinuousMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "<Velocity>"
        "<PanTilt x=\"%.3f\" y=\"%.3f\" xmlns=\"http://www.onvif.org/ver10/schema\"/>"
        "</Velocity></ContinuousMove>",
        token.c_str(), velocity.pan, velocity.tilt);

    const std::string body = m_impl->onvifCall("/onvif/ptz_service", inner);
    if (body.find("ContinuousMoveResponse") == std::string::npos)
        return false;

    if (durationMs > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));
        stopPtz();
    }
    return true;
}

bool ReolinkE1Pro::stopPtz()
{
    const std::string token = m_impl->resolveProfileToken();

    char inner[256];
    std::snprintf(
        inner, sizeof(inner),
        "<Stop xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken><PanTilt>true</PanTilt><Zoom>true</Zoom></Stop>",
        token.c_str());

    const std::string body = m_impl->onvifCall("/onvif/ptz_service", inner);
    return body.find("StopResponse") != std::string::npos;
}

bool ReolinkE1Pro::movePtzTo(const PtzVector& position)
{
    (void)position;
    return false; // absolute positioning not supported
}

bool ReolinkE1Pro::enumeratePtzPresets(std::vector<PtzPreset>& outPresets)
{
    const std::string token = m_impl->resolveProfileToken();

    char inner[256];
    std::snprintf(
        inner, sizeof(inner),
        "<GetPresets xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>%s</ProfileToken></GetPresets>",
        token.c_str());

    const std::string body = m_impl->onvifCall("/onvif/ptz_service", inner);
    outPresets.clear();

    size_t pos = 0;
    uint32_t seq = 0;
    while ((pos = body.find("token=\"", pos)) != std::string::npos)
    {
        const size_t start = pos + 7;
        const size_t end = body.find('"', start);
        if (end == std::string::npos)
            break;
        const std::string tokenStr = body.substr(start, end - start);
        pos = end + 1;

        PtzPreset preset;
        try { preset.id = static_cast<uint32_t>(std::stoul(tokenStr)); }
        catch (...) { preset.id = seq; }

        // Name follows in the next <...:Name>...</...:Name>.
        const size_t nameStart = body.find(":Name>", end);
        if (nameStart != std::string::npos)
        {
            const size_t vs = nameStart + 6;
            const size_t ve = body.find("</", vs);
            if (ve != std::string::npos)
                preset.name = body.substr(vs, ve - vs);
        }
        outPresets.push_back(std::move(preset));
        ++seq;
    }
    return true;
}

bool ReolinkE1Pro::gotoPtzPreset(uint32_t presetId)
{
    const std::string token = m_impl->resolveProfileToken();

    char inner[320];
    std::snprintf(
        inner, sizeof(inner),
        "<GotoPreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken><PresetToken>%u</PresetToken></GotoPreset>",
        token.c_str(), presetId);

    const std::string body = m_impl->onvifCall("/onvif/ptz_service", inner);
    return body.find("GotoPresetResponse") != std::string::npos;
}

bool ReolinkE1Pro::savePtzPreset(uint32_t presetId, std::string_view name)
{
    const std::string token = m_impl->resolveProfileToken();

    char inner[384];
    std::snprintf(
        inner, sizeof(inner),
        "<SetPreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken><PresetName>%.*s</PresetName><PresetToken>%u</PresetToken></SetPreset>",
        token.c_str(), static_cast<int>(name.size()), name.data(), presetId);

    const std::string body = m_impl->onvifCall("/onvif/ptz_service", inner);
    return body.find("SetPresetResponse") != std::string::npos;
}

bool ReolinkE1Pro::movePtzHome()
{
    const std::string token = m_impl->resolveProfileToken();

    char inner[256];
    std::snprintf(
        inner, sizeof(inner),
        "<GotoHomePosition xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>%s</ProfileToken></GotoHomePosition>",
        token.c_str());

    const std::string body = m_impl->onvifCall("/onvif/ptz_service", inner);
    return body.find("GotoHomePositionResponse") != std::string::npos;
}

std::future<bool> ReolinkE1Pro::movePtzAsync(const PtzVector& velocity, uint32_t durationMs)
{
    return std::async(std::launch::async, [this, velocity, durationMs]()
    {
        return movePtz(velocity, durationMs);
    });
}

std::future<bool> ReolinkE1Pro::gotoPtzPresetAsync(uint32_t presetId)
{
    return std::async(std::launch::async, [this, presetId]()
    {
        return gotoPtzPreset(presetId);
    });
}

// ============================================================================
// IAudioInput (microphone)
// ============================================================================

AudioFormat ReolinkE1Pro::getSourceFormat() const
{
    AudioFormat fmt;
    fmt.sampleRate = 16000;
    fmt.sampleSize = 16;
    fmt.channels = 1;
    return fmt;
}

void ReolinkE1Pro::setAudioQueueSize(size_t size)
{
    (void)size;
}

size_t ReolinkE1Pro::getAudioQueueSize() const
{
    return 0;
}

bool ReolinkE1Pro::getLatestFrame(AudioFrame& outFrame)
{
    (void)outFrame;
    return false; // live frame streaming not implemented; use recordAudioToFile
}

std::future<void> ReolinkE1Pro::getLatestFrameAsync(AudioFrame& outFrame)
{
    return std::async(std::launch::async, [this, &outFrame]()
    {
        (void)getLatestFrame(outFrame);
    });
}

bool ReolinkE1Pro::popFrame(AudioFrame& outFrame)
{
    (void)outFrame;
    return false; // live frame streaming not implemented; use recordAudioToFile
}

// ============================================================================
// IAudioOutput (speaker)
// ============================================================================

AudioFormat ReolinkE1Pro::getSinkFormat() const
{
    AudioFormat fmt;
    fmt.sampleRate = 16000;
    fmt.sampleSize = 16;
    fmt.channels = 1;
    return fmt;
}

bool ReolinkE1Pro::playFrame(const AudioFrame& frame)
{
    (void)frame;
    return false; // frame streaming not implemented; use playAudioFile
}

std::future<bool> ReolinkE1Pro::playFrameAsync(const AudioFrame& frame)
{
    return std::async(std::launch::async, [this, &frame]()
    {
        return playFrame(frame);
    });
}

void ReolinkE1Pro::stopPlayback()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& go2rtc = service::Go2RtcService::get();
    if (m_impl->go2rtcTalkActive)
        go2rtc.streamToCamera(m_impl->talkStreamName, "");
    else if (m_impl->go2rtcActive)
        go2rtc.streamToCamera(m_impl->streamName, "");
}

// ============================================================================
// File-based helpers
// ============================================================================

bool ReolinkE1Pro::recordAudioToFile(const std::string& path, uint32_t seconds)
{
    const std::string source = rtsp_url(m_config, true);
    const std::string log = (std::filesystem::temp_directory_path() / "wave_ffmpeg_rec.log").string();

    const std::vector<std::string> args = {
        resolve_ffmpeg(), "-nostdin", "-y",
        "-rtsp_transport", "tcp",
        "-i", source,
        "-t", std::to_string(seconds),
        "-vn", "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le",
        path,
    };

    const int rc = run_process(args, log);
    if (rc != 0)
    {
        LOG_ERROR("ReolinkE1Pro: audio record failed (ffmpeg rc={}, see {})", rc, log);
        return false;
    }
    LOG_INFO("ReolinkE1Pro: recorded {}s of audio to {}", seconds, path);
    return true;
}

bool ReolinkE1Pro::playAudioFile(const std::string& path)
{
    if (!ensureGo2rtcStream())
    {
        LOG_ERROR("ReolinkE1Pro: playAudioFile requires go2rtc to be enabled");
        return false;
    }

    std::string abs;
    std::string talk_stream;
    std::string video_stream;
    bool use_talk_stream = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::error_code ec;
        abs = std::filesystem::absolute(path, ec).string();
        if (ec || abs.empty())
        {
            LOG_ERROR("ReolinkE1Pro: bad audio path '{}'", path);
            return false;
        }
        video_stream = m_impl->streamName;
        talk_stream = m_impl->talkStreamName;
        use_talk_stream = !m_impl->go2rtcTalkSource.empty();
    }

    if (!std::filesystem::exists(abs))
    {
        LOG_ERROR("ReolinkE1Pro: audio file not found '{}'", abs);
        return false;
    }

    if (use_talk_stream && !ensureGo2rtcTalkStream())
    {
        LOG_ERROR("ReolinkE1Pro: go2rtc talk stream unavailable");
        return false;
    }

    const std::string& dst = use_talk_stream ? talk_stream : video_stream;
    const std::string source =
        std::string("ffmpeg:") + abs + "#audio=" + kTalkCodec + "#input=file";

    auto& go2rtc = service::Go2RtcService::get();
    constexpr int k_max_attempts = 5;
    for (int attempt = 0; attempt < k_max_attempts; ++attempt)
    {
        if (go2rtc.streamToCamera(dst, source))
        {
            LOG_INFO("ReolinkE1Pro: playing TTS audio via go2rtc ({})", abs);
            return true;
        }
        if (attempt + 1 < k_max_attempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    LOG_ERROR("ReolinkE1Pro: go2rtc talk failed for '{}'", abs);
    return false;
}

// ============================================================================
// Registration
// ============================================================================

void ReolinkE1Pro::registerActionsAndQueries()
{
    m_queries = {
        {Query::Json, "stream", "RTSP stream URIs (main/sub, go2rtc)", json::object()},
        {Query::Json, "status", "Streaming and microphone level", json::object()},
    };

    m_actionMap.clear();
    m_queryMap.clear();
    for (auto& query : m_queries)
        m_queryMap[query.name] = &query;
}

bool ReolinkE1Pro::isHostReachable() const
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
    addr.sin_port = htons(m_config.onvifPort);
    if (::inet_pton(AF_INET, m_config.host.c_str(), &addr.sin_addr) != 1)
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

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
