#include "log/LogSystem.h"

#include <deque>
#include <mutex>

namespace Moer::LogSystem {

namespace {

struct ConsoleLogEntryStorage {
    uint64_t                  sequence = 0;
    spdlog::level::level_enum level    = spdlog::level::info;
    std::string              message;
};

struct ConsoleLogStorage {
    std::mutex                         mutex;
    std::deque<ConsoleLogEntryStorage> entries;
    uint64_t                           next_sequence = 1;
};

ConsoleLogStorage& GetConsoleLogStorage() {
    static ConsoleLogStorage storage;
    return storage;
}

void PushConsoleLogLocked(
    ConsoleLogStorage&            storage,
    spdlog::level::level_enum     level,
    std::string_view              message
) {
    std::string text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }

    storage.entries.push_back(ConsoleLogEntryStorage{
        .sequence = storage.next_sequence++,
        .level    = level,
        .message  = std::move(text),
    });

    constexpr size_t k_max_console_entries = 4096;
    while (storage.entries.size() > k_max_console_entries) {
        storage.entries.pop_front();
    }

}

} // namespace

void Init() {

#if !defined(NDEBUG)
    spdlog::set_level(spdlog::level::trace);
#endif
}

void Flush() {
    if (auto* logger = spdlog::default_logger_raw(); logger != nullptr) {
        logger->flush();
    }
}

bool PollConsoleLogs(
    uint64_t&         next_sequence,
    ConsoleLogVisitor visitor,
    void*             user_data,
    size_t            max_count
) {
    if (!visitor || max_count == 0) {
        return false;
    }

    auto& storage = GetConsoleLogStorage();
    std::lock_guard lock(storage.mutex);
    if (storage.entries.empty()) {
        return false;
    }

    const uint64_t first_sequence = storage.entries.front().sequence;
    if (next_sequence < first_sequence) {
        next_sequence = first_sequence;
    }

    bool visited_any = false;
    for (const ConsoleLogEntryStorage& entry : storage.entries) {
        if (entry.sequence < next_sequence) {
            continue;
        }
        const ConsoleLogEntryView view{
            .sequence = entry.sequence,
            .level    = entry.level,
            .message  = entry.message,
        };
        visitor(view, user_data);
        visited_any = true;
        next_sequence = entry.sequence + 1;
        if (--max_count == 0) {
            break;
        }
    }

    return visited_any;
}

void ClearConsoleLogs() {
    auto& storage = GetConsoleLogStorage();
    std::lock_guard lock(storage.mutex);
    storage.entries.clear();
}

void PushConsoleLog(spdlog::level::level_enum level, std::string_view message) {
    auto& storage = GetConsoleLogStorage();
    std::lock_guard lock(storage.mutex);
    PushConsoleLogLocked(storage, level, message);
}

} // namespace Moer::LogSystem
