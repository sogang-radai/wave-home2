#include "action_queue.h"

#include <json/json.h>

#include "../app/app_state.h"
#include "../core/logger.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

ActionQueue::ActionQueue() = default;

ActionQueue::~ActionQueue()
{
    stop();
}

void ActionQueue::start(dev::DeviceManager& devices)
{
    if (m_running.exchange(true))
        return;

    m_devices = &devices;
    m_worker = std::thread([this]() { runLoop(); });
}

void ActionQueue::stop()
{
    if (!m_running.exchange(false))
        return;

    {
        std::lock_guard lock(m_mutex);
        m_cv.notify_all();
    }

    if (m_worker.joinable())
        m_worker.join();

    std::lock_guard lock(m_mutex);
    m_queue.clear();
    m_waiters.clear();
    m_repeatJobs.clear();
    m_toggleStates.clear();
    m_devices = nullptr;
}

void ActionQueue::enqueue(ActionJob job)
{
    if (!m_running.load())
    {
        LOG_WARN("ActionQueue: enqueue ignored (not running)");
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(job));
    }
    m_cv.notify_one();
}

std::future<ActionResult> ActionQueue::enqueueAndWait(ActionJob job, uint32_t timeout_ms)
{
    auto pending = std::make_shared<PendingWait>();
    pending->job = std::move(job);
    auto future = pending->promise.get_future();

    if (!m_running.load())
    {
        ActionResult result;
        result.code = "QUEUE_STOPPED";
        pending->promise.set_value(result);
        return future;
    }

    {
        std::lock_guard lock(m_mutex);
        m_waiters.push_back(pending);
    }
    m_cv.notify_one();

    if (timeout_ms == 0)
        return future;

    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::timeout)
    {
        ActionResult result;
        result.code = "TIMEOUT";
        return std::async(std::launch::deferred, [result]() { return result; });
    }

    return future;
}

void ActionQueue::cancelRepeatJobs(const std::string& rule_id)
{
    std::lock_guard lock(m_mutex);
    m_repeatJobs.erase(rule_id);
}

dev::Device* ActionQueue::findDevice(const std::string& external_id) const
{
    if (!m_devices)
        return nullptr;

    const auto id = dev::parseDeviceID(external_id);
    if (id == 0)
        return nullptr;
    return m_devices->findDevice(id);
}

dev::Actionable* ActionQueue::asActionable(dev::Device* device) const
{
    return dynamic_cast<dev::Actionable*>(device);
}

ActionResult ActionQueue::executeJob(const ActionJob& job)
{
    ActionResult result;

    auto* device = findDevice(job.targetDeviceId);
    if (!device)
    {
        result.code = "NOT_FOUND";
        return result;
    }

    if (device->getState() == dev::DeviceState::Initializing)
    {
        result.code = "DEVICE_INITIALIZING";
        return result;
    }

    if (device->getState() != dev::DeviceState::Running)
    {
        result.code = "DEVICE_OFFLINE";
        return result;
    }

    auto* actionable = asActionable(device);
    if (!actionable || !actionable->findAction(job.actionName))
    {
        result.code = "ACTION_NOT_FOUND";
        return result;
    }

    json params = job.params.is_object() ? job.params : json::object();
    std::string action_name = job.actionName;

    if (job.execMode == ExecMode::Toggle && !job.ruleId.empty())
    {
        bool& toggle_on = m_toggleStates[job.ruleId];
        toggle_on = !toggle_on;

        if (actionable->findAction("toggle"))
            action_name = "toggle";
        else if (toggle_on && actionable->findAction("on"))
            action_name = "on";
        else if (!toggle_on && actionable->findAction("off"))
            action_name = "off";
    }

    result.rc = actionable->invoke(action_name, params);
    if (result.rc != 0)
    {
        result.code = "INVOKE_FAILED";
        return result;
    }

    Json::Value detail;
    detail["action"] = action_name;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    if (params.is_object())
    {
        std::istringstream stream(params.dump());
        Json::Value params_json;
        Json::CharReaderBuilder reader;
        std::string errors;
        Json::parseFromStream(reader, stream, &params_json, &errors);
        detail["params"] = params_json;
    }
    else
    {
        detail["params"] = Json::Value(Json::objectValue);
    }

    const std::string device_name = job.deviceName.empty() ? std::string(device->getName()) : job.deviceName;
    const std::string message = job.logMessage.empty()
        ? ("제어: " + action_name)
        : job.logMessage;

    AppState::get().iot.logEvent(
        "execution",
        job.targetDeviceId,
        device_name,
        message,
        job.sourceRef,
        detail);

    result.code.clear();
    return result;
}

void ActionQueue::processRepeatJobs()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& [rule_id, state] : m_repeatJobs)
    {
        (void)rule_id;
        const uint32_t interval = state.job.repeatIntervalMs > 0 ? state.job.repeatIntervalMs : 200;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastFire).count();

        if (elapsed >= static_cast<int64_t>(interval))
        {
            state.lastFire = now;
            m_queue.push_back(state.job);
        }
    }
}

void ActionQueue::runLoop()
{
    while (m_running.load())
    {
        std::deque<ActionJob> batch;
        std::deque<std::shared_ptr<PendingWait>> waiters;

        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(50), [this]()
            {
                return !m_running.load() || !m_queue.empty() || !m_waiters.empty();
            });

            processRepeatJobs();
            batch.swap(m_queue);
            waiters.swap(m_waiters);
        }

        for (auto& pending : waiters)
        {
            const auto result = executeJob(pending->job);
            pending->promise.set_value(result);
        }

        for (auto& job : batch)
        {
            ActionJob stored = job;
            const auto result = executeJob(stored);
            if (stored.execMode == ExecMode::Repeat && !stored.ruleId.empty() && result.rc == 0)
            {
                std::lock_guard lock(m_mutex);
                if (!m_repeatJobs.contains(stored.ruleId))
                {
                    RepeatState state;
                    state.job = std::move(stored);
                    state.lastFire = std::chrono::steady_clock::now();
                    m_repeatJobs.emplace(stored.ruleId, std::move(state));
                }
            }
        }

        if (!m_running.load())
            break;
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
