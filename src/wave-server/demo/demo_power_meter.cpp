#include "demo_power_meter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>

#include "../device/device_wire_id.hpp"

WAVE_NAMESPACE_BEGIN

namespace
{
    constexpr int64_t kJitterIntervalMs = 5000;
    constexpr size_t kMaxHistory = 3600; // ~1h at 1s

    int stepSecondsForRange(const std::string& range)
    {
        if (range == "min10")
            return 10;
        if (range == "min30")
            return 30;
        if (range == "hour")
            return 60;
        return 1;
    }

    std::string formatAgoLabel(int seconds_ago)
    {
        if (seconds_ago <= 0)
            return "지금";
        if (seconds_ago < 60)
            return "-" + std::to_string(seconds_ago) + "s";
        return "-" + std::to_string((seconds_ago + 59) / 60) + "분";
    }

    double ratedPowerForDevice(const std::string& device_id)
    {
        if (device_id == "0000000000000006")
            return 20.0;
        if (device_id == "0000000000000007")
            return 100.0;
        if (device_id == "0000000000000008")
            return 600.0;
        if (device_id == "0000000000000009")
            return 2400.0;
        return 0.0;
    }
}

DemoPowerMeter& DemoPowerMeter::instance()
{
    static DemoPowerMeter meter;
    return meter;
}

int64_t DemoPowerMeter::nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double DemoPowerMeter::jitterFactor()
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(-0.045, 0.045);
    return 1.0 + dist(rng);
}

void DemoPowerMeter::pushHistory(PlugSlot& slot, const DemoPowerReading& reading)
{
    Sample sample;
    sample.ts_ms = reading.ts_ms;
    sample.w = reading.switch_on ? reading.power_w : 0.0;
    sample.v = reading.voltage_v;
    sample.a = reading.switch_on ? reading.current_ma / 1000.0 : 0.0;

    // Keep at most one sample per second so combo charts stay dense/smooth.
    if (!slot.history.empty() && slot.history.back().ts_ms / 1000 == sample.ts_ms / 1000)
        slot.history.back() = sample;
    else
        slot.history.push_back(sample);

    while (slot.history.size() > kMaxHistory)
        slot.history.pop_front();
}

DemoPowerReading DemoPowerMeter::refreshLocked(
    PlugSlot& slot,
    const bool switch_on,
    const double rated_w,
    const double voltage_v,
    const bool force)
{
    const int64_t now = nowMs();
    slot.reading.switch_on = switch_on;
    slot.reading.rated_w = rated_w;
    slot.reading.voltage_v = voltage_v > 0 ? voltage_v : 235.0;

    const bool due = force || slot.last_jitter_ms == 0 ||
        (now - slot.last_jitter_ms) >= kJitterIntervalMs;

    if (!switch_on)
    {
        slot.reading.power_w = 0.0;
        slot.reading.current_ma = 0.0;
        slot.reading.ts_ms = now;
        if (due)
            slot.last_jitter_ms = now;
        pushHistory(slot, slot.reading);
        return slot.reading;
    }

    if (due)
    {
        const double factor = jitterFactor();
        const double voltage = slot.reading.voltage_v * (1.0 + (factor - 1.0) * 0.15);
        const double power = std::max(0.0, rated_w * factor);
        slot.reading.voltage_v = std::round(voltage * 10.0) / 10.0;
        slot.reading.power_w = std::round(power * 10.0) / 10.0;
        slot.reading.current_ma =
            slot.reading.voltage_v > 0
                ? std::round((slot.reading.power_w / slot.reading.voltage_v) * 1000.0 * 10.0) / 10.0
                : 0.0;
        slot.reading.ts_ms = now;
        slot.last_jitter_ms = now;
    }
    else
    {
        if (slot.reading.ts_ms == 0)
        {
            slot.reading.power_w = rated_w;
            slot.reading.current_ma =
                slot.reading.voltage_v > 0
                    ? (rated_w / slot.reading.voltage_v) * 1000.0
                    : 0.0;
        }
        slot.reading.ts_ms = now;
    }

    // Always record the held reading so 1s charts do not drop to 0 between jitters.
    pushHistory(slot, slot.reading);
    return slot.reading;
}

void DemoPowerMeter::syncPlug(
    const std::string& runtime_id,
    const std::string& device_id,
    const bool switch_on,
    const double rated_w,
    const double voltage_v)
{
    std::lock_guard lock(m_mutex);
    auto& slot = m_sessions[runtime_id].plugs[device_id];
    refreshLocked(slot, switch_on, rated_w, voltage_v, true);
}

DemoPowerReading DemoPowerMeter::samplePlug(
    const std::string& runtime_id,
    const std::string& device_id,
    const bool switch_on,
    const double rated_w,
    const double voltage_v)
{
    std::lock_guard lock(m_mutex);
    auto& slot = m_sessions[runtime_id].plugs[device_id];
    return refreshLocked(slot, switch_on, rated_w, voltage_v, false);
}

Json::Value DemoPowerMeter::listPlugs(
    const std::string& runtime_id,
    const drogon::orm::DbClientPtr& client)
{
    Json::Value body(Json::arrayValue);
    Json::Value aggregate;
    aggregate["id"] = "all";
    aggregate["name"] = "전체";
    aggregate["room"] = "집합";
    aggregate["summary"] = "계측 플러그 합산";
    aggregate["connected"] = true;
    aggregate["connectionStatus"] = "online";
    aggregate["powerW"] = 0.0;
    aggregate["voltageV"] = 235.0;
    aggregate["currentMa"] = 0;
    aggregate["switchOn"] = false;
    aggregate["hourlyCostWon"] = 0.0;
    aggregate["trend"] = Json::Value(Json::objectValue);
    body.append(aggregate);

    if (!client)
        return body;

    auto rows = client->execSqlSync(
        R"SQL(
SELECT d.id, d.name, d.description, d.class,
       r.name AS room_name
FROM device d
LEFT JOIN device_room_map drm ON drm.device_id = d.id
LEFT JOIN room r ON r.id = drm.room_id
WHERE d.archived = 0 AND d.class = 'tuya_ep2h'
ORDER BY d.id
)SQL");

    double total_w = 0;
    double total_v = 0;
    double total_a = 0;
    int on_count = 0;
    int count = 0;

    for (const auto& row : rows)
    {
        const auto wire_id = dev::wireIdForDbRow(row["id"].as<int64_t>(), row["name"].as<std::string>());
        const double rated = ratedPowerForDevice(wire_id);
        const bool default_on = wire_id != "0000000000000009";

        DemoPowerReading reading;
        {
            std::lock_guard lock(m_mutex);
            auto& slot = m_sessions[runtime_id].plugs[wire_id];
            const bool switch_on = slot.reading.ts_ms > 0 ? slot.reading.switch_on : default_on;
            const double rated_w = slot.reading.rated_w > 0 ? slot.reading.rated_w : rated;
            reading = refreshLocked(slot, switch_on, rated_w, 235.0, false);
        }

        Json::Value plug;
        plug["id"] = wire_id;
        plug["name"] = row["name"].as<std::string>();
        plug["room"] = row["room_name"].isNull() ? "미지정" : row["room_name"].as<std::string>();
        plug["summary"] = row["description"].as<std::string>();
        plug["connected"] = true;
        plug["connectionStatus"] = "online";
        plug["switchOn"] = reading.switch_on;
        plug["powerW"] = reading.power_w;
        plug["voltageV"] = reading.voltage_v;
        plug["currentMa"] = static_cast<Json::Int64>(static_cast<int64_t>(std::lround(reading.current_ma)));
        plug["hourlyCostWon"] = std::round(reading.power_w * 0.11 * 10.0) / 10.0;
        plug["sampledAtMs"] = static_cast<Json::Int64>(reading.ts_ms);
        plug["trend"] = Json::Value(Json::objectValue);
        body.append(plug);

        total_w += reading.power_w;
        total_v += reading.voltage_v;
        total_a += reading.current_ma;
        on_count += reading.switch_on ? 1 : 0;
        ++count;
    }

    if (count > 0)
    {
        body[0]["powerW"] = total_w;
        body[0]["voltageV"] = total_v / count;
        body[0]["currentMa"] = static_cast<Json::Int64>(static_cast<int64_t>(std::lround(total_a)));
        body[0]["switchOn"] = on_count > 0;
        body[0]["hourlyCostWon"] = std::round(total_w * 0.11 * 10.0) / 10.0;
    }
    return body;
}

Json::Value DemoPowerMeter::comboTrend(
    const std::string& runtime_id,
    const std::string& device_id,
    const std::string& range)
{
    const int step_seconds = stepSecondsForRange(range);
    constexpr int points = 60;
    const int64_t now = nowMs();

    std::lock_guard lock(m_mutex);
    auto session_it = m_sessions.find(runtime_id);

    auto sampleAt = [](const std::deque<Sample>& history, const int64_t target_ts, Sample& out) -> bool
    {
        for (auto it = history.rbegin(); it != history.rend(); ++it)
        {
            if (it->ts_ms <= target_ts)
            {
                out = *it;
                return true;
            }
        }
        return false;
    };

    Json::Value series(Json::arrayValue);
    for (int i = 0; i < points; ++i)
    {
        const int seconds_ago = (points - 1 - i) * step_seconds;
        const int64_t target_ts = now - static_cast<int64_t>(seconds_ago) * 1000;

        Sample chosen{};
        bool found = false;

        if (session_it != m_sessions.end())
        {
            if (device_id == "all")
            {
                double sum_w = 0;
                double sum_a = 0;
                double last_v = 0;
                int contributors = 0;
                for (const auto& [id, slot] : session_it->second.plugs)
                {
                    (void)id;
                    Sample plug_sample{};
                    if (!sampleAt(slot.history, target_ts, plug_sample))
                        continue;
                    sum_w += plug_sample.w;
                    sum_a += plug_sample.a;
                    last_v = plug_sample.v;
                    ++contributors;
                }
                if (contributors > 0)
                {
                    chosen.w = sum_w;
                    chosen.a = sum_a;
                    chosen.v = last_v;
                    found = true;
                }
            }
            else
            {
                auto plug_it = session_it->second.plugs.find(device_id);
                if (plug_it != session_it->second.plugs.end())
                    found = sampleAt(plug_it->second.history, target_ts, chosen);
            }
        }

        Json::Value point;
        point["label"] = formatAgoLabel(seconds_ago);
        point["value"] = found ? chosen.w : 0.0;
        point["wh"] = point["value"].asDouble() * (step_seconds / 3600.0);
        point["v"] = found ? chosen.v : 0.0;
        point["a"] = found ? chosen.a : 0.0;
        series.append(point);
    }
    return series;
}

WAVE_NAMESPACE_END
