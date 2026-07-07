#include "power_store.h"

#include <chrono>
#include <cmath>

#include "../../../app/app_state.h"
#include "../../../service/power_manager.h"
#include "iot_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {
namespace
{
    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    const dev::DeviceManifestEntry* findManifestEntry(
        const dev::DeviceManager& devices,
        const std::string& external_id)
    {
        for (const auto& entry : devices.manifestEntries())
        {
            if (entry.config.value("id", "") == external_id)
                return &entry;
        }
        return nullptr;
    }

    std::string roomNameForEntry(const dev::DeviceManifestEntry& entry, const dev::DeviceManager& devices)
    {
        const auto room_id_text = entry.config.value("room_id", "");
        if (room_id_text.empty())
            return "미지정";

        const auto room_id = dev::parseRoomID(room_id_text);
        if (const auto* room = devices.findRoom(room_id))
            return room->name;
        return "미지정";
    }
}

PowerStore::PowerStore(IotStore& iot) :
    m_iot(iot)
{
}

int PowerStore::stepSecondsForRange(const std::string& range)
{
    if (range == "min10")
        return 10;
    if (range == "min30")
        return 30;
    if (range == "hour")
        return 60;
    return 1;
}

int PowerStore::pointCountForRange(const std::string& range)
{
    (void)range;
    return 60;
}

std::string PowerStore::formatAgoLabel(int seconds_ago)
{
    if (seconds_ago <= 0)
        return "지금";
    if (seconds_ago < 60)
        return "-" + std::to_string(seconds_ago) + "s";
    return "-" + std::to_string((seconds_ago + 59) / 60) + "분";
}

Json::Value PowerStore::listPlugs()
{
    auto& power = ws::service::PowerManager::get();
    const auto& devices = AppState::get().deviceManager;

    double total_w = 0;
    double total_v = 0;
    double total_a_ma = 0;
    int switch_on_count = 0;
    int connected_count = 0;
    int plug_count = 0;

    Json::Value aggregate;
    aggregate["id"] = "all";
    aggregate["name"] = "전체";
    aggregate["room"] = "집합";
    aggregate["summary"] = "계측 플러그 합산";
    aggregate["connected"] = true;
    aggregate["connectionStatus"] = "online";
    aggregate["powerW"] = 0.0;
    aggregate["voltageV"] = 0.0;
    aggregate["currentMa"] = 0;
    aggregate["switchOn"] = false;
    aggregate["hourlyCostWon"] = 0.0;
    aggregate["trend"] = Json::Value(Json::objectValue);

    Json::Value body(Json::arrayValue);
    body.append(aggregate);

    for (const auto& plug_id : m_iot.listPlugIds())
    {
        const auto* entry = findManifestEntry(devices, plug_id);
        const auto reading = power.getReading(plug_id);

        std::string room_name = "미지정";
        std::string name = entry ? entry->config.value("name", plug_id) : plug_id;
        std::string summary = entry ? entry->config.value("description", "") : "";
        if (entry)
            room_name = roomNameForEntry(*entry, devices);

        std::string connection_status = "offline";
        if (entry)
            connection_status = m_iot.connectionStatusForEntry(*entry);
        else if (reading && reading->connected)
            connection_status = "online";

        const bool online = connection_status == "online";

        Json::Value plug;
        plug["id"] = plug_id;
        plug["name"] = name;
        plug["room"] = room_name;
        plug["summary"] = summary;
        plug["connected"] = online;
        plug["connectionStatus"] = connection_status;
        plug["switchOn"] = online && reading && reading->switch_on;
        plug["trend"] = Json::Value(Json::objectValue);

        if (online && reading)
        {
            plug["powerW"] = reading->power_w;
            plug["voltageV"] = reading->voltage_v;
            plug["currentMa"] = static_cast<Json::Int64>(static_cast<int64_t>(std::lround(reading->current_ma)));
            plug["hourlyCostWon"] = std::round(reading->power_w * 0.25 * 10.0) / 10.0;
            plug["sampledAtMs"] = static_cast<Json::Int64>(reading->ts_ms);

            total_w += reading->power_w;
            total_v += reading->voltage_v;
            total_a_ma += reading->current_ma;
            switch_on_count += reading->switch_on ? 1 : 0;
            ++connected_count;
        }
        else
        {
            plug["powerW"] = Json::Value();
            plug["voltageV"] = Json::Value();
            plug["currentMa"] = Json::Value();
            plug["hourlyCostWon"] = Json::Value();
        }

        body.append(plug);
        ++plug_count;
    }

    if (body.size() > 0 && body[0].isObject())
    {
        body[0]["powerW"] = total_w;
        body[0]["voltageV"] = connected_count > 0 ? total_v / connected_count : 0.0;
        body[0]["currentMa"] = static_cast<Json::Int64>(static_cast<int64_t>(std::lround(total_a_ma)));
        body[0]["switchOn"] = switch_on_count > 0;
        body[0]["hourlyCostWon"] = std::round(total_w * 0.25 * 10.0) / 10.0;
        body[0]["connected"] = connected_count > 0;
        body[0]["connectionStatus"] = connected_count > 0 ? "online" : "offline";
    }

    return body;
}

Json::Value PowerStore::comboTrend(const std::string& device_id, const std::string& range, const std::string& metric)
{
    (void)metric;
    auto& power = ws::service::PowerManager::get();

    const int step_seconds = stepSecondsForRange(range);
    const int points = pointCountForRange(range);
    const int64_t now = nowMs();

    const auto history = device_id == "all" ? power.getMergedHistory() : power.getHistory(device_id);

    Json::Value series(Json::arrayValue);
    for (int i = 0; i < points; ++i)
    {
        const int seconds_ago = (points - 1 - i) * step_seconds;
        const int64_t target_ts = now - static_cast<int64_t>(seconds_ago) * 1000;

        ws::service::PowerSample chosen{};
        bool found = false;
        for (auto it = history.rbegin(); it != history.rend(); ++it)
        {
            if (it->ts_ms <= target_ts)
            {
                chosen = *it;
                found = true;
                break;
            }
        }
        if (!found && !history.empty())
            chosen = history.back();

        Json::Value point;
        point["label"] = formatAgoLabel(seconds_ago);
        point["value"] = found || !history.empty() ? chosen.w : 0.0;
        point["wh"] = point["value"].asDouble() * (step_seconds / 3600.0);
        point["v"] = found || !history.empty() ? chosen.v : 0.0;
        point["a"] = found || !history.empty() ? chosen.a : 0.0;
        series.append(point);
    }
    return series;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
