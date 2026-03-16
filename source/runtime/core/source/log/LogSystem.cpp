#include "log/LogSystem.h"

namespace Moer::LogSystem {

void Init() {

#if !defined(NDEBUG)
    spdlog::set_level(spdlog::level::trace);
#endif
}

bool PollConsoleLogs(
    uint64_t&                     next_sequence,
    std::vector<ConsoleLogEntry>& out_entries,
    size_t                        max_count
) {
    (void)next_sequence;
    (void)max_count;
    out_entries.clear();
    return false;
}

void ClearConsoleLogs() {}

} // namespace Moer::LogSystem
