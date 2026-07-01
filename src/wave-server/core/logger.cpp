#include "logger.h"

#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

WAVE_NAMESPACE_BEGIN

namespace
{
    struct LevelStyle
    {
        const char* label;
        const char* color;
        const char* messageColor;
    };

    constexpr const char* kReset = "\033[0m";
    constexpr const char* kDim = "\033[2m";

    const LevelStyle& levelStyle(LogLevel level)
    {
        static const LevelStyle styles[] = {
            {"TRACE", "\033[90m", "\033[90m"},
            {"DEBUG", "\033[36m", "\033[36m"},
            {"INFO",  "\033[32m", "\033[37m"},
            {"WARN",  "\033[33m", "\033[33m"},
            {"ERROR", "\033[31m", "\033[31m"},
            {"FATAL", "\033[1;31m", "\033[1;37m"},
        };

        const auto index = static_cast<size_t>(level);
        if (index >= sizeof(styles) / sizeof(styles[0]))
            return styles[static_cast<size_t>(LogLevel::Info)];

        return styles[index];
    }

    struct LoggerState
    {
        std::mutex mutex;
        std::deque<LogItem> items;
        size_t queueSize = 1024;
        std::function<void(const LogItem&)> outputFunction;
        bool useDefaultOutput = true;
    };

    LoggerState& state()
    {
        static LoggerState s;
        return s;
    }

    std::string pathBasename(const std::string& path)
    {
        const auto pos = path.find_last_of("/\\");
        if (pos == std::string::npos)
            return path;

        return path.substr(pos + 1);
    }

    std::string formatTimestamp(log_time_t timestamp)
    {
        const auto timeT = log_clock_t::to_time_t(timestamp);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()) % 1000;

        std::tm localTime{};
#if defined(_WIN32)
        localtime_s(&localTime, &timeT);
#else
        localtime_r(&timeT, &localTime);
#endif

        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);

        std::ostringstream oss;
        oss << buffer << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    void appendLocation(std::ostringstream& oss, const LogItem& item)
    {
        bool hasLocation = false;

        if ((item.mask & LOG_MASK_FILE) != 0 && !item.file.empty())
        {
            oss << pathBasename(item.file);
            hasLocation = true;
        }

        if ((item.mask & LOG_MASK_LINE) != 0)
        {
            if (hasLocation)
                oss << ':';

            oss << item.line;
            hasLocation = true;
        }

        if ((item.mask & LOG_MASK_COLUMN) != 0)
        {
            if (hasLocation)
                oss << ':';

            oss << item.column;
            hasLocation = true;
        }

        if ((item.mask & LOG_MASK_FUNCTION) != 0 && !item.function.empty())
        {
            if (hasLocation)
                oss << ' ';

            oss << item.function << "()";
            hasLocation = true;
        }

        if (hasLocation && (item.mask & LOG_MASK_MESSAGE) != 0 && !item.message.empty())
            oss << " | ";
    }

    void defaultOutput(const LogItem& item)
    {
        const auto& style = levelStyle(item.level);

        std::ostringstream oss;
        oss << kDim << formatTimestamp(item.timestamp) << kReset << ' ';
        oss << style.color << '[' << style.label << ']' << kReset << ' ';

        appendLocation(oss, item);

        if ((item.mask & LOG_MASK_MESSAGE) != 0 && !item.message.empty())
            oss << style.messageColor << item.message << kReset;

        std::cerr << oss.str() << std::endl;

        if (item.level == LogLevel::Fatal)
            std::abort();
    }

    void emitItem(const LogItem& item)
    {
        auto& loggerState = state();
        if (loggerState.useDefaultOutput || !loggerState.outputFunction)
            defaultOutput(item);
        else
            loggerState.outputFunction(item);
    }
}

void Logger::setQueueSize(size_t size)
{
    auto& loggerState = state();
    std::lock_guard<std::mutex> lock(loggerState.mutex);

    loggerState.queueSize = size > 0 ? size : 1;
    while (loggerState.items.size() > loggerState.queueSize)
        loggerState.items.pop_front();
}

void Logger::addItem(const LogItem& item)
{
    auto& loggerState = state();
    {
        std::lock_guard<std::mutex> lock(loggerState.mutex);
        loggerState.items.push_back(item);
        while (loggerState.items.size() > loggerState.queueSize)
            loggerState.items.pop_front();
    }

    emitItem(item);
}

void Logger::enumerateItems(std::function<void(const LogItem& item)> callback)
{
    if (!callback)
        return;

    auto& loggerState = state();
    std::lock_guard<std::mutex> lock(loggerState.mutex);
    for (const auto& item : loggerState.items)
        callback(item);
}

void Logger::enumerateItems(
    log_time_t start,
    log_time_t end,
    std::function<void(const LogItem& item)> callback)
{
    if (!callback)
        return;

    auto& loggerState = state();
    std::lock_guard<std::mutex> lock(loggerState.mutex);
    for (const auto& item : loggerState.items)
    {
        if (item.timestamp >= start && item.timestamp <= end)
            callback(item);
    }
}

void Logger::setOutputFunction(std::function<void(const LogItem& item)> outputFunction)
{
    auto& loggerState = state();
    std::lock_guard<std::mutex> lock(loggerState.mutex);

    if (outputFunction)
    {
        loggerState.outputFunction = std::move(outputFunction);
        loggerState.useDefaultOutput = false;
    }
    else
    {
        loggerState.outputFunction = nullptr;
        loggerState.useDefaultOutput = true;
    }
}

void Logger::log(
    LogLevel level,
    LogMask mask,
    const char* file,
    const char* function,
    uint32_t line,
    uint32_t column,
    std::string_view message)
{
    LogItem item;
    item.level = level;
    item.mask = mask;
    item.timestamp = log_clock_t::now();
    item.line = line;
    item.column = column;
    item.file = file ? file : "";
    item.function = function ? function : "";
    item.message = message;

    addItem(item);
}

WAVE_NAMESPACE_END
