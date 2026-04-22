#ifndef MOERENGINE_LOG_SYSTEM_H
#define MOERENGINE_LOG_SYSTEM_H

#include "API_Macro.h"
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(NDEBUG)
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#else
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include "spdlog/spdlog.h"

namespace Moer { namespace LogSystem {
CORE_API void Init();
CORE_API void Flush();

struct ConsoleLogEntryView {
    uint64_t                  sequence = 0;
    spdlog::level::level_enum level    = spdlog::level::info;
    std::string_view          message;
};

using ConsoleLogVisitor = void (*)(const ConsoleLogEntryView& entry, void* user_data);

// Visit log entries starting from next_sequence (inclusive).
// next_sequence is updated to the next unread sequence after this call.
CORE_API bool PollConsoleLogs(
    uint64_t&         next_sequence,
    ConsoleLogVisitor visitor,
    void*             user_data,
    size_t            max_count = 256
);

CORE_API void ClearConsoleLogs();
CORE_API void PushConsoleLog(spdlog::level::level_enum level, std::string_view message);

inline void LogBufferedMessage(
    spdlog::source_loc        loc,
    spdlog::level::level_enum level,
    std::string_view          message
) {
    PushConsoleLog(level, message);
    if (auto* logger = spdlog::default_logger_raw(); logger) {
        logger->log(loc, level, spdlog::string_view_t(message.data(), message.size()));
    }
}

inline void LogWithSource(
    spdlog::source_loc        loc,
    spdlog::level::level_enum level,
    const char*               message
) {
    LogBufferedMessage(loc, level, message ? std::string_view(message) : std::string_view());
}

inline void LogWithSource(
    spdlog::source_loc        loc,
    spdlog::level::level_enum level,
    const std::string&        message
) {
    LogBufferedMessage(loc, level, message);
}

inline void LogWithSource(
    spdlog::source_loc        loc,
    spdlog::level::level_enum level,
    std::string_view          message
) {
    LogBufferedMessage(loc, level, message);
}

template<class T, typename std::enable_if<!spdlog::is_convertible_to_any_format_string<const T&>::value, int>::type = 0>
inline void LogWithSource(
    spdlog::source_loc        loc,
    spdlog::level::level_enum level,
    const T&                  message
) {
    std::string formatted = spdlog::fmt_lib::format("{}", message);
    LogBufferedMessage(loc, level, formatted);
}

template<typename... Args>
inline void LogWithSource(
    spdlog::source_loc        loc,
    spdlog::level::level_enum level,
    std::string_view          fmt,
    Args&&...                 args
) {
    std::string message = std::vformat(fmt, std::make_format_args(args...));
    LogBufferedMessage(loc, level, message);
}
}} // namespace Moer::LogSystem

#if defined(SPDLOG_NO_SOURCE_LOC)
#define MOER_LOG_SOURCE_LOC spdlog::source_loc{}
#else
#define MOER_LOG_SOURCE_LOC spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
#define LOG_TRACE(...) ::Moer::LogSystem::LogWithSource(MOER_LOG_SOURCE_LOC, spdlog::level::trace, __VA_ARGS__)
#else
#define LOG_TRACE(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
#define LOG_DEBUG(...) ::Moer::LogSystem::LogWithSource(MOER_LOG_SOURCE_LOC, spdlog::level::debug, __VA_ARGS__)
#else
#define LOG_DEBUG(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
#define LOG_INFO(...) ::Moer::LogSystem::LogWithSource(MOER_LOG_SOURCE_LOC, spdlog::level::info, __VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_WARN
#define LOG_WARNING(...) ::Moer::LogSystem::LogWithSource(MOER_LOG_SOURCE_LOC, spdlog::level::warn, __VA_ARGS__)
#else
#define LOG_WARNING(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_ERROR
#define LOG_ERROR(...) ::Moer::LogSystem::LogWithSource(MOER_LOG_SOURCE_LOC, spdlog::level::err, __VA_ARGS__)
#else
#define LOG_ERROR(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_CRITICAL
#define LOG_CRITICAL(...) ::Moer::LogSystem::LogWithSource(MOER_LOG_SOURCE_LOC, spdlog::level::critical, __VA_ARGS__)
#else
#define LOG_CRITICAL(...) (void)0
#endif

#endif
