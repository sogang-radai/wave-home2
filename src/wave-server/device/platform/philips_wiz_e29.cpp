#include "philips_wiz_e29.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../core/logger.h"
#include "network/net_util.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    constexpr const char* kClass = "philips_wiz_e29";

    json makeError(int code, std::string_view message = {})
    {
        json out = json::object();
        out["code"] = code;
        if (!message.empty())
            out["message"] = std::string(message);
        return out;
    }

    PhilipsWizE29::Config parseConfig(const json& config)
    {
        const auto& iface = config.at("interface");

        PhilipsWizE29::Config out;
        out.className = config.at("class").get<std::string>();
        out.host = iface.at("host").get<std::string>();
        out.mac = iface.value("mac", "");
        out.port = static_cast<uint16_t>(iface.value("port", 38899));
        return out;
    }

    void validateConfig(const json& config)
    {
        if (config.at("class").get<std::string>().rfind("philips_wiz_e29", 0) != 0)
            throw std::invalid_argument("philips_wiz_e29 config field 'class' must start with 'philips_wiz_e29'");

        if (!config.contains("interface") || !config["interface"].is_object())
            throw std::invalid_argument("philips_wiz_e29 requires object field 'interface'");

        const auto& iface = config["interface"];
        if (!iface.contains("host") || !iface["host"].is_string() || iface["host"].get<std::string>().empty())
            throw std::invalid_argument("philips_wiz_e29 interface requires non-empty string 'host'");
    }

    // Derives capabilities from the WiZ moduleName (e.g. "ESP01_SHRGB1C_31"),
    // cross-checked against the fields the bulb reports in getPilot.
    PhilipsWizE29::Capabilities deriveCapabilities(const std::string& moduleName, const json& pilot)
    {
        PhilipsWizE29::Capabilities caps;
        caps.module = moduleName;
        caps.dimming = true;

        const bool moduleRGB = moduleName.find("RGB") != std::string::npos;
        const bool moduleTW = moduleName.find("TW") != std::string::npos;
        const bool pilotRGB = pilot.contains("r") && pilot.contains("g") && pilot.contains("b");
        const bool pilotTemp = pilot.contains("temp");

        if (moduleRGB || pilotRGB)
        {
            caps.color = true;
            caps.tunableWhite = true;
            caps.tempMinK = 2200;
            caps.tempMaxK = 6500;
        }
        else if (moduleTW || pilotTemp)
        {
            caps.tunableWhite = true;
            caps.tempMinK = 2700;
            caps.tempMaxK = 6500;
        }
        return caps;
    }
}

// ============================================================================
// Impl (WiZ local UDP protocol)
// ============================================================================

struct PhilipsWizE29::Impl
{
    Config config;

    json call(const json& request) const
    {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            throw std::runtime_error("wiz: socket() failed");

        struct FdGuard
        {
            int fd;
            ~FdGuard() { if (fd >= 0) ::close(fd); }
        } guard{fd};

        timeval tv {};
        tv.tv_sec = 2;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config.port);
        if (::inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("wiz: invalid host " + config.host);

        const std::string payload = request.dump();
        if (::sendto(fd, payload.data(), payload.size(), 0,
                reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("wiz: sendto failed");

        char buffer[2048];
        const ssize_t n = ::recvfrom(fd, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr);
        if (n <= 0)
            throw std::runtime_error("wiz: no response from " + config.host + " (timeout)");
        buffer[n] = '\0';

        json response = json::parse(buffer, nullptr, false);
        if (response.is_discarded())
            throw std::runtime_error("wiz: malformed JSON response");
        if (response.contains("error"))
            throw std::runtime_error("wiz: device error " + response["error"].dump());
        return response;
    }

    json getPilot() const
    {
        const json response = call({{"method", "getPilot"}, {"params", json::object()}});
        if (!response.contains("result"))
            throw std::runtime_error("wiz: getPilot missing result");
        return response["result"];
    }

    json getSystemConfig() const
    {
        const json response = call({{"method", "getSystemConfig"}, {"params", json::object()}});
        if (!response.contains("result"))
            throw std::runtime_error("wiz: getSystemConfig missing result");
        return response["result"];
    }

    bool setPilot(const json& params) const
    {
        const json response = call({{"method", "setPilot"}, {"params", params}});
        return response.contains("result") && response["result"].value("success", false);
    }
};

// ============================================================================
// PhilipsWizE29
// ============================================================================

PhilipsWizE29::PhilipsWizE29() :
    Device(),
    m_impl(std::make_unique<Impl>())
{
    registerActionsAndQueries();
}

PhilipsWizE29::~PhilipsWizE29()
{
    shutdown();
}

const PhilipsWizE29::Config& PhilipsWizE29::getConfig() const
{
    return m_config;
}

const PhilipsWizE29::Capabilities& PhilipsWizE29::getCapabilities() const
{
    return m_capabilities;
}

// ============================================================================
// Device
// ============================================================================

int PhilipsWizE29::init(const json& config)
{
    validateConfig(config);
    loadBaseConfig(config);
    m_config = parseConfig(config);
    m_impl->config = m_config;

    if (!isEnabled())
        return -2;

    if (m_state == DeviceState::Running)
        return 0;

    if (m_state != DeviceState::Uninitialized && m_state != DeviceState::Stopped)
        return -3;

    m_state = DeviceState::Initializing;

    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const json sys = m_impl->getSystemConfig();
        const std::string moduleName = sys.value("moduleName", "");
        const std::string mac = sys.value("mac", "");

        if (!m_config.mac.empty() && !mac.empty() && !net::macEquals(m_config.mac, mac))
        {
            LOG_ERROR("philips_wiz_e29: MAC mismatch for {} (expected {}, got {})",
                m_config.host, m_config.mac, mac);
            m_state = DeviceState::Stopped;
            return -7;
        }

        const json pilot = m_impl->getPilot();
        m_capabilities = deriveCapabilities(moduleName, pilot);
        registerActionsAndQueries();

        m_state = DeviceState::Running;
        LOG_INFO("philips_wiz_e29 connected: {} (module={}, color={}, tunableWhite={})",
            m_config.host, moduleName, m_capabilities.color, m_capabilities.tunableWhite);
        return 0;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("philips_wiz_e29 init failed: {}", ex.what());
        m_state = DeviceState::Stopped;
        return -5;
    }
}

void PhilipsWizE29::shutdown()
{
    if (m_state == DeviceState::Uninitialized)
        return;

    m_state = DeviceState::ShuttingDown;
    m_state = DeviceState::Stopped;
}

std::string_view PhilipsWizE29::getClass() const
{
    return m_config.className;
}

// ============================================================================
// Queryable
// ============================================================================

json PhilipsWizE29::query(std::string_view name, const json& params)
{
    (void)params;

    if (name == "capabilities")
    {
        return {
            {"class", m_config.className},
            {"dimming", m_capabilities.dimming},
            {"color", m_capabilities.color},
            {"tunable_white", m_capabilities.tunableWhite},
            {"temp_min_k", m_capabilities.tempMinK},
            {"temp_max_k", m_capabilities.tempMaxK},
            {"module", m_capabilities.module},
        };
    }

    if (m_state != DeviceState::Running)
        return makeError(-4);

    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const json pilot = m_impl->getPilot();
        const bool on = pilot.value("state", false);
        const int dimming = pilot.value("dimming", 0);

        if (name == "state")
            return {{"on", on}, {"brightness", dimming}};

        if (name == "brightness")
            return {{"value", dimming}, {"unit", "%"}};

        if (name == "color")
        {
            if (!m_capabilities.color)
                return makeError(-8, "color not supported by this model");
            return {{"r", pilot.value("r", 0)}, {"g", pilot.value("g", 0)}, {"b", pilot.value("b", 0)}};
        }

        if (name == "temperature")
        {
            if (!m_capabilities.tunableWhite)
                return makeError(-8, "temperature not supported by this model");
            return {{"value", pilot.value("temp", 0)}, {"unit", "K"}};
        }

        if (name == "status")
            return {{"on", on}, {"brightness", dimming}, {"raw", pilot}};

        return makeError(-8);
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("philips_wiz_e29 query failed: {}", ex.what());
        return makeError(-5, ex.what());
    }
}

std::future<json> PhilipsWizE29::queryAsync(std::string_view name, const json& params, uint32_t timeout_ms)
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

int PhilipsWizE29::invoke(std::string_view name, const json& params)
{
    if (m_state != DeviceState::Running)
        return -4;

    if (name == "on")
        return setPower(true);
    if (name == "off")
        return setPower(false);
    if (name == "toggle")
        return togglePower();

    if (name == "brightness")
    {
        if (!params.contains("value") || !params["value"].is_number())
            return -9;
        const int value = std::clamp(params["value"].get<int>(), 10, 100);
        return setBrightness(static_cast<uint8_t>(value));
    }

    if (name == "color")
    {
        if (!m_capabilities.color)
            return -8;
        for (const char* c : {"r", "g", "b"})
            if (!params.contains(c) || !params[c].is_number())
                return -9;
        const auto ch = [&](const char* c) { return static_cast<uint8_t>(std::clamp(params[c].get<int>(), 0, 255)); };
        return setColorRGB(ch("r"), ch("g"), ch("b"));
    }

    if (name == "temperature")
    {
        if (!m_capabilities.tunableWhite)
            return -8;
        if (!params.contains("value") || !params["value"].is_number())
            return -9;
        const int kelvin = std::clamp(
            params["value"].get<int>(),
            static_cast<int>(m_capabilities.tempMinK),
            static_cast<int>(m_capabilities.tempMaxK));
        return setColorTemperature(static_cast<uint16_t>(kelvin));
    }

    return -8;
}

std::future<int> PhilipsWizE29::invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return invoke(name, params);
    });
}

// ============================================================================
// Control helpers
// ============================================================================

int PhilipsWizE29::setPower(bool on)
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_impl->setPilot({{"state", on}}) ? 0 : -5;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("philips_wiz_e29 setPower failed: {}", ex.what());
        return -5;
    }
}

int PhilipsWizE29::togglePower()
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const json pilot = m_impl->getPilot();
        const bool on = pilot.value("state", false);
        return m_impl->setPilot({{"state", !on}}) ? 0 : -5;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("philips_wiz_e29 togglePower failed: {}", ex.what());
        return -5;
    }
}

int PhilipsWizE29::setBrightness(uint8_t percent)
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_impl->setPilot({{"dimming", percent}}) ? 0 : -5;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("philips_wiz_e29 setBrightness failed: {}", ex.what());
        return -5;
    }
}

int PhilipsWizE29::setColorRGB(uint8_t r, uint8_t g, uint8_t b)
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_impl->setPilot({{"r", r}, {"g", g}, {"b", b}}) ? 0 : -5;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("philips_wiz_e29 setColorRGB failed: {}", ex.what());
        return -5;
    }
}

int PhilipsWizE29::setColorTemperature(uint16_t kelvin)
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_impl->setPilot({{"temp", kelvin}}) ? 0 : -5;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("philips_wiz_e29 setColorTemperature failed: {}", ex.what());
        return -5;
    }
}

// ============================================================================
// Registration
// ============================================================================

void PhilipsWizE29::registerActionsAndQueries()
{
    m_actions = {
        {Action::Json, Action::Stateful, "on", "Turn the bulb on", json::object()},
        {Action::Json, Action::Stateful, "off", "Turn the bulb off", json::object()},
        {Action::Json, Action::Toggle | Action::Stateful, "toggle", "Toggle power state", json::object()},
        {Action::Json, Action::Stateful, "brightness", "Set dimming level (10-100)", json::object()},
    };

    m_queries = {
        {Query::Json, "capabilities", "Probed hardware capabilities", json::object()},
        {Query::Json, "state", "Current power and brightness", json::object()},
        {Query::Json, "brightness", "Current dimming level in percent", json::object()},
        {Query::Json, "status", "All readable pilot fields", json::object()},
    };

    if (m_capabilities.color)
    {
        m_actions.push_back({Action::Json, Action::Stateful, "color", "Set RGB color (0-255 per channel)", json::object()});
        m_queries.push_back({Query::Json, "color", "Current RGB color", json::object()});
    }

    if (m_capabilities.tunableWhite)
    {
        m_actions.push_back({Action::Json, Action::Stateful, "temperature", "Set white color temperature in kelvin", json::object()});
        m_queries.push_back({Query::Json, "temperature", "Current white color temperature", json::object()});
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
