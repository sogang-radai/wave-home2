#include "trigger_manager.h"

#include <algorithm>
#include <ctime>
#include <fstream>

#include "../core/logger.h"
#include "../device/device.h"
#include "../device/interface/radar.h"
#include "../device/platform/srs_r4sn.h"
#include "../service/power_manager.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    std::string weekdayToken(int wday)
    {
        switch (wday)
        {
        case 1:
            return "mon";
        case 2:
            return "tue";
        case 3:
            return "wed";
        case 4:
            return "thu";
        case 5:
            return "fri";
        case 6:
            return "sat";
        case 0:
            return "sun";
        default:
            return "";
        }
    }

    bool parseClock(const std::string& hhmm, int& out_hour, int& out_minute)
    {
        const auto colon = hhmm.find(':');
        if (colon == std::string::npos)
            return false;
        out_hour = std::stoi(hhmm.substr(0, colon));
        out_minute = std::stoi(hhmm.substr(colon + 1));
        return true;
    }

    bool localClockMatches(const std::string& hhmm)
    {
        int hour = 0;
        int minute = 0;
        if (!parseClock(hhmm, hour, minute))
            return false;

        const std::time_t now_t = std::time(nullptr);
        std::tm local_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &now_t);
#else
        localtime_r(&now_t, &local_tm);
#endif
        return local_tm.tm_hour == hour && local_tm.tm_min == minute;
    }

    int localWeekday()
    {
        const std::time_t now_t = std::time(nullptr);
        std::tm local_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &now_t);
#else
        localtime_r(&now_t, &local_tm);
#endif
        return local_tm.tm_wday;
    }
}

TriggerManager::TriggerManager() = default;

TriggerManager::~TriggerManager()
{
    stop();
}

void TriggerManager::start(
    RuleStore& rules,
    ActionQueue& actions,
    dev::DeviceManager& devices,
    const std::function<std::filesystem::path(const std::string&)>& resolve_path)
{
    if (m_running.exchange(true))
        return;

    m_rules = &rules;
    m_actions = &actions;
    m_devices = &devices;
    m_resolvePath = resolve_path;

    reconcile();
    m_worker = std::thread([this]() { runLoop(); });
}

void TriggerManager::stop()
{
    if (!m_running.exchange(false))
        return;

    if (m_worker.joinable())
        m_worker.join();

    std::lock_guard lock(m_stateMutex);
    for (auto& runtime : m_gestureRuntimes)
    {
        if (runtime.pipeline)
            runtime.pipeline->shutdown();
    }
    m_gestureRuntimes.clear();
    m_deviceStateRuntime.clear();
    m_scheduleStates.clear();
    m_lastFired.clear();
    m_lastRepeatFire.clear();
    m_index.reset();

    m_rules = nullptr;
    m_actions = nullptr;
    m_devices = nullptr;
}

void TriggerManager::reconcile()
{
    if (!m_rules)
        return;

    const auto index = m_rules->snapshot();
    std::lock_guard lock(m_stateMutex);
    m_index = index;
    if (!index)
        return;

    for (const auto& [key, bindings] : index->gesture)
    {
        (void)bindings;
        ensureGestureRuntime(key);
    }
    removeStaleGestureRuntimes(index);

    m_scheduleStates.clear();
    const auto now = std::chrono::system_clock::now();
    for (const auto& rule : index->scheduleRules)
    {
        ScheduleState state;
        state.rule = rule;
        state.createdAt = now;
        if (rule.schedule && rule.schedule->repeat == "once" && rule.schedule->delayMinutes)
            state.nextFire = now + std::chrono::minutes(*rule.schedule->delayMinutes);
        else
            state.nextFire = now;
        m_scheduleStates[rule.id] = state;
    }
}

dev::Device* TriggerManager::findDevice(const std::string& external_id) const
{
    if (!m_devices)
        return nullptr;

    const auto id = dev::parseDeviceID(external_id);
    if (id == 0)
        return nullptr;
    return m_devices->findDevice(id);
}

void TriggerManager::ensureGestureRuntime(const GestureIndexKey& key)
{
    for (const auto& runtime : m_gestureRuntimes)
    {
        if (runtime.radarDeviceId == key.radarDeviceId && runtime.gestureSetPath == key.gestureSetPath)
            return;
    }

    if (!m_resolvePath)
        return;

    const auto set_path = m_resolvePath(key.gestureSetPath);
    if (!std::filesystem::exists(set_path))
    {
        LOG_WARN("TriggerManager: gesture set not found: {}", set_path.string());
        return;
    }

    try
    {
        json set_config;
        {
            std::ifstream in(set_path);
            in >> set_config;
        }

        GestureRuntime runtime;
        runtime.radarDeviceId = key.radarDeviceId;
        runtime.gestureSetPath = key.gestureSetPath;
        runtime.pipeline = std::make_unique<nn::GesturePipeline>();

        std::string error;
        const auto set_dir = set_path.parent_path();
        if (!runtime.pipeline->init(set_dir.string(), set_config, error))
        {
            LOG_WARN("TriggerManager: gesture pipeline init failed: {}", error);
            return;
        }

        auto* device = findDevice(key.radarDeviceId);
        if (auto* provider = dynamic_cast<dev::IRadarPointCloudProvider*>(device))
        {
            const uint32_t seq = runtime.pipeline->getSequenceLength();
            provider->setPointCloudQueueSize(seq > 0 ? seq * 4 : 144);
        }

        m_gestureRuntimes.push_back(std::move(runtime));
        LOG_INFO(
            "TriggerManager: gesture pipeline ready (radar={}, set={})",
            key.radarDeviceId,
            key.gestureSetPath);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("TriggerManager: failed to load gesture set {}: {}", key.gestureSetPath, e.what());
    }
}

void TriggerManager::removeStaleGestureRuntimes(const TriggerIndexSnapshot& index)
{
    std::vector<GestureRuntime> kept;
    kept.reserve(m_gestureRuntimes.size());

    for (auto& runtime : m_gestureRuntimes)
    {
        GestureIndexKey key{runtime.radarDeviceId, runtime.gestureSetPath};
        if (index->gesture.contains(key))
        {
            kept.push_back(std::move(runtime));
            continue;
        }

        if (runtime.pipeline)
            runtime.pipeline->shutdown();
    }

    m_gestureRuntimes = std::move(kept);
}

bool TriggerManager::checkCooldown(const std::string& rule_id, uint32_t cooldown_ms)
{
    if (cooldown_ms == 0)
        return true;

    const auto now = std::chrono::steady_clock::now();
    const auto it = m_lastFired.find(rule_id);
    if (it == m_lastFired.end())
        return true;

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
    return elapsed >= static_cast<int64_t>(cooldown_ms);
}

void TriggerManager::markFired(const std::string& rule_id)
{
    m_lastFired[rule_id] = std::chrono::steady_clock::now();
}

bool TriggerManager::compareValue(const std::string& op, double lhs, double rhs) const
{
    if (op == ">")
        return lhs > rhs;
    if (op == ">=")
        return lhs >= rhs;
    if (op == "<")
        return lhs < rhs;
    if (op == "<=")
        return lhs <= rhs;
    if (op == "==")
        return lhs == rhs;
    return false;
}

bool TriggerManager::evaluateDeviceState(const Trigger& trigger, double& out_value) const
{
    if (trigger.query == "power")
    {
        const auto reading = PowerManager::get().getReading(trigger.sourceDeviceId);
        if (!reading || !reading->connected)
            return false;
        out_value = reading->power_w;
        return true;
    }

    auto* device = findDevice(trigger.sourceDeviceId);
    if (!device || device->getState() != dev::DeviceState::Running)
        return false;

    auto* queryable = dynamic_cast<dev::Queryable*>(device);
    if (!queryable)
        return false;

    const json result = queryable->query(trigger.query, json::object());
    if (result.is_object())
    {
        if (result.contains(trigger.query) && result[trigger.query].is_number())
        {
            out_value = result[trigger.query].get<double>();
            return true;
        }
        if (result.contains("power") && result["power"].is_number())
        {
            out_value = result["power"].get<double>();
            return true;
        }
    }
    return false;
}

void TriggerManager::dispatchBindings(
    const std::vector<TriggerBinding>& bindings,
    int32_t gesture_class_id,
    const std::string& source_ref)
{
    if (!m_actions)
        return;

    for (const auto& binding : bindings)
    {
        if (gesture_class_id >= 0 && binding.gestureClassId != gesture_class_id)
            continue;

        if (!checkCooldown(binding.ruleId, binding.cooldownMs))
            continue;

        ActionJob job;
        job.targetDeviceId = binding.action.deviceId;
        job.actionName = binding.action.name;
        job.params = binding.action.params;
        job.execMode = binding.execMode;
        job.repeatIntervalMs = binding.repeatIntervalMs;
        job.ruleId = binding.ruleId;
        job.sourceRef = source_ref.empty() ? ("rule:" + binding.ruleId) : source_ref;
        job.logMessage = "룰 실행: " + binding.ruleName + " → " + binding.action.name;

        if (binding.execMode == ExecMode::Once)
        {
            m_actions->enqueue(std::move(job));
            markFired(binding.ruleId);
        }
        else if (binding.execMode == ExecMode::Repeat)
        {
            m_actions->enqueue(std::move(job));
            markFired(binding.ruleId);
        }
        else
        {
            m_actions->enqueue(std::move(job));
            markFired(binding.ruleId);
        }
    }
}

void TriggerManager::tickGesture()
{
    std::lock_guard lock(m_stateMutex);
    if (!m_index)
        return;

    for (auto& runtime : m_gestureRuntimes)
    {
        if (!runtime.pipeline)
            continue;

        auto* device = findDevice(runtime.radarDeviceId);
        auto* provider = dynamic_cast<dev::IRadarPointCloudProvider*>(device);
        if (!provider || !device || device->getState() != dev::DeviceState::Running)
            continue;

        std::vector<uint64_t> indices;
        provider->enumeratePointCloudFrameIndices(indices);
        if (indices.empty())
            continue;

        for (uint64_t frame_idx : indices)
        {
            if (frame_idx <= runtime.lastFrameIndex)
                continue;

            dev::RadarPointCloud frame;
            if (!provider->getPointCloudFrame(frame_idx, frame))
                continue;

            runtime.pipeline->pushFrame(std::move(frame));
            runtime.lastFrameIndex = frame_idx;
        }

        nn::GestureResult result;
        if (!runtime.pipeline->evaluate(result))
            continue;

        GestureIndexKey key{runtime.radarDeviceId, runtime.gestureSetPath};
        const auto it = m_index->gesture.find(key);
        if (it == m_index->gesture.end())
            continue;

        nn::ControlSignal signal;
        while (runtime.pipeline->popSignal(signal))
            dispatchBindings(it->second, signal.classId, "gesture:" + std::to_string(signal.classId));

        const auto now = std::chrono::steady_clock::now();
        for (const auto& binding : it->second)
        {
            if (binding.execMode != ExecMode::Repeat)
                continue;
            if (!runtime.pipeline->isClassActive(binding.gestureClassId))
            {
                m_actions->cancelRepeatJobs(binding.ruleId);
                continue;
            }

            const uint32_t interval = binding.repeatIntervalMs > 0 ? binding.repeatIntervalMs : 200;
            const auto last_it = m_lastRepeatFire.find(binding.ruleId);
            if (last_it != m_lastRepeatFire.end())
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_it->second).count();
                if (elapsed < static_cast<int64_t>(interval))
                    continue;
            }

            if (!checkCooldown(binding.ruleId, binding.cooldownMs))
                continue;

            ActionJob job;
            job.targetDeviceId = binding.action.deviceId;
            job.actionName = binding.action.name;
            job.params = binding.action.params;
            job.execMode = ExecMode::Once;
            job.ruleId = binding.ruleId;
            job.sourceRef = "rule:" + binding.ruleId;
            job.logMessage = "룰 반복: " + binding.ruleName + " → " + binding.action.name;
            m_actions->enqueue(std::move(job));
            m_lastRepeatFire[binding.ruleId] = now;
        }

        if (auto* radar = dynamic_cast<dev::SRSR4SN*>(device))
            radar->releasePointCloudFramesUpTo(runtime.lastFrameIndex);
    }
}

void TriggerManager::tickDeviceState()
{
    std::lock_guard lock(m_stateMutex);
    if (!m_index)
        return;

    for (const auto& [key, bindings] : m_index->deviceState)
    {
        const auto trigger_it = std::find_if(
            m_index->triggers.begin(),
            m_index->triggers.end(),
            [&](const auto& entry)
            {
                const Trigger& trigger = entry.second;
                return trigger.kind == TriggerKind::DeviceState
                    && trigger.sourceDeviceId == key.deviceId
                    && trigger.query == key.query;
            });

        if (trigger_it == m_index->triggers.end())
            continue;

        double value = 0.0;
        if (!evaluateDeviceState(trigger_it->second, value))
            continue;

        const bool matched = compareValue(trigger_it->second.op, value, trigger_it->second.value);

        DeviceStateRuntime& runtime = m_deviceStateRuntime[key];
        if (matched && !runtime.lastMatched)
            dispatchBindings(bindings, -1, "device_state:" + key.query);

        runtime.lastMatched = matched;
    }
}

void TriggerManager::tickSchedule()
{
    const auto now = std::chrono::system_clock::now();
    std::vector<Rule> due_rules;
    std::vector<std::string> once_to_disable;

    {
        std::lock_guard lock(m_stateMutex);
        for (auto& [rule_id, state] : m_scheduleStates)
        {
            (void)rule_id;
            if (!state.rule.enabled || !state.rule.schedule)
                continue;

            const RuleSchedule& schedule = *state.rule.schedule;

            if (schedule.repeat == "once")
            {
                if (!state.firedOnce && now >= state.nextFire)
                {
                    due_rules.push_back(state.rule);
                    state.firedOnce = true;
                    state.rule.enabled = false;
                    once_to_disable.push_back(state.rule.id);
                }
                continue;
            }

            if (schedule.repeat == "daily" && schedule.time)
            {
                if (localClockMatches(*schedule.time) && !state.firedOnce)
                {
                    due_rules.push_back(state.rule);
                    state.firedOnce = true;
                }
                else if (!localClockMatches(*schedule.time))
                {
                    state.firedOnce = false;
                }
                continue;
            }

            if (schedule.repeat == "weekly" && schedule.time)
            {
                const std::string token = weekdayToken(localWeekday());
                const bool day_match = std::find(schedule.daysOfWeek.begin(), schedule.daysOfWeek.end(), token)
                    != schedule.daysOfWeek.end();
                if (day_match && localClockMatches(*schedule.time) && !state.firedOnce)
                {
                    due_rules.push_back(state.rule);
                    state.firedOnce = true;
                }
                else if (!localClockMatches(*schedule.time))
                {
                    state.firedOnce = false;
                }
            }
        }
    }

    for (const auto& rule : due_rules)
    {
        TriggerBinding binding;
        binding.ruleId = rule.id;
        binding.ruleName = rule.name;
        binding.execMode = rule.execMode;
        binding.cooldownMs = rule.cooldownMs;
        binding.repeatIntervalMs = rule.repeatIntervalMs;
        binding.action = rule.action;
        dispatchBindings({binding}, -1, rule.schedule ? "schedule:" + rule.id : "rule:" + rule.id);
    }

    // One-shot schedules should turn off after firing so the UI toggle reflects completion.
    for (const auto& rule_id : once_to_disable)
    {
        if (!m_rules)
            continue;
        try
        {
            m_rules->setEnabledAsync(rule_id, false).get();
        }
        catch (const std::exception& e)
        {
            LOG_WARN("TriggerManager: disable once schedule failed ({}): {}", rule_id, e.what());
        }
    }
}

void TriggerManager::onIrReceived(const std::string& device_id, const std::string& command_id)
{
    TriggerIndexSnapshot index;
    {
        std::lock_guard lock(m_stateMutex);
        index = m_index;
    }

    if (!index)
        return;

    IrRecvIndexKey key{device_id, command_id};
    const auto it = index->irRecv.find(key);
    if (it == index->irRecv.end())
        return;

    dispatchBindings(it->second, -1, "ir:" + command_id);
}

void TriggerManager::runLoop()
{
    while (m_running.load())
    {
        tickGesture();
        tickDeviceState();
        tickSchedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
