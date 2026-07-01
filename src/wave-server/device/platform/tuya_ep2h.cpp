#include "tuya_ep2h.h"

#include <cstdint>
#include <stdexcept>
#include <unistd.h>

#include "tuyaAPI.hpp"

#include "../../core/logger.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    constexpr size_t kMaxBufferSize = 4096;

    // Gosund EP2 / smartplugv1 mapping (make-all/tuya-local)
    constexpr const char* kDpSwitch = "1";
    constexpr const char* kDpTimer = "2";
    constexpr const char* kDpCurrentMa = "4";
    constexpr const char* kDpPowerRaw = "5";
    constexpr const char* kDpVoltageRaw = "6";
    constexpr const char* kDpOvercurrent = "7";

    // Alternate Tuya energy-monitor mapping (developer platform / smartplug v2)
    constexpr const char* kDpEnergyRaw = "17";
    constexpr const char* kDpCurrentAlt = "18";
    constexpr const char* kDpPowerAlt = "19";
    constexpr const char* kDpVoltageAlt = "20";

    json makeQueryError(int code, std::string_view message = {})
    {
        json out = json::object();
        out["code"] = code;
        if (!message.empty())
            out["message"] = std::string(message);
        return out;
    }

    TuyaEP2H::Config parseConfig(const json& config)
    {
        const auto& iface = config.at("interface");

        TuyaEP2H::Config out;
        out.host = iface.at("host").get<std::string>();
        out.deviceId = iface.at("device_id").get<std::string>();
        out.localKey = iface.at("local_key").get<std::string>();
        out.version = iface.value("version", "3.3");
        return out;
    }

    void validateTuyaEp2hConfig(const json& config)
    {
        if (config.at("class").get<std::string>() != "tuya_ep2h")
            throw std::invalid_argument("tuya_ep2h config field 'class' must be 'tuya_ep2h'");

        if (!config.contains("interface") || !config["interface"].is_object())
            throw std::invalid_argument("tuya_ep2h requires object field 'interface'");

        const auto& iface = config["interface"];
        for (const char* key : {"host", "device_id", "local_key"})
        {
            if (!iface.contains(key) || !iface[key].is_string() || iface[key].get<std::string>().empty())
                throw std::invalid_argument(std::string("tuya_ep2h interface requires non-empty string '") + key + "'");
        }

        if (iface.contains("version") && !iface["version"].is_string())
            throw std::invalid_argument("tuya_ep2h interface field 'version' must be a string");
    }

    json parseTuyaResponse(const std::string& response)
    {
        if (response.empty())
            throw std::runtime_error("empty tuya response");

        json parsed = json::parse(response);
        if (!parsed.contains("dps") || !parsed["dps"].is_object())
            throw std::runtime_error("tuya response missing dps object");

        return parsed["dps"];
    }

    bool readSwitchState(const json& dps)
    {
        if (dps.contains(kDpSwitch))
            return dps[kDpSwitch].get<bool>();
        return false;
    }

    double readVoltageVolts(const json& dps)
    {
        if (dps.contains(kDpVoltageRaw))
            return dps[kDpVoltageRaw].get<double>() / 10.0;
        if (dps.contains(kDpVoltageAlt))
            return dps[kDpVoltageAlt].get<double>() / 10.0;
        throw std::runtime_error("voltage datapoint not available");
    }

    double readCurrentMilliamps(const json& dps)
    {
        if (dps.contains(kDpCurrentMa))
            return dps[kDpCurrentMa].get<double>();
        if (dps.contains(kDpCurrentAlt))
            return dps[kDpCurrentAlt].get<double>();
        throw std::runtime_error("current datapoint not available");
    }

    double readPowerWatts(const json& dps)
    {
        if (dps.contains(kDpPowerRaw))
            return dps[kDpPowerRaw].get<double>() / 10.0;
        if (dps.contains(kDpPowerAlt))
            return dps[kDpPowerAlt].get<double>() / 10.0;
        throw std::runtime_error("power datapoint not available");
    }

    double readEnergyKwh(const json& dps)
    {
        if (!dps.contains(kDpEnergyRaw))
            throw std::runtime_error("energy datapoint not available");

        return dps[kDpEnergyRaw].get<double>() / 1000.0;
    }

    json buildStatusResult(const json& dps)
    {
        json out = json::object();
        out["switch"] = readSwitchState(dps);

        if (dps.contains(kDpVoltageRaw) || dps.contains(kDpVoltageAlt))
            out["voltage_v"] = readVoltageVolts(dps);
        if (dps.contains(kDpCurrentMa) || dps.contains(kDpCurrentAlt))
            out["current_ma"] = readCurrentMilliamps(dps);
        if (dps.contains(kDpPowerRaw) || dps.contains(kDpPowerAlt))
            out["power_w"] = readPowerWatts(dps);
        if (dps.contains(kDpEnergyRaw))
            out["energy_kwh"] = readEnergyKwh(dps);
        if (dps.contains(kDpTimer))
            out["timer_s"] = dps[kDpTimer];
        if (dps.contains(kDpOvercurrent))
            out["overcurrent_alarm"] = dps[kDpOvercurrent];

        out["raw_dps"] = dps;
        return out;
    }
}

struct TuyaEP2H::Impl
{
    TuyaEP2H::Config config;

    json queryDatapoints()
    {
        std::unique_ptr<tuyaAPI> client(tuyaAPI::create(config.version));
        if (!client)
            throw std::runtime_error("unsupported tuya protocol version: " + config.version);

        if (!client->ConnectToDevice(config.host))
        {
            throw std::runtime_error(
                "failed to connect to tuya device at " + config.host +
                " (socket error " + std::to_string(client->getlasterror()) + ")");
        }

        unsigned char messageBuffer[kMaxBufferSize];
        const std::string payload = client->GeneratePayload(TUYA_DP_QUERY, config.deviceId, "");
        const int payloadLen = client->BuildTuyaMessage(messageBuffer, TUYA_DP_QUERY, payload, config.localKey);
        if (payloadLen <= 0)
            throw std::runtime_error("failed to build tuya DP_QUERY message");

        if (client->send(messageBuffer, payloadLen) < 0)
            throw std::runtime_error("failed to send tuya DP_QUERY");

        usleep(100000);

        const int numBytes = client->receive(messageBuffer, kMaxBufferSize - 1);
        if (numBytes < 0)
            throw std::runtime_error("failed to receive tuya DP_QUERY response");

        const std::string response = client->DecodeTuyaMessage(messageBuffer, numBytes, config.localKey);
        client->disconnect();
        return parseTuyaResponse(response);
    }

    void controlSwitch(bool on)
    {
        std::unique_ptr<tuyaAPI> client(tuyaAPI::create(config.version));
        if (!client)
            throw std::runtime_error("unsupported tuya protocol version: " + config.version);

        if (!client->ConnectToDevice(config.host))
        {
            throw std::runtime_error(
                "failed to connect to tuya device at " + config.host +
                " (socket error " + std::to_string(client->getlasterror()) + ")");
        }

        unsigned char messageBuffer[kMaxBufferSize];
        const std::string dps = std::string("{\"1\":") + (on ? "true" : "false") + "}";
        const std::string payload = client->GeneratePayload(TUYA_CONTROL, config.deviceId, dps);
        const int payloadLen = client->BuildTuyaMessage(messageBuffer, TUYA_CONTROL, payload, config.localKey);
        if (payloadLen <= 0)
            throw std::runtime_error("failed to build tuya CONTROL message");

        if (client->send(messageBuffer, payloadLen) < 0)
            throw std::runtime_error("failed to send tuya CONTROL");

        usleep(100000);

        const int numBytes = client->receive(messageBuffer, kMaxBufferSize - 1);
        if (numBytes < 0)
            throw std::runtime_error("failed to receive tuya CONTROL response");

        (void)client->DecodeTuyaMessage(messageBuffer, numBytes, config.localKey);
        client->disconnect();
    }
};

// ============================================================================
// TuyaEP2H
// ============================================================================

TuyaEP2H::TuyaEP2H() :
    Device(),
    m_impl(std::make_unique<Impl>())
{
    registerActionsAndQueries();
}

TuyaEP2H::~TuyaEP2H()
{
    shutdown();
}

const TuyaEP2H::Config& TuyaEP2H::getConfig() const
{
    return m_config;
}

// ============================================================================
// Device
// ============================================================================

int TuyaEP2H::init(const json& config)
{
    validateTuyaEp2hConfig(config);
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
        (void)m_impl->queryDatapoints();
        m_state = DeviceState::Running;
        LOG_INFO("TuyaEP2H connected: {}", m_config.host);
        return 0;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("TuyaEP2H init failed: {}", ex.what());
        m_state = DeviceState::Stopped;
        return -5;
    }
}

void TuyaEP2H::shutdown()
{
    if (m_state == DeviceState::Uninitialized)
        return;

    m_state = DeviceState::ShuttingDown;
    m_state = DeviceState::Stopped;
}

std::string_view TuyaEP2H::getClass() const
{
    return "tuya_ep2h";
}

// ============================================================================
// Queryable
// ============================================================================

json TuyaEP2H::query(std::string_view name, const json& params)
{
    (void)params;

    if (m_state != DeviceState::Running)
        return makeQueryError(-4);

    try
    {
        const json dps = fetchDatapoints();

        if (name == "switch")
        {
            return {
                {"value", readSwitchState(dps)},
            };
        }

        if (name == "voltage")
        {
            return {
                {"value", readVoltageVolts(dps)},
                {"unit", "V"},
            };
        }

        if (name == "current")
        {
            return {
                {"value", readCurrentMilliamps(dps)},
                {"unit", "mA"},
            };
        }

        if (name == "power")
        {
            return {
                {"value", readPowerWatts(dps)},
                {"unit", "W"},
            };
        }

        if (name == "energy")
        {
            return {
                {"value", readEnergyKwh(dps)},
                {"unit", "kWh"},
            };
        }

        if (name == "status")
            return buildStatusResult(dps);

        return makeQueryError(-8);
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("TuyaEP2H query failed: {}", ex.what());
        return makeQueryError(-5, ex.what());
    }
}

std::future<json> TuyaEP2H::queryAsync(std::string_view name, const json& params, uint32_t timeout_ms)
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

int TuyaEP2H::invoke(std::string_view name, const json& params)
{
    (void)params;

    if (m_state != DeviceState::Running)
        return -4;

    if (name == "on")
        return setSwitch(true);
    if (name == "off")
        return setSwitch(false);
    if (name == "toggle")
    {
        try
        {
            const json dps = fetchDatapoints();
            return setSwitch(!readSwitchState(dps));
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR("TuyaEP2H toggle failed: {}", ex.what());
            return -5;
        }
    }

    return -8;
}

std::future<int> TuyaEP2H::invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return invoke(name, params);
    });
}

void TuyaEP2H::registerActionsAndQueries()
{
    m_actions = {
        {Action::Json, "on", "Turn the plug on", json::object()},
        {Action::Json, "off", "Turn the plug off", json::object()},
        {Action::Json, "toggle", "Toggle plug power state", json::object()},
    };

    m_queries = {
        {Query::Json, "switch", "Current on/off state", json::object()},
        {Query::Json, "voltage", "AC voltage in volts", json::object()},
        {Query::Json, "current", "Current draw in milliamps", json::object()},
        {Query::Json, "power", "Active power in watts", json::object()},
        {Query::Json, "energy", "Accumulated energy in kWh (if supported)", json::object()},
        {Query::Json, "status", "All readable datapoints", json::object()},
    };

    m_actionMap.clear();
    for (auto& action : m_actions)
        m_actionMap[action.name] = &action;

    m_queryMap.clear();
    for (auto& query : m_queries)
        m_queryMap[query.name] = &query;
}

json TuyaEP2H::fetchDatapoints()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_impl->queryDatapoints();
}

int TuyaEP2H::setSwitch(bool on)
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_impl->controlSwitch(on);
        return 0;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("TuyaEP2H setSwitch failed: {}", ex.what());
        return -5;
    }
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
