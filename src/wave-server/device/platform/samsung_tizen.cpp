#include "samsung_tizen.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#ifdef __APPLE__
#include <Security/Security.h>
#endif

#include "../../core/logger.h"
#include "network/net_util.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    constexpr const char* kClass = "samsung_g7";
    constexpr const char* kWsPath = "/api/v2/channels/samsung.remote.control";

    json makeQueryError(int code, std::string_view message = {})
    {
        json out = json::object();
        out["code"] = code;
        if (!message.empty())
            out["message"] = std::string(message);
        return out;
    }

    std::string base64Encode(std::string_view input)
    {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, bmem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, input.data(), static_cast<int>(input.size()));
        BIO_flush(b64);
        char* data = nullptr;
        const long len = BIO_get_mem_data(bmem, &data);
        std::string out(data, static_cast<size_t>(len));
        BIO_free_all(b64);
        return out;
    }

    std::string sessionStateName(SamsungTizen::SessionState state)
    {
        switch (state)
        {
        case SamsungTizen::SessionState::Disconnected: return "disconnected";
        case SamsungTizen::SessionState::Connecting: return "connecting";
        case SamsungTizen::SessionState::Connected: return "connected";
        case SamsungTizen::SessionState::CoolingDown: return "cooling_down";
        case SamsungTizen::SessionState::WarmingUp: return "warming_up";
        }
        return "unknown";
    }

    const char* inputToRemoteKey(std::string_view source)
    {
        if (source == "hdmi1") return "KEY_HDMI1";
        if (source == "hdmi2") return "KEY_HDMI2";
        if (source == "hdmi3") return "KEY_HDMI3";
        if (source == "hdmi4") return "KEY_HDMI4";
        if (source == "displayport" || source == "dp") return "KEY_DISPLAYPORT";
        if (source == "source") return "KEY_SOURCE";
        if (source == "tv" || source == "dtv") return "KEY_DTV";
        return nullptr;
    }

    bool isIpv4Literal(const std::string& host)
    {
        in_addr addr {};
        return ::inet_pton(AF_INET, host.c_str(), &addr) == 1;
    }

    int tcpConnect(const std::string& host, uint16_t port, uint32_t timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        int fd = -1;

        if (isIpv4Literal(host))
        {
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
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

            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            {
                ::close(fd);
                return -1;
            }
        }
        else
        {
            addrinfo hints {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            addrinfo* result = nullptr;
            const std::string portStr = std::to_string(port);
            if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0 || !result)
                return -1;

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
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const uint32_t ioTimeoutMs = remaining > 0 ? static_cast<uint32_t>(remaining) : 1u;
        timeval tv {};
        tv.tv_sec = static_cast<time_t>(ioTimeoutMs / 1000);
        tv.tv_usec = static_cast<suseconds_t>((ioTimeoutMs % 1000) * 1000);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        return fd;
    }

#ifdef __APPLE__
    OSStatus secureTransportRead(SSLConnectionRef connection, void* data, size_t* dataLength)
    {
        const int fd = static_cast<int>(reinterpret_cast<intptr_t>(connection));
        const ssize_t n = ::read(fd, data, *dataLength);
        if (n > 0)
        {
            *dataLength = static_cast<size_t>(n);
            return noErr;
        }
        if (n == 0)
        {
            *dataLength = 0;
            return errSSLClosedGraceful;
        }
        *dataLength = 0;
        return errSSLClosedAbort;
    }

    OSStatus secureTransportWrite(SSLConnectionRef connection, const void* data, size_t* dataLength)
    {
        const int fd = static_cast<int>(reinterpret_cast<intptr_t>(connection));
        const ssize_t n = ::write(fd, data, *dataLength);
        if (n <= 0)
            return errSSLClosedAbort;
        *dataLength = static_cast<size_t>(n);
        return noErr;
    }
#endif

    int headerContentLength(std::string_view headers)
    {
        std::string lower(headers);
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const std::string needle = "content-length:";
        const size_t pos = lower.find(needle);
        if (pos == std::string::npos)
            return -1;

        size_t start = pos + needle.size();
        while (start < lower.size() && (lower[start] == ' ' || lower[start] == '\t'))
            ++start;

        int value = 0;
        bool found = false;
        while (start < lower.size() && std::isdigit(static_cast<unsigned char>(lower[start])))
        {
            found = true;
            value = value * 10 + (lower[start] - '0');
            ++start;
        }
        return found ? value : -1;
    }

    class TlsConnection
    {
    public:
        ~TlsConnection() { close(); }

        bool connect(const std::string& host, uint16_t port, uint32_t timeoutMs)
        {
            close();
            m_fd = tcpConnect(host, port, timeoutMs);
            if (m_fd < 0)
            {
                LOG_ERROR("samsung_g7: TCP connect failed for {}:{} (errno={})", host, port, errno);
                return false;
            }

#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            m_sslContext = SSLCreateContext(nullptr, kSSLClientSide, kSSLStreamType);
            if (!m_sslContext)
            {
                LOG_ERROR("samsung_g7: SSLCreateContext failed");
                close();
                return false;
            }

            SSLSetIOFuncs(m_sslContext, secureTransportRead, secureTransportWrite);
            SSLSetConnection(m_sslContext, reinterpret_cast<SSLConnectionRef>(static_cast<intptr_t>(m_fd)));
            SSLSetPeerDomainName(m_sslContext, host.c_str(), host.size());
            SSLSetAllowsAnyRoot(m_sslContext, true);
            SSLSetAllowsExpiredCerts(m_sslContext, true);
            SSLSetAllowsExpiredRoots(m_sslContext, true);
            SSLSetSessionOption(m_sslContext, kSSLSessionOptionBreakOnServerAuth, true);

            OSStatus status = SSLHandshake(m_sslContext);
            while (status == errSSLWouldBlock)
                status = SSLHandshake(m_sslContext);
            while (status == errSSLPeerAuthCompleted)
                status = SSLHandshake(m_sslContext);
            if (status != noErr)
            {
                LOG_ERROR("samsung_g7: TLS handshake failed for {} (status={})", host, static_cast<int>(status));
                close();
                return false;
            }
#pragma clang diagnostic pop
#else
            static std::once_flag sslInitFlag;
            std::call_once(sslInitFlag, []() { OPENSSL_init_ssl(0, nullptr); });

            m_ctx = SSL_CTX_new(TLS_client_method());
            if (!m_ctx)
            {
                close();
                return false;
            }
            SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);
            SSL_CTX_set_security_level(m_ctx, 0);

            m_ssl = SSL_new(m_ctx);
            if (!m_ssl)
            {
                close();
                return false;
            }
            SSL_set_tlsext_host_name(m_ssl, host.c_str());
            SSL_set_fd(m_ssl, m_fd);
            if (SSL_connect(m_ssl) != 1)
            {
                LOG_ERROR("samsung_g7: OpenSSL connect failed for {} (ssl_err={})", host, SSL_get_error(m_ssl, -1));
                ERR_print_errors_fp(stderr);
                close();
                return false;
            }
#endif
            return true;
        }

        bool writeAll(const void* data, size_t len)
        {
            const auto* p = static_cast<const uint8_t*>(data);
            size_t sent = 0;
            while (sent < len)
            {
#ifdef __APPLE__
                size_t chunk = len - sent;
                const OSStatus status = SSLWrite(m_sslContext, p + sent, chunk, &chunk);
                if (status != noErr || chunk == 0)
                    return false;
                sent += chunk;
#else
                const int n = SSL_write(m_ssl, p + sent, static_cast<int>(len - sent));
                if (n <= 0)
                    return false;
                sent += static_cast<size_t>(n);
#endif
            }
            return true;
        }

        bool readSome(std::string& buffer, uint32_t timeoutMs)
        {
            timeval tv {};
            tv.tv_sec = static_cast<time_t>(timeoutMs / 1000);
            tv.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);
            ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            char chunk[4096];
#ifdef __APPLE__
            size_t n = sizeof(chunk);
            const OSStatus status = SSLRead(m_sslContext, chunk, sizeof(chunk), &n);
            if (status != noErr && status != errSSLWouldBlock)
                return false;
            if (n == 0)
                return false;
            buffer.append(chunk, n);
            return true;
#else
            const int n = SSL_read(m_ssl, chunk, sizeof(chunk));
            if (n <= 0)
                return false;
            buffer.append(chunk, static_cast<size_t>(n));
            return true;
#endif
        }

        bool readHttpResponse(std::string& body, uint32_t timeoutMs)
        {
            std::string raw;
            bool headersDone = false;

            for (int attempt = 0; attempt < 128; ++attempt)
            {
                if (!readSome(raw, timeoutMs))
                {
                    if (headersDone)
                        break;
                    return !body.empty();
                }

                if (!headersDone)
                {
                    const size_t sep = raw.find("\r\n\r\n");
                    if (sep == std::string::npos)
                        continue;

                    const std::string headers = raw.substr(0, sep);
                    body = raw.substr(sep + 4);
                    headersDone = true;

                    const int contentLen = headerContentLength(headers);
                    if (contentLen >= 0)
                    {
                        while (static_cast<int>(body.size()) < contentLen)
                        {
                            if (!readSome(body, timeoutMs))
                                break;
                        }
                        return static_cast<int>(body.size()) >= contentLen;
                    }
                }
            }

            return headersDone && !body.empty();
        }

        void close()
        {
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            if (m_sslContext)
            {
                SSLClose(m_sslContext);
                CFRelease(m_sslContext);
                m_sslContext = nullptr;
            }
#pragma clang diagnostic pop
#else
            if (m_ssl)
            {
                SSL_shutdown(m_ssl);
                SSL_free(m_ssl);
                m_ssl = nullptr;
            }
            if (m_ctx)
            {
                SSL_CTX_free(m_ctx);
                m_ctx = nullptr;
            }
#endif
            if (m_fd >= 0)
            {
                ::close(m_fd);
                m_fd = -1;
            }
        }

    private:
#ifdef __APPLE__
        SSLContextRef m_sslContext = nullptr;
#else
        SSL_CTX* m_ctx = nullptr;
        SSL* m_ssl = nullptr;
#endif
        int m_fd = -1;
    };

    std::vector<uint8_t> buildMaskedTextFrame(const std::string& text)
    {
        std::vector<uint8_t> frame;
        frame.push_back(0x81);

        const size_t len = text.size();
        uint8_t mask[4];
        std::random_device rd;
        for (int i = 0; i < 4; ++i)
            mask[i] = static_cast<uint8_t>(rd() & 0xff);

        if (len < 126)
        {
            frame.push_back(static_cast<uint8_t>(0x80 | len));
        }
        else if (len < 65536)
        {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
            frame.push_back(static_cast<uint8_t>(len & 0xff));
        }
        else
        {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xff));
        }

        frame.insert(frame.end(), mask, mask + 4);
        for (size_t i = 0; i < len; ++i)
            frame.push_back(static_cast<uint8_t>(text[i] ^ mask[i % 4]));
        return frame;
    }

    bool readWsTextFrame(TlsConnection& tls, std::string& outText, uint32_t timeoutMs)
    {
        std::string buffer;
        while (buffer.size() < 2)
        {
            if (!tls.readSome(buffer, timeoutMs))
                return false;
        }

        const uint8_t opcode = static_cast<uint8_t>(buffer[0]) & 0x0f;
        if (opcode == 0x08)
            return false;

        uint64_t payloadLen = static_cast<uint8_t>(buffer[1]) & 0x7f;
        size_t offset = 2;
        if (payloadLen == 126)
        {
            while (buffer.size() < offset + 2)
                if (!tls.readSome(buffer, timeoutMs))
                    return false;
            payloadLen = (static_cast<uint8_t>(buffer[offset]) << 8)
                | static_cast<uint8_t>(buffer[offset + 1]);
            offset += 2;
        }
        else if (payloadLen == 127)
        {
            while (buffer.size() < offset + 8)
                if (!tls.readSome(buffer, timeoutMs))
                    return false;
            payloadLen = 0;
            for (int i = 0; i < 8; ++i)
                payloadLen = (payloadLen << 8) | static_cast<uint8_t>(buffer[offset + i]);
            offset += 8;
        }

        const bool masked = (static_cast<uint8_t>(buffer[1]) & 0x80) != 0;
        if (masked)
            offset += 4;

        while (buffer.size() < offset + payloadLen)
            if (!tls.readSome(buffer, timeoutMs))
                return false;

        outText.assign(buffer.data() + static_cast<size_t>(offset), static_cast<size_t>(payloadLen));
        return opcode == 0x01 || opcode == 0x02;
    }

    std::string buildWsUrl(const SamsungTizen::InterfaceConfig& cfg, bool includeToken)
    {
        std::string url = "GET " + std::string(kWsPath) + "?name=" + base64Encode(cfg.clientName);
        if (includeToken && !cfg.token.empty())
            url += "&token=" + cfg.token;
        url += " HTTP/1.1\r\n";
        url += "Host: " + cfg.host + ":" + std::to_string(cfg.port) + "\r\n";
        url += "Upgrade: websocket\r\n";
        url += "Connection: Upgrade\r\n";
        url += "Sec-WebSocket-Version: 13\r\n";

        uint8_t keyRaw[16];
        std::random_device rd;
        for (int i = 0; i < 16; ++i)
            keyRaw[i] = static_cast<uint8_t>(rd() & 0xff);
        const std::string wsKey = base64Encode(std::string(reinterpret_cast<char*>(keyRaw), 16));
        url += "Sec-WebSocket-Key: " + wsKey + "\r\n\r\n";
        return url;
    }

    bool httpsGetJson(const std::string& host, uint16_t port, const std::string& path,
        json& out, uint32_t timeoutMs)
    {
        TlsConnection tls;
        if (!tls.connect(host, port, timeoutMs))
            return false;

        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + ":" + std::to_string(port) + "\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n\r\n";

        if (!tls.writeAll(req.data(), req.size()))
        {
            LOG_ERROR("samsung_g7: REST request write failed for {}", host);
            return false;
        }

        std::string body;
        if (!tls.readHttpResponse(body, timeoutMs))
        {
            LOG_ERROR("samsung_g7: REST response read failed for {}", host);
            return false;
        }

        out = json::parse(body, nullptr, false);
        if (out.is_discarded())
        {
            LOG_ERROR("samsung_g7: REST JSON parse failed for {} ({} bytes)", host, body.size());
            return false;
        }
        return true;
    }

    SamsungTizen::InterfaceConfig parseInterface(const json& config)
    {
        const auto& iface = config.at("interface");
        SamsungTizen::InterfaceConfig out;
        out.host = iface.at("host").get<std::string>();
        out.mac = iface.value("mac", "");
        out.token = iface.value("token", "");
        out.port = static_cast<uint16_t>(iface.value("port", 8002));
        out.clientName = iface.value("client_name", "WaveHome");
        return out;
    }

    void validateSamsungConfig(const json& config)
    {
        if (config.at("class").get<std::string>() != kClass)
            throw std::invalid_argument("samsung_g7 config field 'class' must be 'samsung_g7'");

        if (!config.contains("interface") || !config["interface"].is_object())
            throw std::invalid_argument("samsung_g7 requires object field 'interface'");

        const auto& iface = config["interface"];
        if (!iface.contains("host") || !iface["host"].is_string() || iface["host"].get<std::string>().empty())
            throw std::invalid_argument("samsung_g7 interface requires non-empty string 'host'");
    }

    SamsungTizen::Capabilities defaultCapabilities()
    {
        SamsungTizen::Capabilities caps;
        caps.power = true;
        caps.volume = true;
        caps.mute = true;
        caps.navigation = true;
        caps.channel = false;
        caps.apps = false;
        caps.inputs = true;
        caps.inputSources = {"hdmi1", "hdmi2", "displayport"};
        return caps;
    }

    void applyRestInfo(SamsungTizen::Capabilities& caps, const json& info)
    {
        if (!info.contains("device") || !info["device"].is_object())
            return;

        const auto& dev = info["device"];
        if (dev.contains("modelName"))
            caps.modelName = dev["modelName"].get<std::string>();
        if (dev.contains("firmwareVersion"))
            caps.firmwareVersion = dev["firmwareVersion"].get<std::string>();
        if (dev.contains("type"))
            caps.deviceType = dev["type"].get<std::string>();

        const std::string model = caps.modelName;
        if (model.find("TV") != std::string::npos || model.find("tv") != std::string::npos)
        {
            caps.channel = true;
            caps.apps = true;
            if (caps.inputSources.size() < 4)
                caps.inputSources = {"hdmi1", "hdmi2", "hdmi3", "hdmi4", "displayport"};
        }
    }

    int verifyMac(const json& config, const json& restInfo)
    {
        const std::string expected = config.at("interface").value("mac", "");
        if (expected.empty() || !restInfo.contains("device"))
            return 0;

        const auto& dev = restInfo["device"];
        std::string actual;
        if (dev.contains("wifiMac"))
            actual = dev["wifiMac"].get<std::string>();
        else if (dev.contains("ssid"))
            actual = dev["ssid"].get<std::string>();

        if (actual.empty())
            return 0;
        if (!net::macEquals(expected, actual))
        {
            LOG_ERROR("samsung_g7: MAC mismatch (expected {}, got {})", expected, actual);
            return -7;
        }
        return 0;
    }

    class WsSession
    {
    public:
        bool open(const SamsungTizen::InterfaceConfig& cfg, bool includeToken, uint32_t timeoutMs)
        {
            close();
            if (!m_tls.connect(cfg.host, cfg.port, timeoutMs))
                return false;

            const std::string handshake = buildWsUrl(cfg, includeToken);
            if (!m_tls.writeAll(handshake.data(), handshake.size()))
            {
                close();
                return false;
            }

            std::string response;
            if (!m_tls.readHttpResponse(response, timeoutMs) || response.find("101") == std::string::npos)
            {
                close();
                return false;
            }

            const bool hasSavedToken = includeToken && !cfg.token.empty();
            const uint32_t eventWaitMs = hasSavedToken ? 2000u : timeoutMs;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(eventWaitMs);

            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto remaining = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count());
                if (remaining == 0)
                    break;

                std::string frame;
                if (!readWsTextFrame(m_tls, frame, std::min(remaining, 500u)))
                    continue;

                json msg = json::parse(frame, nullptr, false);
                if (msg.is_discarded())
                    continue;

                const std::string event = msg.value("event", "");
                if (event == "ms.channel.connect")
                {
                    m_connected = true;
                    if (msg.contains("data") && msg["data"].contains("token"))
                        m_token = msg["data"]["token"].get<std::string>();
                    return true;
                }
                if (event == "ms.channel.unauthorized")
                {
                    close();
                    return false;
                }
            }

            if (hasSavedToken)
            {
                m_connected = true;
                return true;
            }

            close();
            return false;
        }

        bool sendJson(const json& payload, uint32_t timeoutMs)
        {
            if (!m_connected)
                return false;
            const std::string text = payload.dump();
            const auto frame = buildMaskedTextFrame(text);
            if (!m_tls.writeAll(frame.data(), frame.size()))
            {
                close();
                return false;
            }
            (void)timeoutMs;
            return true;
        }

        bool isConnected() const { return m_connected; }
        const std::string& issuedToken() const { return m_token; }

        void close()
        {
            m_connected = false;
            m_token.clear();
            m_tls.close();
        }

    private:
        TlsConnection m_tls;
        bool m_connected = false;
        std::string m_token;
    };

    void sendWakeOnLan(const std::string& mac)
    {
        uint8_t addr[6];
        if (!net::parseMac(mac, addr))
            return;

        uint8_t packet[102];
        std::memset(packet, 0xff, 6);
        for (int i = 0; i < 16; ++i)
            std::memcpy(packet + 6 + i * 6, addr, 6);

        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return;

        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

        sockaddr_in dest {};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(9);
        dest.sin_addr.s_addr = INADDR_BROADCAST;
        ::sendto(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        ::close(fd);
    }
}

// ============================================================================
// SamsungTizenTokenClient
// ============================================================================

struct SamsungTizenTokenClient::Impl
{
    SamsungTizen::InterfaceConfig iface;
    uint32_t timeoutMs = 120000;
};

SamsungTizenTokenClient::SamsungTizenTokenClient() :
    m_impl(std::make_unique<Impl>())
{
}

SamsungTizenTokenClient::~SamsungTizenTokenClient() = default;

int SamsungTizenTokenClient::configure(const json& config)
{
    try
    {
        if (config.contains("interface"))
            m_impl->iface = parseInterface(config);
        else
        {
            m_impl->iface.host = config.at("host").get<std::string>();
            m_impl->iface.mac = config.value("mac", "");
            m_impl->iface.port = static_cast<uint16_t>(config.value("port", 8002));
            m_impl->iface.clientName = config.value("client_name", "WaveHome");
        }
        m_impl->timeoutMs = config.value("timeout_ms", 120000u);
        m_config.host = m_impl->iface.host;
        m_config.mac = m_impl->iface.mac;
        m_config.port = m_impl->iface.port;
        m_config.clientName = m_impl->iface.clientName;
        m_config.timeoutMs = m_impl->timeoutMs;
        return 0;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("SamsungTizenTokenClient configure failed: {}", ex.what());
        return -9;
    }
}

const SamsungTizenTokenClient::Config& SamsungTizenTokenClient::getConfig() const
{
    return m_config;
}

SamsungTizenTokenClient::Result SamsungTizenTokenClient::requestToken()
{
    Result result;
    try
    {
        json info;
        if (!httpsGetJson(m_impl->iface.host, m_impl->iface.port, "/api/v2/", info, m_impl->timeoutMs))
        {
            result.errorCode = -5;
            result.message = "REST probe failed";
            return result;
        }

        if (info.contains("device"))
        {
            const auto& dev = info["device"];
            result.deviceName = dev.value("name", "");
            result.modelName = dev.value("modelName", "");
        }

        WsSession ws;
        if (!ws.open(m_impl->iface, false, m_impl->timeoutMs))
        {
            result.errorCode = -5;
            result.message = "pairing connection failed (press Allow on the display)";
            return result;
        }

        result.token = ws.issuedToken();
        if (result.token.empty())
        {
            result.errorCode = -5;
            result.message = "no token in ms.channel.connect response";
            return result;
        }

        result.ok = true;
        return result;
    }
    catch (const std::exception& ex)
    {
        result.errorCode = -5;
        result.message = ex.what();
        return result;
    }
}

std::future<SamsungTizenTokenClient::Result> SamsungTizenTokenClient::requestTokenAsync()
{
    return std::async(std::launch::async, [this]() { return requestToken(); });
}

// ============================================================================
// SamsungTizen::Impl
// ============================================================================

struct SamsungTizen::Impl
{
    InterfaceConfig iface;
    SessionConfig session;
    WsSession ws;
};

// ============================================================================
// SamsungTizen
// ============================================================================

SamsungTizen::SamsungTizen() :
    Device(),
    m_impl(std::make_unique<Impl>())
{
    m_capabilities = defaultCapabilities();
    registerActionsAndQueries();
}

SamsungTizen::~SamsungTizen()
{
    shutdown();
}

const SamsungTizen::InterfaceConfig& SamsungTizen::getInterfaceConfig() const
{
    return m_interface;
}

const SamsungTizen::SessionConfig& SamsungTizen::getSessionConfig() const
{
    return m_session;
}

const SamsungTizen::Capabilities& SamsungTizen::getCapabilities() const
{
    return m_capabilities;
}

SamsungTizen::SessionState SamsungTizen::getSessionState() const
{
    return m_sessionState;
}

int SamsungTizen::init(const json& config)
{
    validateSamsungConfig(config);
    loadBaseConfig(config);

    m_interface = parseInterface(config);
    m_impl->iface = m_interface;
    m_capabilities = defaultCapabilities();

    if (!isEnabled())
        return -2;

    if (m_state == DeviceState::Running)
        return 0;

    if (m_state != DeviceState::Uninitialized && m_state != DeviceState::Stopped)
        return -3;

    m_state = DeviceState::Initializing;

    try
    {
        json info;
        if (!httpsGetJson(m_interface.host, m_interface.port, "/api/v2/", info, m_session.connectTimeoutMs))
        {
            LOG_ERROR("samsung_g7: REST probe failed for {}", m_interface.host);
            m_state = DeviceState::Stopped;
            return -5;
        }

        applyRestInfo(m_capabilities, info);
        if (info.contains("device"))
            m_cachedPowerState = info["device"].value("PowerState", "");
        m_restCacheTime = std::chrono::steady_clock::now();

        const int macRc = verifyMac(config, info);
        if (macRc != 0)
        {
            m_state = DeviceState::Stopped;
            return macRc;
        }

        if (m_interface.token.empty())
            LOG_WARN("samsung_g7: no token configured; run test-samsung-tizen --register");

        registerActionsAndQueries();
        m_state = DeviceState::Running;
        LOG_INFO("samsung_g7 ready: {} ({})", m_interface.host, m_capabilities.modelName);
        return 0;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("samsung_g7 init failed: {}", ex.what());
        m_state = DeviceState::Stopped;
        return -5;
    }
}

void SamsungTizen::shutdown()
{
    if (m_state == DeviceState::Uninitialized)
        return;

    m_state = DeviceState::ShuttingDown;

    std::lock_guard<std::mutex> lock(m_mutex);
    disconnectSession(false);

    m_state = DeviceState::Stopped;
}

int SamsungTizen::connectSession()
{
    if (m_sessionState == SessionState::Connected && m_impl->ws.isConnected())
        return 0;

    m_sessionState = SessionState::Connecting;

    if (!m_impl->ws.open(m_interface, true, m_session.connectTimeoutMs))
    {
        m_sessionState = SessionState::Disconnected;
        return -5;
    }

    m_sessionState = SessionState::Connected;
    return 0;
}

void SamsungTizen::disconnectSession(bool remoteHangup)
{
    (void)remoteHangup;
    m_impl->ws.close();
    m_sessionState = SessionState::Disconnected;
}

int SamsungTizen::sendRemoteKey(const std::string& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sessionState != SessionState::Connected && connectSession() != 0)
        return -5;

    const json payload = {
        {"method", "ms.remote.control"},
        {"params", {
            {"Cmd", "Click"},
            {"DataOfCmd", key},
            {"Option", "false"},
            {"TypeOfRemote", "SendRemoteKey"},
        }},
    };

    if (!m_impl->ws.sendJson(payload, m_session.commandTimeoutMs))
    {
        disconnectSession(true);
        return -5;
    }
    return 0;
}

int SamsungTizen::setInput(std::string_view source)
{
    const char* key = inputToRemoteKey(source);
    if (!key)
        return -9;

    const int rc = sendRemoteKey(key);
    if (rc == 0)
        m_lastInput = std::string(source);
    return rc;
}

int SamsungTizen::wakeDisplay()
{
    if (!m_interface.mac.empty())
        sendWakeOnLan(m_interface.mac);
    return 0;
}

int SamsungTizen::powerOn()
{
    if (m_sessionState == SessionState::Connected)
        return 0;

    wakeDisplay();
    m_sessionState = SessionState::WarmingUp;

    for (uint32_t i = 0; i < m_session.restPollAttempts; ++i)
    {
        json info;
        if (httpsGetJson(m_interface.host, m_interface.port, "/api/v2/", info, m_session.restPollIntervalMs))
        {
            if (connectSession() == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(m_session.warmingTimeMs));
                m_sessionState = SessionState::Connected;
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(m_session.restPollIntervalMs));
    }

    m_sessionState = SessionState::Disconnected;
    return -5;
}

int SamsungTizen::powerOff()
{
    const int rc = sendRemoteKey("KEY_POWER");
    disconnectSession(true);
    m_sessionState = SessionState::CoolingDown;
    return rc;
}

int SamsungTizen::powerToggle()
{
    json info;
    if (httpsGetJson(m_interface.host, m_interface.port, "/api/v2/", info, m_session.connectTimeoutMs))
    {
        const std::string power = info.contains("device")
            ? info["device"].value("PowerState", "")
            : "";
        if (power == "on" || m_sessionState == SessionState::Connected)
            return powerOff();
    }
    return powerOn();
}

json SamsungTizen::query(std::string_view name, const json& params)
{
    (void)params;

    if (name == "capabilities")
    {
        return {
            {"power", m_capabilities.power},
            {"volume", m_capabilities.volume},
            {"mute", m_capabilities.mute},
            {"navigation", m_capabilities.navigation},
            {"channel", m_capabilities.channel},
            {"apps", m_capabilities.apps},
            {"inputs", m_capabilities.inputs},
            {"input_sources", m_capabilities.inputSources},
            {"model", m_capabilities.modelName},
            {"firmware", m_capabilities.firmwareVersion},
            {"type", m_capabilities.deviceType},
        };
    }

    if (name == "session")
    {
        return {
            {"state", sessionStateName(m_sessionState)},
            {"connected", m_sessionState == SessionState::Connected},
        };
    }

    if (name == "inputs")
    {
        return {{"sources", m_capabilities.inputSources}};
    }

    if (name == "input")
    {
        return {
            {"source", m_lastInput},
            {"known", !m_lastInput.empty()},
        };
    }

    if (m_state != DeviceState::Running)
        return makeQueryError(-4);

    if (name == "state")
    {
        const auto now = std::chrono::steady_clock::now();
        const bool cacheFresh = m_cachedPowerState.empty() == false
            && std::chrono::duration_cast<std::chrono::seconds>(now - m_restCacheTime).count() < 5;

        std::string powerState = m_cachedPowerState.empty() ? "unknown" : m_cachedPowerState;
        if (!cacheFresh)
        {
            json info;
            if (httpsGetJson(m_interface.host, m_interface.port, "/api/v2/", info, m_session.connectTimeoutMs)
                && info.contains("device"))
            {
                powerState = info["device"].value("PowerState", "unknown");
                m_cachedPowerState = powerState;
                m_restCacheTime = now;
            }
        }

        return {
            {"on", powerState == "on"},
            {"power_state", powerState},
            {"connected", m_sessionState == SessionState::Connected},
            {"input", m_lastInput},
        };
    }

    return makeQueryError(-8);
}

std::future<json> SamsungTizen::queryAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return query(name, params);
    });
}

int SamsungTizen::invoke(std::string_view name, const json& params)
{
    if (m_state != DeviceState::Running)
        return -4;

    if (name == "on")
        return powerOn();
    if (name == "off")
        return powerOff();
    if (name == "toggle")
        return powerToggle();

    if (name == "mute")
        return sendRemoteKey("KEY_MUTE");
    if (name == "volume_up")
        return sendRemoteKey("KEY_VOLUP");
    if (name == "volume_down")
        return sendRemoteKey("KEY_VOLDOWN");
    if (name == "channel_up")
        return sendRemoteKey("KEY_CHUP");
    if (name == "channel_down")
        return sendRemoteKey("KEY_CHDOWN");

    if (name == "input")
    {
        if (!params.contains("source") || !params["source"].is_string())
            return -9;
        return setInput(params["source"].get<std::string>());
    }

    if (name == "send_key")
    {
        if (!params.contains("key") || !params["key"].is_string())
            return -9;
        return sendRemoteKey(params["key"].get<std::string>());
    }

    if (name == "open_app")
    {
        if (!params.contains("app") || !params["app"].is_string())
            return -9;

        const json payload = {
            {"method", "ms.channel.emit"},
            {"params", {
                {"event", "ed.apps.launch"},
                {"to", "host"},
                {"data", {
                    {"appId", params["app"].get<std::string>()},
                    {"action_type", "NATIVE_LAUNCH"},
                }},
            }},
        };

        if (m_sessionState != SessionState::Connected && connectSession() != 0)
            return -5;
        if (!m_impl->ws.sendJson(payload, m_session.commandTimeoutMs))
        {
            disconnectSession(true);
            return -5;
        }
        return 0;
    }
    if (name == "nav_up") return sendRemoteKey("KEY_UP");
    if (name == "nav_down") return sendRemoteKey("KEY_DOWN");
    if (name == "nav_left") return sendRemoteKey("KEY_LEFT");
    if (name == "nav_right") return sendRemoteKey("KEY_RIGHT");
    if (name == "select") return sendRemoteKey("KEY_ENTER");
    if (name == "home") return sendRemoteKey("KEY_HOME");
    if (name == "back") return sendRemoteKey("KEY_RETURN");

    return -8;
}

std::future<int> SamsungTizen::invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return invoke(name, params);
    });
}

void SamsungTizen::registerActionsAndQueries()
{
    m_actions = {
        {Action::Json, Action::Stateful, "on", "Power on", json::object()},
        {Action::Json, Action::Stateful, "off", "Power off", json::object()},
        {Action::Json, Action::Toggle | Action::Stateful, "toggle", "Toggle power", json::object()},
        {Action::Json, Action::Toggle | Action::Stateful, "mute", "Toggle mute", json::object()},
        {Action::Json, Action::Repeat | Action::Stateful, "volume_up", "Volume up", json::object()},
        {Action::Json, Action::Repeat | Action::Stateful, "volume_down", "Volume down", json::object()},
        {Action::Json, Action::Repeat, "nav_up", "D-pad up", json::object()},
        {Action::Json, Action::Repeat, "nav_down", "D-pad down", json::object()},
        {Action::Json, Action::Repeat, "nav_left", "D-pad left", json::object()},
        {Action::Json, Action::Repeat, "nav_right", "D-pad right", json::object()},
        {Action::Json, Action::Repeat, "select", "OK / enter", json::object()},
        {Action::Json, Action::Repeat, "home", "Home", json::object()},
        {Action::Json, Action::Repeat, "back", "Back", json::object()},
        {Action::Json, Action::Repeat, "send_key", "Send raw remote key", json::object()},
    };

    m_queries = {
        {Query::Json, "capabilities", "Display capabilities", json::object()},
        {Query::Json, "session", "WSS session state", json::object()},
        {Query::Json, "state", "Power and input state", json::object()},
        {Query::Json, "inputs", "Available input sources", json::object()},
        {Query::Json, "input", "Last selected input source", json::object()},
    };

    if (m_capabilities.channel)
    {
        m_actions.push_back({Action::Json, Action::Repeat | Action::Stateful, "channel_up", "Channel up", json::object()});
        m_actions.push_back({Action::Json, Action::Repeat | Action::Stateful, "channel_down", "Channel down", json::object()});
    }

    if (m_capabilities.inputs)
    {
        m_actions.push_back({Action::Json, Action::Stateful, "input", "Switch input (hdmi1, hdmi2, displayport, ...)", json::object()});
    }

    if (m_capabilities.apps)
    {
        m_actions.push_back({Action::Json, Action::Stateful, "open_app", "Launch app by package id", json::object()});
    }

    m_actionMap.clear();
    for (auto& action : m_actions)
        m_actionMap[action.name] = &action;

    m_queryMap.clear();
    for (auto& query : m_queries)
        m_queryMap[query.name] = &query;
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
