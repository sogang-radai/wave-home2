#include "task_queue.h"

#include <cassert>

#include <trantor/utils/ConcurrentTaskQueue.h>

WAVE_NAMESPACE_BEGIN

namespace
{
    static TaskQueue* s_instance = nullptr;
}

struct TaskQueue::Impl
{
    std::unique_ptr<trantor::ConcurrentTaskQueue> queue;
};

TaskQueue& TaskQueue::get()
{
    assert(s_instance != nullptr);
    return *s_instance;
}

TaskQueue::TaskQueue() :
    m_impl(std::make_unique<Impl>())
{
    assert(s_instance == nullptr);
    s_instance = this;
}

TaskQueue::~TaskQueue()
{
    shutdown();
    s_instance = nullptr;
}

bool TaskQueue::init(uint32_t thread_count)
{
    if (m_initialized)
        return true;

    m_impl->queue = std::make_unique<trantor::ConcurrentTaskQueue>(
        thread_count, "wave-task-queue");
    m_initialized = true;
    return true;
}

void TaskQueue::shutdown()
{
    if (!m_initialized)
        return;

    m_impl->queue->stop();
    m_impl->queue.reset();
    m_initialized = false;
}

void TaskQueue::enqueue(std::function<void()> task)
{
    assert(m_initialized);
    assert(m_impl->queue != nullptr);
    m_impl->queue->runTaskInQueue(std::move(task));
}

WAVE_NAMESPACE_END
