#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "../device.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

class TuyaEP2H :
    public Device,
    public Queryable,
    public Actionable
{
public:
    struct Config
    {
        std::string host;
        std::string deviceId;
        std::string localKey;
        std::string version;
    };

    TuyaEP2H();
    ~TuyaEP2H() override;

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
    struct Impl;

    void registerActionsAndQueries();
    json fetchDatapoints();
    int setSwitch(bool on);

    std::unique_ptr<Impl> m_impl;
    Config m_config;
    mutable std::mutex m_mutex;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
