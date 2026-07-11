#include "net_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <net/if_dl.h>
#include <net/route.h>
#endif

namespace net
{
    namespace
    {
        int hexVal(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // Sends a datagram to the host so the kernel performs ARP resolution and
        // populates the neighbour cache. Best-effort; failures are ignored.
        void warmArpCache(const std::string& ipv4)
        {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0)
                return;

            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(9); // discard
            if (::inet_pton(AF_INET, ipv4.c_str(), &addr.sin_addr) == 1)
            {
                const uint8_t byte = 0;
                (void)::sendto(fd, &byte, sizeof(byte), 0,
                    reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            }
            ::close(fd);
        }

#if defined(__APPLE__) || defined(__FreeBSD__)
        bool readArpOnce(uint32_t targetAddr, uint8_t out[6])
        {
            int mib[6] = {
                CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS,
#ifdef RTF_LLINFO
                RTF_LLINFO
#else
                0
#endif
            };

            size_t needed = 0;
            if (::sysctl(mib, 6, nullptr, &needed, nullptr, 0) < 0 || needed == 0)
                return false;

            std::string buffer(needed, '\0');
            if (::sysctl(mib, 6, buffer.data(), &needed, nullptr, 0) < 0)
                return false;

            char* end = buffer.data() + needed;
            for (char* next = buffer.data(); next < end;)
            {
                auto* rtm = reinterpret_cast<rt_msghdr*>(next);
                auto* sin = reinterpret_cast<sockaddr_in*>(rtm + 1);
                auto* sdl = reinterpret_cast<sockaddr_dl*>(
                    reinterpret_cast<char*>(sin) + ((sin->sin_len + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1)));

                if (sin->sin_addr.s_addr == targetAddr && sdl->sdl_alen == 6)
                {
                    std::memcpy(out, LLADDR(sdl), 6);
                    return true;
                }
                next += rtm->rtm_msglen;
            }
            return false;
        }
#else
        bool readArpOnce(uint32_t targetAddr, uint8_t out[6])
        {
            in_addr want {};
            want.s_addr = targetAddr;
            char wantStr[INET_ADDRSTRLEN] = {0};
            if (!::inet_ntop(AF_INET, &want, wantStr, sizeof(wantStr)))
                return false;

            FILE* fp = std::fopen("/proc/net/arp", "r");
            if (!fp)
                return false;

            char line[256];
            (void)std::fgets(line, sizeof(line), fp); // header

            bool found = false;
            while (std::fgets(line, sizeof(line), fp))
            {
                char ip[64] = {0};
                char hw[64] = {0};
                unsigned flags = 0;
                // ip  hw_type  flags  hw_addr  mask  device
                if (std::sscanf(line, "%63s %*s 0x%x %63s", ip, &flags, hw) != 3)
                    continue;
                if ((flags & 0x2) == 0) // ATF_COM (complete) not set
                    continue;
                if (std::strcmp(ip, wantStr) != 0)
                    continue;

                uint8_t parsed[6];
                if (parseMac(hw, parsed))
                {
                    std::memcpy(out, parsed, 6);
                    found = true;
                }
                break;
            }
            std::fclose(fp);
            return found;
        }
#endif
    }

    bool parseMac(std::string_view text, uint8_t out[6])
    {
        const bool hasSep =
            text.find(':') != std::string_view::npos || text.find('-') != std::string_view::npos;

        if (hasSep)
        {
            int idx = 0;
            size_t i = 0;
            while (i < text.size() && idx < 6)
            {
                int value = 0;
                int digits = 0;
                while (i < text.size() && text[i] != ':' && text[i] != '-')
                {
                    const int d = hexVal(text[i]);
                    if (d < 0)
                        return false;
                    value = value * 16 + d;
                    if (++digits > 2)
                        return false;
                    ++i;
                }
                if (digits == 0)
                    return false;
                out[idx++] = static_cast<uint8_t>(value);
                if (i < text.size())
                    ++i; // skip separator
            }
            return idx == 6 && i >= text.size();
        }

        if (text.size() != 12)
            return false;
        for (int b = 0; b < 6; ++b)
        {
            const int hi = hexVal(text[2 * b]);
            const int lo = hexVal(text[2 * b + 1]);
            if (hi < 0 || lo < 0)
                return false;
            out[b] = static_cast<uint8_t>(hi * 16 + lo);
        }
        return true;
    }

    std::string macToString(const uint8_t mac[6])
    {
        char buffer[18];
        std::snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return buffer;
    }

    bool macEquals(std::string_view a, std::string_view b)
    {
        uint8_t ma[6];
        uint8_t mb[6];
        if (!parseMac(a, ma) || !parseMac(b, mb))
            return false;
        return std::memcmp(ma, mb, 6) == 0;
    }

    bool resolveMacForIp(const std::string& ipv4, std::string& outMac)
    {
        in_addr addr {};
        if (::inet_pton(AF_INET, ipv4.c_str(), &addr) != 1)
            return false;

        const uint32_t target = addr.s_addr;
        uint8_t mac[6];

        if (readArpOnce(target, mac))
        {
            outMac = macToString(mac);
            return true;
        }

        warmArpCache(ipv4);
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (readArpOnce(target, mac))
            {
                outMac = macToString(mac);
                return true;
            }
        }
        return false;
    }
}
