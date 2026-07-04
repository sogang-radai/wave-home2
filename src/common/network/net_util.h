#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace net
{
    // Parses a MAC string ("aa:bb:cc:dd:ee:ff", "aa-bb-...", "aabbccddeeff",
    // or with dropped leading zeros like "4:e4:..:a") into 6 bytes.
    bool parseMac(std::string_view text, uint8_t out[6]);

    // Normalized lowercase colon form, e.g. "04:e4:b6:a9:8d:0a".
    std::string macToString(const uint8_t mac[6]);

    // True if both strings parse to the same 6 bytes, ignoring case,
    // separator style and dropped leading zeros.
    bool macEquals(std::string_view a, std::string_view b);

    // Resolves the current link-layer (MAC) address for an IPv4 host from the
    // system ARP cache. The cache is warmed best-effort first. On success
    // outMac is set to the normalized colon form. Returns false if the address
    // could not be resolved (host offline or not yet in the cache).
    bool resolveMacForIp(const std::string& ipv4, std::string& outMac);
}
