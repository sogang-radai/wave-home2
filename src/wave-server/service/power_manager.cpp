#include "power_manager.h"

#include <algorithm>
#include <cmath>

#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "../app/app_state.h"
#include "../core/logger.h"
#include "util/time_util.h"
#include "../device/device.h"
#include "../device/device_wire_id.hpp"
#include "../device/platform/tuya_ep2h.h"
#include "../web/http/v1/power_store.h"

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

    std::vector<std::string> list_plug_external_ids(const dev::DeviceManager& devices)
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

    reloadMeteringFromDb();

    m_worker = std::thread([this]()
    {
        while (m_running.load(std::memory_order_acquire))
        {
            const auto tick_start = std::chrono::steady_clock::now();

            if (!AppState::get().no_devices
                && AppState::get().running.load(std::memory_order_acquire))
            {
                sampleAllPlugs();
                maybeEnqueueSchedules();
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

    const auto plug_ids = list_plug_external_ids(AppState::get().deviceManager);
    if (plug_ids.empty())
        return;

    for (const auto& plug_id : plug_ids)
    {
        if (!m_running.load(std::memory_order_acquire))
            return;

        samplePlug(plug_id);
    }

    if (m_running.load(std::memory_order_acquire))
        flushDueBuckets(nowMs());
}

void PowerManager::maybeEnqueueSchedules()
{
    auto client = AppState::get().db();
    if (!client)
        return;

    const auto now_rows = client->execSqlSync("SELECT datetime('now', 'localtime') AS now");
    if (now_rows.empty())
        return;
    const std::string now_full = now_rows[0]["now"].as<std::string>();
    if (now_full.size() < 13)
        return;

    const std::string current_hour = now_full.substr(0, 13); // "YYYY-MM-DD HH"
    const std::string today = now_full.substr(0, 10);

    std::string previous_hour;
    bool run_daily = false;
    bool run_yearly = false;
    {
        std::lock_guard lock(m_mutex);
        if (current_hour != m_lastHourlyReportHour)
        {
            previous_hour = m_lastHourlyReportHour;
            m_lastHourlyReportHour = current_hour;
        }

        if (today != m_lastDailyScheduleDate)
        {
            // First tick of a new calendar day — schedule for the day that just ended.
            if (!m_lastDailyScheduleDate.empty())
                run_daily = true;
            m_lastDailyScheduleDate = today;
        }
    }

    if (!previous_hour.empty())
        web::v1::PowerStore::enqueue_hourly_report(previous_hour + ":00:00");

    if (run_daily)
    {
        const auto yesterday_rows = client->execSqlSync("SELECT date(?, '-1 day') AS d", today);
        const std::string yesterday =
            yesterday_rows.empty() ? today : yesterday_rows[0]["d"].as<std::string>();

        web::v1::PowerStore::enqueue_daily_report(yesterday);

        const auto week_start_rows = client->execSqlSync("SELECT date(?, '-6 day') AS d", yesterday);
        const std::string week_start =
            week_start_rows.empty() ? yesterday : week_start_rows[0]["d"].as<std::string>();
        web::v1::PowerStore::enqueue_weekly_report(week_start);

        const auto month_start_rows = client->execSqlSync("SELECT date(?, '-29 day') AS d", yesterday);
        const std::string month_start =
            month_start_rows.empty() ? yesterday : month_start_rows[0]["d"].as<std::string>();
        web::v1::PowerStore::enqueue_monthly_report(month_start);

        web::v1::PowerStore::enqueue_power_insights_for_date(yesterday);

        // Sunday = strftime('%w') == '0'. "주 시작" — Sunday midnight batch for the prior 365 days.
        const auto wday_rows = client->execSqlSync("SELECT strftime('%w', ?) AS w", today);
        const std::string wday = wday_rows.empty() ? "" : wday_rows[0]["w"].as<std::string>();
        if (wday == "0")
        {
            const auto year_start_rows = client->execSqlSync("SELECT date(?, '-364 day') AS d", yesterday);
            const std::string year_start =
                year_start_rows.empty() ? yesterday : year_start_rows[0]["d"].as<std::string>();

            std::string week_key = today;
            {
                std::lock_guard lock(m_mutex);
                if (m_lastYearlyScheduleWeek != week_key)
                {
                    m_lastYearlyScheduleWeek = week_key;
                    run_yearly = true;
                }
            }
            if (run_yearly)
                web::v1::PowerStore::enqueue_yearly_report(year_start);
        }
    }
}

void PowerManager::setMeteringEnabled(const std::string& external_id, bool enabled)
{
    std::lock_guard lock(m_mutex);
    m_meteringEnabled[external_id] = enabled;
    if (!enabled)
    {
        m_buckets.erase(external_id);
    }
}

bool PowerManager::isMeteringEnabled(const std::string& external_id) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_meteringEnabled.find(external_id);
    if (it == m_meteringEnabled.end())
        return true;
    return it->second;
}

void PowerManager::reloadMeteringFromDb()
{
    auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        std::unordered_map<int64_t, bool> by_db_id;
        const auto rows = client->execSqlSync(
            R"SQL(
SELECT d.id AS db_id,
       COALESCE(json_extract(d.settings_json, '$.metering'), 1) AS metering
FROM device d
WHERE d.archived = 0 AND d.class = 'tuya_ep2h'
)SQL");
        for (const auto& row : rows)
            by_db_id[row["db_id"].as<int64_t>()] = row["metering"].as<int>() != 0;

        std::unordered_map<std::string, bool> next;
        for (const auto& entry : AppState::get().deviceManager.manifestEntries())
        {
            if (entry.config.value("class", "") != "tuya_ep2h")
                continue;
            const std::string wire = entry.config.value("id", "");
            if (wire.empty())
                continue;
            bool metering = true;
            if (const auto db_id = dev::dbIdForWireId(client, wire))
            {
                const auto it = by_db_id.find(*db_id);
                if (it != by_db_id.end())
                    metering = it->second;
            }
            next[wire] = metering;
        }

        std::lock_guard lock(m_mutex);
        m_meteringEnabled = std::move(next);
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("PowerManager: reload metering failed: {}", e.what());
    }
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
        trim_history(history);
    }

    if (reading.connected && isMeteringEnabled(external_id))
        accumulateEnergy(external_id, reading);
}

std::string PowerManager::bucket_time_start(int64_t ts_ms)
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
    if (!isMeteringEnabled(external_id))
        return;

    const std::string bucket_key = bucket_time_start(reading.ts_ms);
    std::optional<EnergyBucket> due_bucket;
    std::string due_key;

    {
        std::lock_guard lock(m_mutex);
        auto& bucket = m_buckets[external_id];
        if (bucket.time_start.empty())
            bucket.time_start = bucket_key;

        if (bucket.time_start != bucket_key)
        {
            due_key = external_id;
            due_bucket = bucket;
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

    if (due_bucket)
    {
        const auto db_id = resolveDbDeviceId(due_key);
        flushBucket(due_key, *due_bucket, db_id);
    }
}

void PowerManager::flushDueBuckets(int64_t now_ms)
{
    const std::string current_bucket = bucket_time_start(now_ms);
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
        if (!isMeteringEnabled(key))
            continue;

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
        const auto db_id = dev::dbIdForWireId(client, external_id);
        if (!db_id)
            return std::nullopt;

        const int64_t id = *db_id;
        std::lock_guard lock(m_mutex);
        m_db_device_cache[external_id] = id;
        return id;
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("PowerManager: device lookup failed for {}: {}", external_id, e.what());
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
        WLOG_WARN("PowerManager: power_energy write failed: {}", e.what());
    }
}

void PowerManager::trim_history(std::deque<PowerSample>& history)
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
    trim_history(merged);
    return merged;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
