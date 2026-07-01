#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "coredefs.h"

WAVE_NAMESPACE_BEGIN

namespace detail
{
    template<typename... Args>
    std::string formatLog(std::format_string<Args...> fmt, Args&&... args)
    {
        return std::format(fmt, std::forward<Args>(args)...);
    }
}

using log_clock_t = std::chrono::system_clock;
using log_time_t = log_clock_t::time_point;

enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

enum LogMask
{
    LOG_MASK_NONE = 0,
    LOG_MASK_LINE = 1 << 0,
    LOG_MASK_COLUMN = 1 << 1,
    LOG_MASK_FILE = 1 << 2,
    LOG_MASK_FUNCTION = 1 << 3,
    LOG_MASK_MESSAGE = 1 << 4,
    LOG_MASK_DEFAULT = LOG_MASK_LINE | LOG_MASK_COLUMN | LOG_MASK_FUNCTION | LOG_MASK_MESSAGE,
    LOG_MASK_ALL = LOG_MASK_LINE | LOG_MASK_COLUMN | LOG_MASK_FILE | LOG_MASK_FUNCTION | LOG_MASK_MESSAGE,
};

struct LogItem
{
    LogLevel level = LogLevel::Info;
    LogMask mask = LOG_MASK_DEFAULT;
    log_time_t timestamp = log_clock_t::now();
    uint32_t line = 0;
    uint32_t column = 0;
    std::string file;
    std::string function;
    std::string message;
};

class Logger
{
public:
    static void setQueueSize(size_t size = 1024);
    static void addItem(const LogItem& item);

    static void enumerateItems(std::function<void(const LogItem& item)> callback);
    static void enumerateItems(
        log_time_t start,
        log_time_t end,
        std::function<void(const LogItem& item)> callback);

    static void setOutputFunction(std::function<void(const LogItem& item)> outputFunction);

    static void log(
        LogLevel level,
        LogMask mask,
        const char* file,
        const char* function,
        uint32_t line,
        uint32_t column,
        std::string_view message);
};

#define WS_LOG_IMPL(level, fmt, ...) \
    ::ws::Logger::log( \
        (level), \
        ::ws::LOG_MASK_DEFAULT, \
        __FILE__, \
        __FUNCTION__, \
        static_cast<uint32_t>(__LINE__), \
        0u, \
        ::ws::detail::formatLog((fmt) __VA_OPT__(,) __VA_ARGS__))

#define LOG_TRACE(fmt, ...) WS_LOG_IMPL(::ws::LogLevel::Trace, fmt, __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) WS_LOG_IMPL(::ws::LogLevel::Debug, fmt, __VA_ARGS__)
#define LOG_INFO(fmt, ...)  WS_LOG_IMPL(::ws::LogLevel::Info,  fmt, __VA_ARGS__)
#define LOG_WARN(fmt, ...)  WS_LOG_IMPL(::ws::LogLevel::Warn,  fmt, __VA_ARGS__)
#define LOG_ERROR(fmt, ...) WS_LOG_IMPL(::ws::LogLevel::Error, fmt, __VA_ARGS__)
#define LOG_FATAL(fmt, ...) WS_LOG_IMPL(::ws::LogLevel::Fatal, fmt, __VA_ARGS__)

WAVE_NAMESPACE_END
