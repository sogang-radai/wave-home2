#include "demo_session_registry.h"

#include <chrono>

WAVE_NAMESPACE_BEGIN

namespace
{
    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
}

DemoSessionRegistry& DemoSessionRegistry::instance()
{
    static DemoSessionRegistry registry;
    return registry;
}

DemoSessionData& DemoSessionRegistry::touch(const std::string& runtime_id)
{
    std::lock_guard lock(m_mutex);
    evictExpired();
    auto& session = m_sessions[runtime_id];
    session.last_touch_ms = nowMs();
    return session;
}

std::optional<DemoSessionData> DemoSessionRegistry::get(const std::string& runtime_id) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_sessions.find(runtime_id);
    if (it == m_sessions.end())
        return std::nullopt;
    return it->second;
}

void DemoSessionRegistry::evictExpired(int64_t ttl_ms, size_t max_sessions)
{
    const auto now = nowMs();
    for (auto it = m_sessions.begin(); it != m_sessions.end();)
    {
        if (now - it->second.last_touch_ms > ttl_ms)
            it = m_sessions.erase(it);
        else
            ++it;
    }
    while (m_sessions.size() > max_sessions)
    {
        auto oldest = m_sessions.begin();
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
        {
            if (it->second.last_touch_ms < oldest->second.last_touch_ms)
                oldest = it;
        }
        m_sessions.erase(oldest);
    }
}

WAVE_NAMESPACE_END
