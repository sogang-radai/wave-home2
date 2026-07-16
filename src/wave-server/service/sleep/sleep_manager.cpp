#include "sleep_manager.h"

#include <fstream>
#include <map>

#include <drogon/drogon.h>

#include "../../app/app_state.h"
#include "../../core/logger.h"
#include "util/time_util.h"
#include "../../device/device.h"
#include "../../device/device_wire_id.hpp"
#include "../../device/device_manager.h"
#include "../../device/interface/radar.h"
#include "../../device/platform/radai_ws.h"
#include "../../device/platform/srs_r4sn.h"
#include "../agent_client.h"
#include "../insight_generator.h"

#include <future>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    dev::Device* find_device_by_external_id(const std::string& external_id)
    {
        const auto id = dev::parseDeviceID(external_id);
        if (id == 0)
            return nullptr;
        return AppState::get().deviceManager.findDevice(id);
    }

    json row_to_camel_stat(
        const drogon::orm::Result& rows,
        size_t index)
    {
        const auto& row = rows[index];
        json out;
        out["id"] = row["id"].as<int64_t>();
        out["userId"] = row["user_id"].as<int64_t>();
        out["roomId"] = row["room_id"].as<int64_t>();
        if (!row["session_id"].isNull())
            out["sessionId"] = row["session_id"].as<int64_t>();
        out["granularity"] = row["granularity"].as<std::string>();
        out["timeStart"] = row["time_start"].as<std::string>();
        if (!row["time_end"].isNull())
            out["timeEnd"] = row["time_end"].as<std::string>();
        out["coverage"] = row["coverage"].as<double>();
        if (!row["status_ratio"].isNull())
            out["statusRatio"] = json::parse(row["status_ratio"].as<std::string>());
        if (!row["toss_mean"].isNull())
            out["tossMean"] = row["toss_mean"].as<double>();
        if (!row["toss_max"].isNull())
            out["tossMax"] = row["toss_max"].as<double>();
        if (!row["toss_p90"].isNull())
            out["tossP90"] = row["toss_p90"].as<double>();
        if (!row["toss_events"].isNull())
            out["tossEvents"] = row["toss_events"].as<int64_t>();
        if (!row["toss_ratio"].isNull())
            out["tossRatio"] = json::parse(row["toss_ratio"].as<std::string>());
        if (!row["summary_text"].isNull())
            out["summaryText"] = row["summary_text"].as<std::string>();
        if (!row["env_temp"].isNull())
            out["envTemp"] = row["env_temp"].as<double>();
        if (!row["env_lux"].isNull())
            out["envLux"] = row["env_lux"].as<double>();
        return out;
    }

    json row_to_camel_session(const drogon::orm::Result& rows, size_t index)
    {
        const auto& row = rows[index];
        json out;
        out["id"] = row["id"].as<int64_t>();
        out["userId"] = row["user_id"].as<int64_t>();
        out["roomId"] = row["room_id"].as<int64_t>();
        out["radarId"] = row["radar_id"].as<int64_t>();
        if (!row["station_id"].isNull())
            out["stationId"] = row["station_id"].as<int64_t>();
        out["nightDate"] = row["night_date"].as<std::string>();
        if (!row["onset"].isNull())
            out["onset"] = row["onset"].as<std::string>();
        if (!row["final_wake"].isNull())
            out["finalWake"] = row["final_wake"].as<std::string>();
        if (!row["time_in_bed_s"].isNull())
            out["timeInBedS"] = row["time_in_bed_s"].as<int64_t>();
        if (!row["asleep_total_s"].isNull())
            out["asleepTotalS"] = row["asleep_total_s"].as<int64_t>();
        if (!row["efficiency"].isNull())
            out["efficiency"] = row["efficiency"].as<double>();
        if (!row["toss_events"].isNull())
            out["tossEvents"] = row["toss_events"].as<int64_t>();
        return out;
    }

    bool is_sleep_radar_manifest_entry(const dev::DeviceManifestEntry& entry)
    {
        if (entry.config.value("class", "") != "srs_r4sn")
            return false;
        if (!entry.config.contains("settings") || !entry.config["settings"].is_object())
            return false;
        return entry.config["settings"].value("sleep", false);
    }
}

SleepManager& SleepManager::get()
{
    static SleepManager instance;
    return instance;
}

SleepManager::~SleepManager()
{
    stop();
}

void SleepManager::start()
{
    if (m_running.exchange(true))
        return;

    reconcile();

    m_worker = std::thread([this]()
    {
        while (m_running.load(std::memory_order_acquire))
        {
            const auto tick_start = std::chrono::steady_clock::now();

            if (!AppState::get().no_devices
                && AppState::get().running.load(std::memory_order_acquire))
            {
                std::lock_guard lock(m_mutex);
                for (auto& [room_id, runtime] : m_runtimes)
                {
                    (void)room_id;
                    tickRuntime(runtime);
                }
            }

            const auto elapsed = std::chrono::steady_clock::now() - tick_start;
            const auto sleep_for = std::chrono::milliseconds(75) - elapsed;
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

    m_jobWorker = std::thread([this]()
    {
        while (m_running.load(std::memory_order_acquire))
        {
            SleepJob job;
            {
                std::unique_lock lock(m_jobMutex);
                m_jobCv.wait_for(lock, std::chrono::seconds(1), [this]()
                {
                    return !m_jobs.empty() || !m_running.load(std::memory_order_acquire);
                });
                if (!m_running.load(std::memory_order_acquire))
                    break;
                if (m_jobs.empty())
                    continue;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            processJob(job);
        }
    });
}

void SleepManager::stop()
{
    if (!m_running.exchange(false))
        return;

    m_stopCv.notify_all();
    m_jobCv.notify_all();

    if (m_worker.joinable())
        m_worker.join();
    if (m_jobWorker.joinable())
        m_jobWorker.join();
}

void SleepManager::reconcile()
{
    const auto configs = loadRoomConfigs();
    std::lock_guard lock(m_mutex);

    std::unordered_map<int32_t, bool> seen;
    for (const auto& config : configs)
    {
        seen[config.roomId] = true;
        auto it = m_runtimes.find(config.roomId);
        if (it == m_runtimes.end())
        {
            SleepRuntime runtime;
            runtime.config = config;
            std::string error;
            if (!initPipeline(runtime, error))
            {
                LOG_WARN("Sleep pipeline init failed for room {}: {}", config.roomId, error);
                continue;
            }
            m_runtimes.emplace(config.roomId, std::move(runtime));
            LOG_INFO(
                "Sleep runtime added (room={}, user={}, radar={})",
                config.roomId,
                config.userId,
                config.radarExternalId);
        }
        else
        {
            it->second.config = config;
        }
    }

    for (auto it = m_runtimes.begin(); it != m_runtimes.end();)
    {
        if (!seen.count(it->first))
            it = m_runtimes.erase(it);
        else
            ++it;
    }
}

bool SleepManager::is_sleep_enabled_radar(const std::string& external_id)
{
    for (const auto& entry : AppState::get().deviceManager.manifestEntries())
    {
        if (entry.config.value("id", "") != external_id)
            continue;
        return is_sleep_radar_manifest_entry(entry);
    }
    return false;
}

void SleepManager::tickVitals(SleepRuntime& runtime)
{
    if (runtime.sessionFsm.phase() != SessionPhase::Sleeping)
        return;

    auto* device = find_device_by_external_id(runtime.config.radarExternalId);
    auto* pc_provider = dynamic_cast<dev::IRadarPointCloudProvider*>(device);
    if (pc_provider && device && device->getState() == dev::DeviceState::Running)
    {
        std::vector<uint64_t> indices;
        pc_provider->enumeratePointCloudFrameIndices(indices);
        if (!indices.empty())
        {
            dev::RadarPointCloud frame;
            if (pc_provider->getPointCloudFrame(indices.back(), frame))
                runtime.vitalPicker.update(frame);
        }
    }

    auto* iq_provider = dynamic_cast<dev::IRadarIQProvider*>(device);
    const auto& target = runtime.vitalPicker.target();
    if (!iq_provider || !target.valid)
        return;

    static thread_local uint32_t s_vital_tick = 0;
    if (++s_vital_tick % 20 != 0)
        return;

    std::vector<dev::RadarIQRequest> requests(1);
    requests[0].azimuth = target.azimuth;
    requests[0].elevation = target.elevation;
    requests[0].distance = target.distance;

    std::vector<dev::RadarIQResponse> responses;
    auto future = iq_provider->requestIQAsync(requests, responses);
    if (future.wait_for(std::chrono::milliseconds(150)) != std::future_status::ready)
        return;

    future.get();
    if (responses.empty())
        return;

    runtime.vitalProcessor.pushSample(responses[0].iq);
    runtime.lastVitals = runtime.vitalProcessor.estimate();
}

std::vector<SleepRoomConfig> SleepManager::loadRoomConfigs()
{
    std::vector<SleepRoomConfig> configs;
    auto client = AppState::get().db();
    if (!client)
        return configs;

    try
    {
        const auto rows = client->execSqlSync(R"SQL(
SELECT
    r.id AS room_id,
    ru.user_id,
    d.id AS device_id,
    d.class AS device_class,
    d.name AS device_name
FROM room r
JOIN room_user_map ru ON ru.room_id = r.id
JOIN device_room_map drm ON drm.room_id = r.id
JOIN device d ON d.id = drm.device_id AND d.archived = 0 AND d.enabled = 1
WHERE d.class IN ('srs_r4sn', 'wave_station')
ORDER BY r.id, ru.user_id, d.id
)SQL");

        std::map<std::pair<int64_t, int64_t>, SleepRoomConfig> grouped;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int64_t room_id = rows[i]["room_id"].as<int64_t>();
            const int64_t user_id = rows[i]["user_id"].as<int64_t>();
            const auto key = std::make_pair(room_id, user_id);
            auto& config = grouped[key];
            if (config.roomId == 0)
            {
                config.roomId = static_cast<int32_t>(room_id);
                config.userId = static_cast<int32_t>(user_id);
            }

            const auto device_id = rows[i]["device_id"].as<int64_t>();
            const auto device_class = rows[i]["device_class"].as<std::string>();
            const auto device_name = rows[i]["device_name"].as<std::string>();
            const auto wire_id = dev::wireIdForDbRow(device_id, device_name);

            if (device_class == "srs_r4sn")
            {
                if (!is_sleep_enabled_radar(wire_id))
                    continue;
                config.radarDbId = device_id;
                config.radarExternalId = wire_id;
            }
            else if (device_class == "wave_station")
            {
                config.stationDbId = device_id;
                config.stationExternalId = wire_id;
            }
        }

        for (auto& [key, config] : grouped)
        {
            (void)key;
            if (config.radarDbId <= 0 || config.radarExternalId.empty())
                continue;
            configs.push_back(std::move(config));
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("Sleep room config load failed: {}", e.what());
    }

    return configs;
}

bool SleepManager::initPipeline(SleepRuntime& runtime, std::string& out_error)
{
    runtime.pipeline = std::make_unique<nn::SleepPipeline>();
    const auto& app = AppState::get();
    const auto model_path = app.resolvePath(app.config.sleep_model_path);

    std::ifstream in(model_path);
    if (!in)
    {
        out_error = "sleep model config not found: " + model_path.string();
        return false;
    }

    json config_json;
    try
    {
        in >> config_json;
    }
    catch (const std::exception& e)
    {
        out_error = std::string("invalid sleep model config: ") + e.what();
        return false;
    }

    if (!runtime.pipeline->init(app.config_dir.string(), config_json, out_error))
        return false;

    auto* device = find_device_by_external_id(runtime.config.radarExternalId);
    if (auto* provider = dynamic_cast<dev::IRadarPointCloudProvider*>(device))
    {
        const uint32_t seq = std::max(runtime.pipeline->getBedWindow(), runtime.pipeline->getTossWindow());
        provider->setPointCloudQueueSize(seq * 4);
    }

    m_modelReady = true;
    return true;
}

void SleepManager::runLoop()
{
}

void SleepManager::runJobLoop()
{
}

void SleepManager::tickRuntime(SleepRuntime& runtime)
{
    consumePointCloud(runtime);
    tickVitals(runtime);

    const std::string now_ts = formatTimestamp();
    flushSecondBoundary(runtime, now_ts);
    flushMinuteBoundary(runtime, now_ts);
    flushThirtyMinBoundary(runtime, now_ts);
}

void SleepManager::consumePointCloud(SleepRuntime& runtime)
{
    if (!runtime.pipeline)
        return;

    auto* device = find_device_by_external_id(runtime.config.radarExternalId);
    auto* provider = dynamic_cast<dev::IRadarPointCloudProvider*>(device);
    if (!provider || !device || device->getState() != dev::DeviceState::Running)
        return;

    std::vector<uint64_t> indices;
    provider->enumeratePointCloudFrameIndices(indices);
    if (indices.empty())
        return;

    bool evaluated = false;
    nn::SleepResult result;
    for (uint64_t frame_idx : indices)
    {
        if (frame_idx <= runtime.lastFrameIndex)
            continue;

        dev::RadarPointCloud frame;
        if (!provider->getPointCloudFrame(frame_idx, frame))
            continue;

        runtime.pipeline->pushFrame(std::move(frame));
        runtime.lastFrameIndex = frame_idx;

        if (runtime.pipeline->evaluate(result))
            evaluated = true;
    }

    if (evaluated)
        ingestResult(runtime, result);

    if (auto* radar = dynamic_cast<dev::SRSR4SN*>(device))
        radar->releasePointCloudFramesUpTo(runtime.lastFrameIndex);
}

void SleepManager::ingestResult(SleepRuntime& runtime, const nn::SleepResult& result)
{
    SecondSample sample;
    sample.statusClass = result.statusClass;
    sample.statusScores = result.statusScores;
    sample.tossValid = result.tossValid;
    sample.tossClass = result.tossClass;
    sample.tossIndex = result.tossIndex;
    sample.sampleCount = 1;
    sample.connected = true;

    const std::string now_ts = formatTimestamp();
    const std::string second_start = floorToSecond(now_ts);

    if (!runtime.secondInitialized)
    {
        runtime.secondAgg.reset(second_start);
        runtime.activeSecondStart = second_start;
        runtime.secondInitialized = true;
    }
    else if (second_start != runtime.activeSecondStart)
    {
        SecondSnapshot snapshot;
        if (runtime.secondAgg.flush(snapshot))
        {
            if (!runtime.minuteInitialized)
            {
                runtime.minuteAgg.reset(floorToMinute(snapshot.timeStart));
                runtime.activeMinuteStart = floorToMinute(snapshot.timeStart);
                runtime.minuteInitialized = true;
            }
            runtime.minuteAgg.addSecond(snapshot);
        }
        runtime.secondAgg.reset(second_start);
        runtime.activeSecondStart = second_start;
    }

    runtime.secondAgg.addSample(sample);
}

void SleepManager::flushSecondBoundary(SleepRuntime& runtime, const std::string& now_ts)
{
    if (!runtime.secondInitialized)
        return;

    const std::string second_start = floorToSecond(now_ts);
    if (second_start == runtime.activeSecondStart)
        return;

    SecondSnapshot snapshot;
    if (runtime.secondAgg.flush(snapshot))
    {
        if (!runtime.minuteInitialized)
        {
            runtime.minuteAgg.reset(floorToMinute(snapshot.timeStart));
            runtime.activeMinuteStart = floorToMinute(snapshot.timeStart);
            runtime.minuteInitialized = true;
        }
        runtime.minuteAgg.addSecond(snapshot);
    }

    runtime.secondAgg.reset(second_start);
    runtime.activeSecondStart = second_start;
}

void SleepManager::flushMinuteBoundary(SleepRuntime& runtime, const std::string& now_ts)
{
    if (!runtime.minuteInitialized)
        return;

    const std::string minute_start = floorToMinute(now_ts);
    if (minute_start == runtime.activeMinuteStart)
        return;

    MinuteStat stat;
    if (runtime.minuteAgg.flush(stat))
    {
        persistMinuteStat(runtime, stat);

        if (!runtime.thirtyMinInitialized)
        {
            runtime.thirtyMinAgg.reset(floorToThirtyMin(stat.timeStart));
            runtime.activeThirtyMinStart = floorToThirtyMin(stat.timeStart);
            runtime.thirtyMinInitialized = true;
        }
        runtime.thirtyMinAgg.addMinute(stat);

        if (auto close = runtime.sessionFsm.onMinute(stat))
            handleSessionClose(runtime, *close);
    }

    runtime.minuteAgg.reset(minute_start);
    runtime.activeMinuteStart = minute_start;
}

void SleepManager::flushThirtyMinBoundary(SleepRuntime& runtime, const std::string& now_ts)
{
    if (!runtime.thirtyMinInitialized)
        return;

    const std::string window_start = floorToThirtyMin(now_ts);
    if (window_start == runtime.activeThirtyMinStart)
        return;

    ThirtyMinStat stat;
    if (runtime.thirtyMinAgg.flush(stat))
        persistThirtyMinStat(runtime, stat);

    runtime.thirtyMinAgg.reset(window_start);
    runtime.activeThirtyMinStart = window_start;
}

void SleepManager::persistMinuteStat(SleepRuntime& runtime, const MinuteStat& stat)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        const auto& session_id = runtime.sessionFsm.state().sessionId;
        if (session_id)
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO sleep_stat (
    user_id, room_id, session_id, granularity, time_start, time_end, coverage,
    status_ratio, toss_mean, toss_max, toss_p90, toss_events, toss_ratio
) VALUES (?, ?, ?, '1m', ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(user_id, granularity, time_start) DO UPDATE SET
    room_id = excluded.room_id,
    session_id = excluded.session_id,
    time_end = excluded.time_end,
    coverage = excluded.coverage,
    status_ratio = excluded.status_ratio,
    toss_mean = excluded.toss_mean,
    toss_max = excluded.toss_max,
    toss_p90 = excluded.toss_p90,
    toss_events = excluded.toss_events,
    toss_ratio = excluded.toss_ratio
)SQL",
                runtime.config.userId,
                runtime.config.roomId,
                *session_id,
                stat.timeStart,
                stat.timeEnd,
                stat.coverage,
                stat.statusRatio.dump(),
                stat.tossMean,
                stat.tossMax,
                stat.tossP90,
                stat.tossEvents,
                stat.tossRatio.dump());
        }
        else
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO sleep_stat (
    user_id, room_id, session_id, granularity, time_start, time_end, coverage,
    status_ratio, toss_mean, toss_max, toss_p90, toss_events, toss_ratio
) VALUES (?, ?, NULL, '1m', ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(user_id, granularity, time_start) DO UPDATE SET
    room_id = excluded.room_id,
    session_id = excluded.session_id,
    time_end = excluded.time_end,
    coverage = excluded.coverage,
    status_ratio = excluded.status_ratio,
    toss_mean = excluded.toss_mean,
    toss_max = excluded.toss_max,
    toss_p90 = excluded.toss_p90,
    toss_events = excluded.toss_events,
    toss_ratio = excluded.toss_ratio
)SQL",
                runtime.config.userId,
                runtime.config.roomId,
                stat.timeStart,
                stat.timeEnd,
                stat.coverage,
                stat.statusRatio.dump(),
                stat.tossMean,
                stat.tossMax,
                stat.tossP90,
                stat.tossEvents,
                stat.tossRatio.dump());
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("sleep_stat 1m write failed: {}", e.what());
    }
}

void SleepManager::persistThirtyMinStat(SleepRuntime& runtime, const ThirtyMinStat& stat)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    std::optional<double> env_temp;
    std::optional<double> env_lux;
    if (runtime.config.stationExternalId)
    {
        env_temp = queryStationEnv(*runtime.config.stationExternalId, "temperature_c");
        env_lux = queryStationEnv(*runtime.config.stationExternalId, "lux");
    }

    const StageSynthResult stage = synthesizeThirtyMinStage(stat, runtime.asleepMinutesBeforeWindow);
    if (stat.statusRatio.is_object())
    {
        runtime.asleepMinutesBeforeWindow += static_cast<int32_t>(
            30.0 * stat.statusRatio.value("asleep", 0.0));
    }
    if (runtime.sessionFsm.state().onset)
        runtime.sessionStageWindows.push_back(stage);

    const double snore_ratio = estimateSnoreRatio(stat, env_temp);
    std::optional<double> hr_mean;
    std::optional<double> br_mean;
    std::optional<double> hr_confidence;
    if (runtime.lastVitals.hrBpm && runtime.lastVitals.hrConfidence > 0.0)
    {
        hr_mean = *runtime.lastVitals.hrBpm;
        hr_confidence = runtime.lastVitals.hrConfidence;
    }
    if (runtime.lastVitals.brRpm && runtime.lastVitals.brConfidence > 0.0)
        br_mean = *runtime.lastVitals.brRpm;

    try
    {
        const auto& session_id = runtime.sessionFsm.state().sessionId;
        const std::optional<std::string> summary_text = stat.summaryText.empty()
            ? std::nullopt
            : std::make_optional(stat.summaryText);

        if (session_id)
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO sleep_stat (
    user_id, room_id, session_id, granularity, time_start, time_end, coverage,
    stage_label, stage_ratio, stage_confidence,
    status_ratio, toss_mean, toss_max, toss_p90, toss_events, toss_ratio,
    hr_mean, hr_confidence, br_mean, snore_ratio,
    summary_text, env_temp, env_lux
) VALUES (?, ?, ?, '30m', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(user_id, granularity, time_start) DO UPDATE SET
    room_id = excluded.room_id,
    session_id = excluded.session_id,
    time_end = excluded.time_end,
    coverage = excluded.coverage,
    stage_label = excluded.stage_label,
    stage_ratio = excluded.stage_ratio,
    stage_confidence = excluded.stage_confidence,
    status_ratio = excluded.status_ratio,
    toss_mean = excluded.toss_mean,
    toss_max = excluded.toss_max,
    toss_p90 = excluded.toss_p90,
    toss_events = excluded.toss_events,
    toss_ratio = excluded.toss_ratio,
    hr_mean = excluded.hr_mean,
    hr_confidence = excluded.hr_confidence,
    br_mean = excluded.br_mean,
    snore_ratio = excluded.snore_ratio,
    summary_text = excluded.summary_text,
    env_temp = excluded.env_temp,
    env_lux = excluded.env_lux
)SQL",
                runtime.config.userId,
                runtime.config.roomId,
                *session_id,
                stat.timeStart,
                stat.timeEnd,
                stat.coverage,
                stage.stageLabel,
                stage.stageRatio.dump(),
                stage.confidence,
                stat.statusRatio.dump(),
                stat.tossMean,
                stat.tossMax,
                stat.tossP90,
                stat.tossEvents,
                stat.tossRatio.dump(),
                hr_mean,
                hr_confidence,
                br_mean,
                snore_ratio,
                summary_text,
                env_temp,
                env_lux);
        }
        else
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO sleep_stat (
    user_id, room_id, session_id, granularity, time_start, time_end, coverage,
    stage_label, stage_ratio, stage_confidence,
    status_ratio, toss_mean, toss_max, toss_p90, toss_events, toss_ratio,
    hr_mean, hr_confidence, br_mean, snore_ratio,
    summary_text, env_temp, env_lux
) VALUES (?, ?, NULL, '30m', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(user_id, granularity, time_start) DO UPDATE SET
    room_id = excluded.room_id,
    session_id = excluded.session_id,
    time_end = excluded.time_end,
    coverage = excluded.coverage,
    stage_label = excluded.stage_label,
    stage_ratio = excluded.stage_ratio,
    stage_confidence = excluded.stage_confidence,
    status_ratio = excluded.status_ratio,
    toss_mean = excluded.toss_mean,
    toss_max = excluded.toss_max,
    toss_p90 = excluded.toss_p90,
    toss_events = excluded.toss_events,
    toss_ratio = excluded.toss_ratio,
    hr_mean = excluded.hr_mean,
    hr_confidence = excluded.hr_confidence,
    br_mean = excluded.br_mean,
    snore_ratio = excluded.snore_ratio,
    summary_text = excluded.summary_text,
    env_temp = excluded.env_temp,
    env_lux = excluded.env_lux
)SQL",
                runtime.config.userId,
                runtime.config.roomId,
                stat.timeStart,
                stat.timeEnd,
                stat.coverage,
                stage.stageLabel,
                stage.stageRatio.dump(),
                stage.confidence,
                stat.statusRatio.dump(),
                stat.tossMean,
                stat.tossMax,
                stat.tossP90,
                stat.tossEvents,
                stat.tossRatio.dump(),
                hr_mean,
                hr_confidence,
                br_mean,
                snore_ratio,
                summary_text,
                env_temp,
                env_lux);
        }

        const auto rows = client->execSqlSync(
            "SELECT id FROM sleep_stat WHERE user_id = ? AND granularity = '30m' AND time_start = ? LIMIT 1",
            runtime.config.userId,
            stat.timeStart);
        if (!rows.empty())
        {
            SleepJob job;
            job.kind = SleepJobKind::Summary30m;
            job.userId = runtime.config.userId;
            job.roomId = runtime.config.roomId;
            job.statId = rows[0]["id"].as<int64_t>();

            json payload;
            payload["id"] = job.statId;
            payload["userId"] = runtime.config.userId;
            payload["roomId"] = runtime.config.roomId;
            payload["granularity"] = "30m";
            payload["timeStart"] = stat.timeStart;
            payload["timeEnd"] = stat.timeEnd;
            payload["coverage"] = stat.coverage;
            payload["stageLabel"] = stage.stageLabel;
            payload["stageRatio"] = stage.stageRatio;
            payload["stageConfidence"] = stage.confidence;
            payload["statusRatio"] = stat.statusRatio;
            payload["tossMean"] = stat.tossMean;
            payload["tossMax"] = stat.tossMax;
            payload["tossP90"] = stat.tossP90;
            payload["tossEvents"] = stat.tossEvents;
            payload["tossRatio"] = stat.tossRatio;
            payload["snoreRatio"] = snore_ratio;
            payload["summaryText"] = stat.summaryText;
            if (env_temp)
                payload["envTemp"] = *env_temp;
            if (env_lux)
                payload["envLux"] = *env_lux;
            job.payload = std::move(payload);
            enqueueJob(std::move(job));
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("sleep_stat 30m write failed: {}", e.what());
    }
}

std::optional<double> SleepManager::queryStationEnv(const std::string& external_id, const char* field)
{
    auto* device = find_device_by_external_id(external_id);
    auto* station = dynamic_cast<dev::RadaiWs*>(device);
    if (!station || device->getState() != dev::DeviceState::Running)
        return std::nullopt;

    try
    {
        const json result = station->query("env", json::object());
        if (result.is_object() && result.contains("code") && result["code"].is_number_integer()
            && result["code"].get<int>() < 0)
        {
            return std::nullopt;
        }
        if (result.contains(field))
            return result[field].get<double>();
    }
    catch (const std::exception&)
    {
    }
    return std::nullopt;
}

void SleepManager::handleSessionClose(SleepRuntime& runtime, const SessionCloseResult& close)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        if (runtime.config.stationDbId > 0)
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO sleep_session (
    user_id, room_id, radar_id, station_id, night_date, onset, final_wake,
    time_in_bed_s, asleep_total_s, efficiency, toss_events
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
                runtime.config.userId,
                runtime.config.roomId,
                runtime.config.radarDbId,
                runtime.config.stationDbId,
                close.nightDate,
                close.onset,
                close.finalWake,
                close.timeInBedS,
                close.asleepTotalS,
                close.efficiency,
                close.tossEvents);
        }
        else
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO sleep_session (
    user_id, room_id, radar_id, station_id, night_date, onset, final_wake,
    time_in_bed_s, asleep_total_s, efficiency, toss_events
) VALUES (?, ?, ?, NULL, ?, ?, ?, ?, ?, ?, ?)
)SQL",
                runtime.config.userId,
                runtime.config.roomId,
                runtime.config.radarDbId,
                close.nightDate,
                close.onset,
                close.finalWake,
                close.timeInBedS,
                close.asleepTotalS,
                close.efficiency,
                close.tossEvents);
        }

        const auto rows = client->execSqlSync(
            "SELECT id FROM sleep_session WHERE user_id = ? AND night_date = ? AND onset = ? ORDER BY id DESC LIMIT 1",
            runtime.config.userId,
            close.nightDate,
            close.onset);
        if (rows.empty())
            return;

        const int64_t session_id = rows[0]["id"].as<int64_t>();
        runtime.sessionFsm.mutableState().sessionId = session_id;
        backfillSessionId(runtime.config.userId, close.onset, close.finalWake, session_id);

        json stage_totals_sec = json::object();
        const json stage_totals_min = synthesizeStageTotals(runtime.sessionStageWindows);
        for (auto it = stage_totals_min.begin(); it != stage_totals_min.end(); ++it)
            stage_totals_sec[it.key()] = it.value().get<double>() * 60.0;

        std::optional<double> hr_mean;
        std::optional<double> br_mean;
        std::optional<double> snore_ratio;
        const auto vitals_rows = client->execSqlSync(
            R"SQL(
SELECT AVG(hr_mean) AS hr_mean, AVG(br_mean) AS br_mean, AVG(snore_ratio) AS snore_ratio
FROM sleep_stat
WHERE session_id = ? AND granularity = '30m'
)SQL",
            session_id);
        if (!vitals_rows.empty())
        {
            if (!vitals_rows[0]["hr_mean"].isNull())
                hr_mean = vitals_rows[0]["hr_mean"].as<double>();
            if (!vitals_rows[0]["br_mean"].isNull())
                br_mean = vitals_rows[0]["br_mean"].as<double>();
            if (!vitals_rows[0]["snore_ratio"].isNull())
                snore_ratio = vitals_rows[0]["snore_ratio"].as<double>();
        }

        std::optional<std::string> stage_totals_json;
        if (!stage_totals_sec.empty())
            stage_totals_json = stage_totals_sec.dump();

        client->execSqlSync(
            R"SQL(
UPDATE sleep_session
SET stage_totals = ?, hr_mean = ?, br_mean = ?, snore_ratio = ?
WHERE id = ?
)SQL",
            stage_totals_json,
            hr_mean,
            br_mean,
            snore_ratio,
            session_id);

        SleepJob daily;
        daily.kind = SleepJobKind::DailyReport;
        daily.userId = runtime.config.userId;
        daily.roomId = runtime.config.roomId;
        daily.sessionId = session_id;
        daily.period = "daily";
        daily.periodStart = close.nightDate;
        enqueueJob(std::move(daily));

        SleepJob weekly;
        weekly.kind = SleepJobKind::WeeklyReport;
        weekly.userId = runtime.config.userId;
        weekly.roomId = runtime.config.roomId;
        weekly.period = "weekly";
        weekly.periodStart = mondayOfWeek(close.nightDate);
        enqueueJob(std::move(weekly));

        runtime.sessionFsm.reset();
        runtime.sessionStageWindows.clear();
        runtime.asleepMinutesBeforeWindow = 0;
    }
    catch (const std::exception& e)
    {
        LOG_WARN("sleep_session write failed: {}", e.what());
    }
}

void SleepManager::backfillSessionId(
    int32_t user_id,
    const std::string& onset,
    const std::string& final_wake,
    int64_t session_id)
{
    auto client = AppState::get().db();
    if (!client)
        return;

    try
    {
        client->execSqlSync(
            R"SQL(
UPDATE sleep_stat
SET session_id = ?
WHERE user_id = ?
  AND time_start >= ?
  AND time_start < ?
)SQL",
            session_id,
            user_id,
            onset,
            final_wake);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("sleep_stat session backfill failed: {}", e.what());
    }
}

void SleepManager::enqueueJob(SleepJob job)
{
    {
        std::lock_guard lock(m_jobMutex);
        m_jobs.push_back(std::move(job));
    }
    m_jobCv.notify_one();
}

void SleepManager::processJob(const SleepJob& job)
{
    const auto& app = AppState::get();
    auto client = app.db();
    if (!client)
        return;

    AgentSleepJobResult agent_result;
    std::string error;

    if (job.kind == SleepJobKind::Summary30m)
    {
        json body;
        body["window"] = job.payload;
        body["embed"] = true;

        if (runSleepJobSync(app.config.agent.base_url, "/sleep/v1/summaries", body, agent_result, error)
            != AgentClientResult::success)
        {
            LOG_WARN("sleep summary job failed (stat {}): {}", job.statId, error);
            try
            {
                const std::string fallback = job.payload.value("summaryText", std::string("수면 구간 요약"));
                client->execSqlSync(
                    "UPDATE sleep_stat SET summary_text = ? WHERE id = ?",
                    fallback,
                    job.statId);
            }
            catch (const std::exception& e)
            {
                LOG_WARN("sleep summary fallback write failed: {}", e.what());
            }
            return;
        }

        try
        {
            client->execSqlSync(
                "UPDATE sleep_stat SET summary_text = ? WHERE id = ?",
                agent_result.text,
                job.statId);
            storeAgentEmbeddings(SleepJobKind::Summary30m, job.statId, agent_result.embedding);
        }
        catch (const std::exception& e)
        {
            LOG_WARN("sleep summary write failed: {}", e.what());
        }
        return;
    }

    if (job.kind == SleepJobKind::DailyReport)
    {
        const auto session_rows = client->execSqlSync(
            "SELECT * FROM sleep_session WHERE id = ? LIMIT 1",
            job.sessionId);
        if (session_rows.empty())
            return;

        const auto stats_rows = client->execSqlSync(
            "SELECT * FROM sleep_stat WHERE session_id = ? AND granularity = '30m' ORDER BY time_start",
            job.sessionId);

        json metrics;
        metrics["asleepTotalS"] = session_rows[0]["asleep_total_s"].isNull()
            ? nullptr
            : json(session_rows[0]["asleep_total_s"].as<int64_t>());
        metrics["timeInBedS"] = session_rows[0]["time_in_bed_s"].isNull()
            ? nullptr
            : json(session_rows[0]["time_in_bed_s"].as<int64_t>());
        metrics["efficiency"] = session_rows[0]["efficiency"].isNull()
            ? nullptr
            : json(session_rows[0]["efficiency"].as<double>());
        metrics["latencyS"] = nullptr;
        metrics["tossEvents"] = session_rows[0]["toss_events"].isNull()
            ? nullptr
            : json(session_rows[0]["toss_events"].as<int64_t>());
        metrics["snoreRatio"] = nullptr;

        json body;
        body["userId"] = job.userId;
        body["period"] = "daily";
        body["periodStart"] = job.periodStart;
        body["metrics"] = metrics;
        body["sessions"] = json::array({row_to_camel_session(session_rows, 0)});
        json stats30m = json::array();
        for (size_t i = 0; i < stats_rows.size(); ++i)
            stats30m.push_back(row_to_camel_stat(stats_rows, i));
        body["stats30m"] = stats30m;
        body["embed"] = true;

        if (runSleepJobSync(app.config.agent.base_url, "/sleep/v1/reports", body, agent_result, error)
            != AgentClientResult::success)
        {
            LOG_WARN("sleep daily report job failed ({}): {}", job.periodStart, error);
            return;
        }

        try
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO sleep_report (user_id, period, period_start, session_id, metrics, report_text)
VALUES (?, 'daily', ?, ?, ?, ?)
ON CONFLICT(user_id, period, period_start) DO UPDATE SET
    session_id = excluded.session_id,
    metrics = excluded.metrics,
    report_text = excluded.report_text
)SQL",
                job.userId,
                job.periodStart,
                job.sessionId,
                metrics.dump(),
                agent_result.text);

            const auto report_rows = client->execSqlSync(
                "SELECT id FROM sleep_report WHERE user_id = ? AND period = 'daily' AND period_start = ? LIMIT 1",
                job.userId,
                job.periodStart);
            if (!report_rows.empty())
                storeAgentEmbeddings(SleepJobKind::DailyReport, report_rows[0]["id"].as<int64_t>(), agent_result.embedding);
        }
        catch (const std::exception& e)
        {
            LOG_WARN("sleep_report daily write failed: {}", e.what());
        }

        {
            std::string insight_error;
            if (!generateAndPersistInsights(
                    client, app.config.agent.base_url, job.userId, "sleep_report", job.periodStart, insight_error))
            {
                LOG_WARN("insight generation (sleep_report daily) failed: {}", insight_error);
            }
        }
        return;
    }

    if (job.kind == SleepJobKind::WeeklyReport)
    {
        const auto week_end_rows = client->execSqlSync(
            "SELECT date(?, '+7 day') AS week_end",
            job.periodStart);
        const std::string week_end = week_end_rows.empty()
            ? job.periodStart
            : week_end_rows[0]["week_end"].as<std::string>();

        const auto session_rows = client->execSqlSync(
            "SELECT * FROM sleep_session WHERE user_id = ? AND night_date >= ? AND night_date < ? ORDER BY night_date",
            job.userId,
            job.periodStart,
            week_end);
        if (session_rows.empty())
            return;

        std::vector<int64_t> session_ids;
        for (size_t i = 0; i < session_rows.size(); ++i)
            session_ids.push_back(session_rows[i]["id"].as<int64_t>());

        std::string placeholders;
        for (size_t i = 0; i < session_ids.size(); ++i)
        {
            if (i > 0)
                placeholders += ",";
            placeholders += std::to_string(session_ids[i]);
        }

        const auto stats_rows = client->execSqlSync(
            "SELECT * FROM sleep_stat WHERE granularity = '30m' AND session_id IN (" + placeholders
                + ") ORDER BY time_start");

        double asleep_sum = 0.0;
        double eff_sum = 0.0;
        int count = 0;
        for (size_t i = 0; i < session_rows.size(); ++i)
        {
            if (!session_rows[i]["asleep_total_s"].isNull())
            {
                asleep_sum += session_rows[i]["asleep_total_s"].as<double>();
                ++count;
            }
            if (!session_rows[i]["efficiency"].isNull())
                eff_sum += session_rows[i]["efficiency"].as<double>();
        }

        json metrics;
        metrics["nights"] = static_cast<int64_t>(session_rows.size());
        if (count > 0)
        {
            metrics["avgAsleepS"] = static_cast<int64_t>(asleep_sum / count);
            metrics["avgEfficiency"] = eff_sum / count;
        }
        else
        {
            metrics["avgAsleepS"] = nullptr;
            metrics["avgEfficiency"] = nullptr;
        }

        json body;
        body["userId"] = job.userId;
        body["period"] = "weekly";
        body["periodStart"] = job.periodStart;
        body["metrics"] = metrics;
        json sessions = json::array();
        for (size_t i = 0; i < session_rows.size(); ++i)
            sessions.push_back(row_to_camel_session(session_rows, i));
        body["sessions"] = sessions;
        json stats30m = json::array();
        for (size_t i = 0; i < stats_rows.size(); ++i)
            stats30m.push_back(row_to_camel_stat(stats_rows, i));
        body["stats30m"] = stats30m;
        body["embed"] = true;

        if (runSleepJobSync(app.config.agent.base_url, "/sleep/v1/reports", body, agent_result, error)
            != AgentClientResult::success)
        {
            LOG_WARN("sleep weekly report job failed ({}): {}", job.periodStart, error);
            return;
        }

        try
        {
            client->execSqlSync(
                R"SQL(
INSERT INTO sleep_report (user_id, period, period_start, session_id, metrics, report_text)
VALUES (?, 'weekly', ?, NULL, ?, ?)
ON CONFLICT(user_id, period, period_start) DO UPDATE SET
    metrics = excluded.metrics,
    report_text = excluded.report_text
)SQL",
                job.userId,
                job.periodStart,
                metrics.dump(),
                agent_result.text);

            const auto report_rows = client->execSqlSync(
                "SELECT id FROM sleep_report WHERE user_id = ? AND period = 'weekly' AND period_start = ? LIMIT 1",
                job.userId,
                job.periodStart);
            if (!report_rows.empty())
                storeAgentEmbeddings(SleepJobKind::WeeklyReport, report_rows[0]["id"].as<int64_t>(), agent_result.embedding);
        }
        catch (const std::exception& e)
        {
            LOG_WARN("sleep_report weekly write failed: {}", e.what());
        }

        {
            std::string insight_error;
            if (!generateAndPersistInsights(
                    client, app.config.agent.base_url, job.userId, "sleep_report", job.periodStart, insight_error))
            {
                LOG_WARN("insight generation (sleep_report weekly) failed: {}", insight_error);
            }
        }
    }
}

void SleepManager::storeAgentEmbeddings(
    SleepJobKind kind,
    int64_t row_id,
    const std::vector<float>& embedding)
{
    if (embedding.empty())
        return;

    auto client = AppState::get().db();
    if (!client)
        return;

    SleepVecStore store(client);
    switch (kind)
    {
    case SleepJobKind::Summary30m:
        store.storeSleepStatEmbedding(row_id, embedding);
        break;
    case SleepJobKind::DailyReport:
    case SleepJobKind::WeeklyReport:
        store.storeSleepReportEmbedding(row_id, embedding);
        break;
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
