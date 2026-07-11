#include "demo_runtime_id.h"

#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

WAVE_NAMESPACE_BEGIN

namespace
{
    std::mutex g_preferredRuntimeMutex;
    std::string g_preferredRuntimeId;

    std::string randomHex(size_t bytes)
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0, 255);
        std::ostringstream stream;
        for (size_t i = 0; i < bytes; ++i)
        {
            stream << std::hex << std::setw(2) << std::setfill('0') << dist(rng);
        }
        return stream.str();
    }

    std::optional<std::string> parseCookieValue(const std::string& cookie_header, const char* name)
    {
        const std::string key = std::string(name) + "=";
        size_t pos = 0;
        while (pos < cookie_header.size())
        {
            const auto next = cookie_header.find(';', pos);
            const auto part = cookie_header.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            const auto trimmed_start = part.find_first_not_of(" \t");
            if (trimmed_start != std::string::npos)
            {
                const auto token = part.substr(trimmed_start);
                if (token.rfind(key, 0) == 0 && token.size() > key.size())
                    return token.substr(key.size());
            }
            if (next == std::string::npos)
                break;
            pos = next + 1;
        }
        return std::nullopt;
    }
}

std::string generateDemoRuntimeId()
{
    return randomHex(16);
}

void rememberPreferredDemoRuntimeId(const std::string& runtime_id)
{
    if (runtime_id.size() < 8)
        return;
    std::lock_guard<std::mutex> lock(g_preferredRuntimeMutex);
    g_preferredRuntimeId = runtime_id;
}

std::optional<std::string> preferredDemoRuntimeId()
{
    std::lock_guard<std::mutex> lock(g_preferredRuntimeMutex);
    if (g_preferredRuntimeId.size() < 8)
        return std::nullopt;
    return g_preferredRuntimeId;
}

std::string fallbackDemoRuntimeId()
{
    if (const auto preferred = preferredDemoRuntimeId())
        return *preferred;
    const auto minted = generateDemoRuntimeId();
    rememberPreferredDemoRuntimeId(minted);
    return minted;
}

std::optional<std::string> demoRuntimeIdFromCookie(const drogon::HttpRequestPtr& req)
{
    if (!req)
        return std::nullopt;
    const auto cookie = req->getHeader("Cookie");
    if (cookie.empty())
        return std::nullopt;
    auto value = parseCookieValue(cookie, kDemoRuntimeCookieName);
    if (!value || value->size() < 8)
        return std::nullopt;
    return value;
}

std::optional<std::string> demoRuntimeIdFromHeader(const drogon::HttpRequestPtr& req)
{
    if (!req)
        return std::nullopt;
    const auto value = req->getHeader(kDemoRuntimeHeaderName);
    if (value.empty() || value.size() < 8)
        return std::nullopt;
    return value;
}

void attachDemoRuntimeCookieIfNeeded(
    const drogon::HttpRequestPtr& req,
    const drogon::HttpResponsePtr& resp,
    const std::string& runtime_id)
{
    if (!resp || runtime_id.empty())
        return;
    rememberPreferredDemoRuntimeId(runtime_id);
    // Always expose the active runtime id so the SPA can pin the session even
    // when Set-Cookie is not visible to fetch()/document.cookie.
    resp->addHeader(kDemoRuntimeHeaderName, runtime_id);
    if (demoRuntimeIdFromCookie(req))
        return;
    resp->addHeader(
        "Set-Cookie",
        std::string(kDemoRuntimeCookieName) + "=" + runtime_id + "; Path=/; SameSite=Lax; Max-Age=2700");
}

WAVE_NAMESPACE_END
