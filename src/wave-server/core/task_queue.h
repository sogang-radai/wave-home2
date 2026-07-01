#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>

#include "coredefs.h"

WAVE_NAMESPACE_BEGIN

class TaskQueue
{
public:
    static TaskQueue& get();

    TaskQueue();
    ~TaskQueue();

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    bool init(uint32_t thread_count = 1);
    void shutdown();

    template<typename Func, typename... Args>
    static auto enqueueAsync(Func&& func, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>>
    {
        using Res = std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>;

        auto task = std::make_shared<std::packaged_task<Res()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<Res> future = task->get_future();
        get().enqueue([task]() { (*task)(); });
        return future;
    }

private:
    void enqueue(std::function<void()> task);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_initialized = false;
};

WAVE_NAMESPACE_END
