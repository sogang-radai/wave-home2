#include "ir_device.h"

#include <stdexcept>

#include "../../core/logger.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

namespace
{
    json makeQueryError(int code, std::string_view message = {})
    {
        json out = json::object();
        out["code"] = code;
        if (!message.empty())
            out["message"] = std::string(message);
        return out;
    }

    std::string readOptionalString(const json& object, std::string_view key)
    {
        if (!object.contains(key) || !object[key].is_string())
            return {};

        return object[key].get<std::string>();
    }

    bool isSupportedIrClass(std::string_view className)
    {
        return className == "ir_reciever" ||
            className == "ir_remote" ||
            className == "ir_device";
    }

    void validateIrDeviceConfig(const json& config)
    {
        if (!config.contains("class") || !config["class"].is_string())
            throw std::invalid_argument("ir device config requires string field 'class'");

        const auto className = config["class"].get<std::string>();
        if (!isSupportedIrClass(className))
            throw std::invalid_argument("unsupported ir device class: " + className);

        if (!config.contains("interface") || !config["interface"].is_object())
            throw std::invalid_argument("ir device requires object field 'interface'");

        const auto& iface = config["interface"];
        if (iface.contains("transport") && !iface["transport"].is_string())
            throw std::invalid_argument("ir device interface field 'transport' must be a string");

        if (className == "ir_reciever")
        {
            if (!iface.contains("device") || !iface["device"].is_string() || iface["device"].get<std::string>().empty())
                throw std::invalid_argument("ir_reciever interface requires non-empty string 'device'");
        }
        else if (className == "ir_remote")
        {
            if (!iface.contains("device") || !iface["device"].is_string() || iface["device"].get<std::string>().empty())
                throw std::invalid_argument("ir_remote interface requires non-empty string 'device'");
            if (!iface.contains("command_list") || !iface["command_list"].is_string())
                throw std::invalid_argument("ir_remote interface requires string 'command_list'");
        }
        else if (className == "ir_device")
        {
            const bool hasInput =
                (iface.contains("input_device") && iface["input_device"].is_string()) ||
                (iface.contains("receive_device") && iface["receive_device"].is_string());
            const bool hasOutput =
                (iface.contains("output_device") && iface["output_device"].is_string()) ||
                (iface.contains("transmit_device") && iface["transmit_device"].is_string());
            const bool hasLegacyDevice = iface.contains("device") && iface["device"].is_string();
            if (!hasInput && !hasOutput && !hasLegacyDevice)
                throw std::invalid_argument("ir_device interface requires input_device, output_device, or device");
        }
    }

    void applyIrDevicePaths(const json& iface, IRDevice::Config& out)
    {
        out.inputDevice = readOptionalString(iface, "input_device");
        if (out.inputDevice.empty())
            out.inputDevice = readOptionalString(iface, "receive_device");

        out.outputDevice = readOptionalString(iface, "output_device");
        if (out.outputDevice.empty())
            out.outputDevice = readOptionalString(iface, "transmit_device");

        if (out.inputDevice.empty() && out.outputDevice.empty())
            out.inputDevice = readOptionalString(iface, "device");
    }

    IRDevice::Config parseConfig(const json& config)
    {
        const auto& iface = config.at("interface");
        const std::string className = config.at("class").get<std::string>();

        IRDevice::Config out;
        out.transport = iface.value("transport", "lirc");

        if (className == "ir_reciever")
        {
            out.inputDevice = iface.at("device").get<std::string>();
        }
        else if (className == "ir_remote")
        {
            out.outputDevice = iface.at("device").get<std::string>();
            out.commandListPath = iface.at("command_list").get<std::string>();
        }
        else
        {
            applyIrDevicePaths(iface, out);
            out.commandListPath = readOptionalString(iface, "command_list");
        }

        return out;
    }

    json payloadToJson(const ir::Payload& payload)
    {
        json out = json::object();
        out["kind"] = ir::Payload::kindToString(payload.kind());
        out["protocol"] = ir::Payload::protocolToString(payload.protocol());

        switch (payload.kind())
        {
        case ir::Payload::Kind::NecCodeOnly:
            out["code"] = payload.code();
            break;
        case ir::Payload::Kind::NecCodeData:
            out["code"] = payload.code();
            out["data"] = payload.data();
            break;
        case ir::Payload::Kind::LgAc28:
            out["raw28"] = payload.raw28();
            break;
        case ir::Payload::Kind::Repeat:
            break;
        default:
            if (ir::Payload::isRawKind(payload.kind()))
            {
                out["bit_count"] = payload.bitCount();
                out["raw_bits"] = payload.rawBits();
            }
            break;
        }

        if (payload.isNec())
            out["nec_wire32"] = payload.necWire32();

        return out;
    }
}

// ============================================================================
// IRDevice
// ============================================================================

IRDevice::IRDevice() = default;

IRDevice::~IRDevice()
{
    shutdown();
}

const IRDevice::Config& IRDevice::getConfig() const
{
    return m_config;
}

// ============================================================================
// Device
// ============================================================================

int IRDevice::init(const json& config)
{
    validateIrDeviceConfig(config);
    loadBaseConfig(config);
    m_config = parseConfig(config);
    m_className = config.at("class").get<std::string>();

    if (!isEnabled())
        return -2;

    if (m_state == DeviceState::Running)
        return 0;

    if (m_state != DeviceState::Uninitialized && m_state != DeviceState::Stopped)
        return -3;

    m_state = DeviceState::Initializing;

    if (!m_config.inputDevice.empty() || !m_config.outputDevice.empty())
        m_commandList = std::make_shared<ir::CommandList>();

    if (!m_config.commandListPath.empty() && m_commandList)
    {
        if (!m_commandList->loadFromFile(m_config.commandListPath))
            LOG_WARN("IRDevice failed to load command list: {}", m_config.commandListPath);
    }

    if (!m_config.inputDevice.empty())
    {
        m_receiver = std::make_unique<ir::Receiver>(m_commandList);
        const ir::Result rxRes = m_receiver->init(m_config.inputDevice);
        if (rxRes != ir::Result::SUCCESS)
        {
            LOG_ERROR("IRDevice receiver init failed: {}", ir::to_string(rxRes));
            shutdown();
            return irResultToCode(rxRes);
        }
    }

    if (!m_config.outputDevice.empty())
    {
        m_transmitter = std::make_unique<ir::Transmitter>(m_commandList);
        const ir::Result txRes = m_transmitter->init(m_config.outputDevice);
        if (txRes != ir::Result::SUCCESS)
        {
            LOG_ERROR("IRDevice transmitter init failed: {}", ir::to_string(txRes));
            shutdown();
            return irResultToCode(txRes);
        }
    }

    registerActionsAndQueries();
    m_state = DeviceState::Running;
    LOG_INFO("IRDevice ready: class={} input={} output={}", m_className, m_config.inputDevice, m_config.outputDevice);
    return 0;
}

void IRDevice::shutdown()
{
    if (m_state == DeviceState::Uninitialized)
        return;

    m_state = DeviceState::ShuttingDown;

    if (m_receiver)
    {
        m_receiver->shutdown();
        m_receiver.reset();
    }

    if (m_transmitter)
    {
        m_transmitter->shutdown();
        m_transmitter.reset();
    }

    m_commandList.reset();
    m_state = DeviceState::Stopped;
}

std::string_view IRDevice::getClass() const
{
    return m_className;
}

// ============================================================================
// Queryable
// ============================================================================

json IRDevice::query(std::string_view name, const json& params)
{
    (void)params;

    if (m_state != DeviceState::Running)
        return makeQueryError(-4);

    if (name == "has_data")
    {
        if (!m_receiver)
            return makeQueryError(-8, "receiver not available");

        return json{{"available", m_receiver->hasData()}};
    }

    if (name == "recv")
    {
        if (!m_receiver)
            return makeQueryError(-8, "receiver not available");

        if (!m_receiver->hasData())
            return json{{"available", false}};

        ir::Payload payload;
        std::string cmdName;
        const ir::Result res = m_receiver->recv(payload, cmdName);
        if (res == ir::Result::SUCCESS)
        {
            return json{
                {"available", true},
                {"command", cmdName.empty() ? json() : json(cmdName)},
                {"payload", payloadToJson(payload)},
            };
        }

        if (res == ir::Result::ERROR_CMD_NOT_FOUND)
        {
            return json{
                {"available", true},
                {"command", json()},
                {"payload", payloadToJson(payload)},
            };
        }

        return makeQueryError(irResultToCode(res), ir::to_string(res));
    }

    return makeQueryError(-8);
}

std::future<json> IRDevice::queryAsync(std::string_view name, const json& params, uint32_t timeout_ms)
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

int IRDevice::invoke(std::string_view name, const json& params)
{
    if (m_state != DeviceState::Running)
        return -4;

    if (name != "send")
        return -8;

    if (!m_transmitter)
        return -8;

    if (!params.contains("command") || !params["command"].is_string())
        return -8;

    const std::string command = params["command"].get<std::string>();
    ir::Result res = ir::Result::SUCCESS;

    if (params.contains("data"))
    {
        if (params["data"].is_number_unsigned())
            res = m_transmitter->send(command, static_cast<uint8_t>(params["data"].get<uint32_t>()));
        else if (params["data"].is_string())
            res = m_transmitter->send(command, params["data"].get<std::string>());
        else
            return -8;
    }
    else
    {
        res = m_transmitter->send(command);
    }

    return irResultToCode(res);
}

std::future<int> IRDevice::invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms)
{
    return std::async(std::launch::async, [this, name, params, timeout_ms]()
    {
        (void)timeout_ms;
        return invoke(name, params);
    });
}

void IRDevice::registerActionsAndQueries()
{
    m_queries.clear();
    m_actions.clear();
    m_queryMap.clear();
    m_actionMap.clear();

    if (!m_config.inputDevice.empty() || m_receiver)
    {
        m_queries.push_back({Query::Json, "has_data", "Check whether an IR frame is available", json::object()});
        m_queries.push_back({Query::Json, "recv", "Receive one IR frame", json::object()});
    }

    if (!m_config.outputDevice.empty() || m_transmitter)
    {
        m_actions.push_back({
            Action::Json,
            "send",
            "Send a registered IR command",
            json{
                {"type", "object"},
                {"properties", json{
                    {"command", json{{"type", "string"}}},
                    {"data", json{{"type", "string"}}},
                }},
                {"required", json::array({"command"})},
            },
        });
    }

    for (auto& query : m_queries)
        m_queryMap[query.name] = &query;

    for (auto& action : m_actions)
        m_actionMap[action.name] = &action;
}

int IRDevice::irResultToCode(ir::Result result) const
{
    switch (result)
    {
    case ir::Result::SUCCESS:
        return 0;
    case ir::Result::ERROR_CMD_NOT_FOUND:
        return -8;
    default:
        return -5;
    }
}

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
