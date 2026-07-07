#include "power_manager.h"

#include <algorithm>
#include <cmath>
#include <future>

#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "../app/app_state.h"
#include "../core/logger.h"
#include "../core/time_util.h"
#include "../device/device.h"
#include "../device/platform/tuya_ep2h.h"

#define SERVICE_NAMESPACE_BEGIN namespace service {
#define SERVICE_NAMESPACE_END }

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    bool isQueryError(const nlohmann::json& value)
    {
        return value.is_object() && value.contains("code") && value["code"].is_number_integer()
            && value["code"].get<int>() < 0;
    }

    std::vector<std::string> listPlugExternalIds(const dev::DeviceManager& devices)
    {
        std::vector<std::string> ids;
        for (const auto& entry : devices.manifestEntries())
        {
            if (entry.config.value("class", "") != "tuya_ep2h")
                continue;
            ids.push_back(entry.config.value("id", ""));
        }
        return ids;
    }
}

PowerManager& PowerManager::get()
{
    static PowerManager instance;
    return instance;
}

PowerManager::~PowerManager()
{
    stop();
}

void PowerManager::start()
{
    if (m_running.exchange(true))
        return;

    m_worker = std::thread([this]()
    {
        while (m_running.load(std::memory_order_acquire))
        {
            const auto tick_start = std::chrono::steady_clock::now();

            if (!AppState::get().no_devices
                && AppState::get().running.load(std::memory_order_acquire))
            {
                sampleAllPlugs();
            }

            const auto elapsed = std::chrono::steady_clock::now() - tick_start;
            const auto sleep_for = std::chrono::milliseconds(1000) - elapsed;
            if (sleep_for > std::chrono::milliseconds(0))
            {
                std::unique_lock lock(m_stopMutex);
                m_stopCv.wait_for(lock, sleep_for, [this]()
                {
                    return !m_running.load(std::memory_order_acquire);
                });
            }
        }
    });
}

void PowerManager::stop()
{
    if (!m_running.exchange(false))
        return;

    m_stopCv.notify_all();

    if (m_worker.joinable())
        m_worker.join();
}

void PowerManager::sampleNow()
{
    sampleAllPlugs();
}

void PowerManager::samplePlugNow(const std::string& external_id)
{
    samplePlug(external_id);
    flushDueBuckets(nowMs());
}

void PowerManager::sampleAllPlugs()
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    const auto plug_ids = listPlugExternalIds(AppState::get().deviceManager);
    if (plug_ids.empty())
        return;

    std::vector<std::future<void>> jobs;
    jobs.reserve(plug_ids.size());
    for (const auto& plug_id : plug_ids)
    {
        if (!m_running.load(std::memory_order_acquire))
            return;

        jobs.push_back(std::async(std::launch::async, [this, plug_id]()
        {
            samplePlug(plug_id);
        }));
    }

    for (auto& job : jobs)
    {
        if (!m_running.load(std::memory_order_acquire))
            break;

        if (job.wait_for(std::chrono::milliseconds(800)) == std::future_status::ready)
            job.get();
    }

    if (m_running.load(std::memory_order_acquire))
        flushDueBuckets(nowMs());
}

void PowerManager::samplePlug(const std::string& external_id)
{
    if (!AppState::get().running.load(std::memory_order_acquire))
        return;

    PlugReading reading;
    reading.ts_ms = nowMs();

    auto* device = AppState::get().deviceManager.findDevice(dev::parseDeviceID(external_id));
    if (!device || !device->isEnabled() || device->getState() != dev::DeviceState::Running)
    {
        storeSample(external_id, reading);
        return;
    }

    auto* plug = dynamic_cast<dev::TuyaEP2H*>(device);
    if (!plug)
    {
        storeSample(external_id, reading);
        return;
    }

    const auto raw = plug->readStatus(false);
    if (isQueryError(raw))
    {
        storeSample(external_id, reading);
        return;
    }

    reading.connected = true;
    reading.switch_on = raw.value("switch", false);
    reading.power_w = raw.value("power_w", 0.0);
    reading.voltage_v = raw.value("voltage_v", 0.0);
    reading.current_ma = raw.value("current_ma", 0.0);
    storeSample(external_id, reading);
}

void PowerManager::storeSample(const std::string& external_id, const PlugReading& reading)
{
    PowerSample sample;
    sample.ts_ms = reading.ts_ms;
    sample.w = reading.connected && reading.switch_on ? reading.power_w : 0.0;
    sample.v = reading.connected ? reading.voltage_v : 0.0;
    sample.a = reading.connected ? reading.current_ma / 1000.0 : 0.0;

    {
        std::lock_guard lock(m_mutex);
        m_latest[external_id] = reading;

        auto& history = m_history[external_id];
        if (history.empty() || history.back().ts_ms != sample.ts_ms)
            history.push_back(sample);
        trimHistory(history);
    }

    if (reading.connected)
        accumulateEnergy(external_id, reading);
}

std::string PowerManager::bucketTimeStart(int64_t ts_ms)
{
    const auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms));
    const auto local = formatTimestamp(tp);
    if (local.size() < 16)
        return local;

    const int minute = std::stoi(local.substr(14, 2));
    const int bucket_minute = (minute / 5) * 5;
    char buffer[32];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s:%02d:00",
        local.substr(0, 13).c_str(),
        bucket_minute);
    return std::string(buffer);
}

void PowerManager::accumulateEnergy(const std::string& external_id, const PlugReading& reading)
{
    const std::string bucket_key = bucketTimeStart(reading.ts_ms);

    std::lock_guard lock(m_mutex);
    auto& bucket = m_buckets[external_id];
    if (bucket.time_start.empty())
        bucket.time_start = bucket_key;

    if (bucket.time_start != bucket_key)
    {
        flushBucket(external_id, bucket, resolveDbDeviceId(external_id));
        bucket = EnergyBucket{};
        bucket.time_start = bucket_key;
    }

    if (bucket.last_ts_ms > 0 && reading.ts_ms > bucket.last_ts_ms)
    {
        const double delta_h = static_cast<double>(reading.ts_ms - bucket.last_ts_ms) / 3600000.0;
        const double avg_w = (bucket.last_power_w + reading.power_w) * 0.5;
        bucket.energy_wh += avg_w * delta_h;
    }

    bucket.last_ts_ms = reading.ts_ms;
    bucket.last_power_w = reading.switch_on ? reading.power_w : 0.0;
    ++bucket.sample_count;
}

void PowerManager::flushDueBuckets(int64_t now_ms)
{
    const std::string current_bucket = bucketTimeStart(now_ms);
    std::vector<std::pair<std::string, EnergyBucket>> due;

    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_buckets.begin(); it != m_buckets.end();)
        {
            if (!it->second.time_start.empty() && it->second.time_start != current_bucket
                && it->second.sample_count > 0)
            {
                due.emplace_back(it->first, it->second);
                it = m_buckets.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    double agg_energy = 0;
    int agg_samples = 0;
    std::string agg_time_start;

    for (auto& [key, bucket] : due)
    {
        const auto db_id = resolveDbDeviceId(key);
        flushBucket(key, bucket, db_id);

        agg_energy += bucket.energy_wh;
        agg_samples += bucket.sample_count;
        if (agg_time_start.empty())
            agg_time_start = bucket.time_start;
    }

    if (!due.empty() && !agg_time_start.empty())
    {
        EnergyBucket aggregate;
        aggregate.time_start = agg_time_start;
        aggregate.energy_wh = agg_energy;
        aggregate.sample_count = agg_samples;
        flushBucket("__aggregate__", aggregate, std::nullopt);
    }
}

std::optional<int64_t> PowerManager::resolveDbDeviceId(const std::string& external_id)
{
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_db_device_cache.find(external_id);
        if (it != m_db_device_cache.end())
            return it->second;
    }

    auto client = AppState::get().db();
    if (!client)
        return std::nullopt;

    try
    {
        auto rows = client->execSqlSync(
            "SELECT id FROM device WHERE external_id = ? AND archived = 0 LIMIT 1",
            external_id);
        if (rows.empty())
            return std::nullopt;

        const int64_t id = rows[0]["id"].as<int64_t>();
        std::lock_guard lock(m_mutex);
        m_db_device_cache[external_id] = id;
        return id;
    }
    catch (const std::exception& e)
    {
        LOG_WARN("PowerManager: device lookup failed for {}: {}", external_id, e.what());
        return std::nullopt;
    }
}

void PowerManager::flushBucket(
    const std::string& bucket_key,
    EnergyBucket& bucket,
    std::optional<int64_t> db_device_id)
{
    (void)bucket_key;
    if (bucket.sample_count <= 0 || bucket.time_start.empty())
        return;

    auto client = AppState::get().db();
    if (!client)
        return;

    const double coverage = std::min(1.0, static_cast<double>(bucket.sample_count) / 300.0);
    const double energy = std::round(bucket.energy_wh * 10000.0) / 10000.0;
    const double cov = std::round(coverage * 10000.0) / 10000.0;

    try
    {
        if (db_device_id)
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO power_energy (device_id, granularity, time_start, energy_wh, coverage, sample_count)
VALUES (?, '5m', ?, ?, ?, ?)
ON CONFLICT DO UPDATE SET
    energy_wh = excluded.energy_wh,
    coverage = excluded.coverage,
    sample_count = excluded.sample_count
)SQL",
                *db_device_id,
                bucket.time_start,
                energy,
                cov,
                bucket.sample_count);
        }
        else
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO power_energy (device_id, granularity, time_start, energy_wh, coverage, sample_count)
VALUES (NULL, '5m', ?, ?, ?, ?)
ON CONFLICT DO UPDATE SET
    energy_wh = excluded.energy_wh,
    coverage = excluded.coverage,
    sample_count = excluded.sample_count
)SQL",
                bucket.time_start,
                energy,
                cov,
                bucket.sample_count);
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("PowerManager: power_energy write failed: {}", e.what());
    }
}

void PowerManager::trimHistory(std::deque<PowerSample>& history)
{
    while (history.size() > 3700)
        history.pop_front();
}

std::optional<PlugReading> PowerManager::getReading(const std::string& external_id) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_latest.find(external_id);
    if (it == m_latest.end())
        return std::nullopt;
    return it->second;
}

std::deque<PowerSample> PowerManager::getHistory(const std::string& external_id) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_history.find(external_id);
    if (it == m_history.end())
        return {};
    return it->second;
}

std::deque<PowerSample> PowerManager::getMergedHistory() const
{
    std::deque<PowerSample> merged;
    std::lock_guard lock(m_mutex);
    for (const auto& [id, history] : m_history)
    {
        (void)id;
        for (const auto& sample : history)
        {
            auto it = std::find_if(merged.begin(), merged.end(), [&](const PowerSample& s)
            {
                return s.ts_ms == sample.ts_ms;
            });
            if (it == merged.end())
                merged.push_back(sample);
            else
            {
                it->w += sample.w;
                it->a += sample.a;
                it->v = (it->v + sample.v) / 2.0;
            }
        }
    }
    std::sort(merged.begin(), merged.end(), [](const PowerSample& a, const PowerSample& b)
    {
        return a.ts_ms < b.ts_ms;
    });
    trimHistory(merged);
    return merged;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
