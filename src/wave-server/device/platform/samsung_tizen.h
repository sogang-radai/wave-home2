#pragma once

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../device.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

class SamsungTizenTokenClient
{
public:
    struct Config
    {
        std::string host;
        std::string mac;
        uint16_t port = 8002;
        std::string clientName = "WaveHome";
        uint32_t timeoutMs = 120000;
    };

    struct Result
    {
        bool ok = false;
        std::string token;
        std::string deviceName;
        std::string modelName;
        int errorCode = 0;
        std::string message;
    };

    SamsungTizenTokenClient();
    ~SamsungTizenTokenClient();

    int configure(const json& config);
    const Config& getConfig() const;

    Result requestToken();
    std::future<Result> requestTokenAsync();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Config m_config;
};

class SamsungTizen :
    public Device,
    public Queryable,
    public Actionable
{
public:
    struct InterfaceConfig
    {
        std::string host;
        std::string mac;
        std::string token;
        uint16_t port = 8002;
        std::string clientName = "WaveHome";
    };

    struct SessionConfig
    {
        uint32_t connectTimeoutMs = 8000;
        uint32_t commandTimeoutMs = 3000;
        uint32_t warmingTimeMs = 10000;
        uint32_t coolingTimeMs = 8000;
        uint32_t restPollIntervalMs = 1000;
        uint32_t restPollAttempts = 30;
    };

    struct Capabilities
    {
        bool power = true;
        bool volume = true;
        bool mute = true;
        bool navigation = true;
        bool channel = false;
        bool apps = false;
        bool inputs = true;
        std::vector<std::string> inputSources;
        std::string modelName;
        std::string firmwareVersion;
        std::string deviceType;
    };

    enum class SessionState
    {
        Disconnected,
        Connecting,
        Connected,
        CoolingDown,
        WarmingUp,
    };

    SamsungTizen();
    ~SamsungTizen() override;

    const InterfaceConfig& getInterfaceConfig() const;
    const SessionConfig& getSessionConfig() const;
    const Capabilities& getCapabilities() const;
    SessionState getSessionState() const;
    bool isApiReachable() const;

    int init(const json& config) override;
    void shutdown() override;

    json query(std::string_view name, const json& params) override;
    std::future<json> queryAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    int invoke(std::string_view name, const json& params) override;
    std::future<int> invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

private:
    struct Impl;

    void registerActionsAndQueries();

    int connectSession();
    void disconnectSession(bool remoteHangup);
    int sendRemoteKey(const std::string& key);
    int sendRemoteKeyRepeated(const std::string& key, int count);
    int setInput(std::string_view source);
    int wakeDisplay();
    int powerOn();
    int powerOff();
    int powerToggle();

    std::unique_ptr<Impl> m_impl;
    InterfaceConfig m_interface;
    SessionConfig m_session;
    Capabilities m_capabilities;
    SessionState m_sessionState = SessionState::Disconnected;
    std::string m_lastInput;
    std::string m_cachedPowerState;
    std::chrono::steady_clock::time_point m_restCacheTime {};
    mutable std::chrono::steady_clock::time_point m_lastTcpProbe {};
    mutable bool m_lastTcpReachable = false;
    mutable std::mutex m_mutex;
};

/*
Queries:
  capabilities, session, state, inputs, input

Actions:
  on, off, toggle, mute, volume_up, volume_down, channel_up, channel_down,
  input { "source": "hdmi1" | "hdmi2" | "hdmi3" | "hdmi4" | "displayport" },
  send_key { "key": "KEY_..." },
  open_app { "app": "package.id" },
  nav_up, nav_down, nav_left, nav_right, select, home, back
*/

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
