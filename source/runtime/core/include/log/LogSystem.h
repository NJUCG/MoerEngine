#ifndef MOERENGINE_LOG_SYSTEM_H
#define MOERENGINE_LOG_SYSTEM_H

#include "API_Macro.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(NDEBUG)
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#else
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include "spdlog/spdlog.h"

namespace Moer { namespace LogSystem {

struct ConsoleLogEntryView {
    std::uint64_t             sequence = 0;
    spdlog::level::level_enum level    = spdlog::level::info;
    std::string_view          message;
};

struct ConsoleLogPollResult {
    std::uint64_t next_sequence = 1;
    std::uint64_t dropped_count = 0;
    std::size_t   visited_count = 0;
};

// Views are borrowed only for the visitor invocation. Init installs a
// process-lifetime forwarding default logger: the previous logger and its sink
// storage remain untouched in their owning module, while admitted messages are
// also copied into this channel. This avoids reallocating an owning STL
// container across separate mimalloc heaps. Call Init again at a
// logger-quiescent point after replacing the default logger; spdlog does not
// synchronize default-logger replacement with concurrent producers. Configure
// non-virtual formatter and error-handler state before Init; runtime level and
// flush-level changes remain synchronized to the forwarded logger. Backtrace
// state is not introspectable through spdlog's public API, so enable backtrace
// after Init; dumped entries are forwarded without a second logger-level filter.
using ConsoleLogVisitor = void (*)(const ConsoleLogEntryView& _entry, void* _context);

CORE_API void Init();

// Sequence zero is normalized to one. Entries removed by bounded overwrite or
// ClearConsoleLogs are reported through dropped_count. The visitor runs after
// releasing channel locks and may re-enter logging, polling, or clearing.
[[nodiscard]] CORE_API ConsoleLogPollResult VisitConsoleLogs(
    std::uint64_t     _next_sequence,
    std::size_t       _max_count,
    ConsoleLogVisitor _visitor,
    void*             _context
);

// Clear preserves sequence identity so existing cursors can observe the loss.
CORE_API void ClearConsoleLogs();
}} // namespace Moer::LogSystem

// Active when in debug mode
#define LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)

// Active when in debug & release mode
#define LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARNING(...)  SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

#endif
