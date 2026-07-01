#pragma once

#include <future>
#include <memory>
#include <string>

#include "../device.h"
#include "../protocol/ir_remote.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

class IRDevice :
    public Device,
    public Queryable,
    public Actionable
{
public:
    struct Config
    {
        std::string transport;
        std::string inputDevice;
        std::string outputDevice;
        std::string commandListPath;
    };

    IRDevice();
    ~IRDevice() override;

    const Config& getConfig() const;

    // Device
    int init(const json& config) override;
    void shutdown() override;

    std::string_view getClass() const override;

    // Queryable
    json query(std::string_view name, const json& params) override;
    std::future<json> queryAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    // Actionable
    int invoke(std::string_view name, const json& params) override;
    std::future<int> invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

private:
    void registerActionsAndQueries();
    int irResultToCode(ir::Result result) const;

    Config m_config;
    std::string m_className;
    std::shared_ptr<ir::CommandList> m_commandList;
    std::unique_ptr<ir::Receiver> m_receiver;
    std::unique_ptr<ir::Transmitter> m_transmitter;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
