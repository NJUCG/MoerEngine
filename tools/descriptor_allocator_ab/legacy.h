#pragma once
#include "misc/Types.h"
#include <atomic>
#include <optional>

// The pre-change per-submission cursor, NOT a simulated old GPU renderer.
// Fixed slot acquisition / waiting is deliberately outside the timed region.
struct LegacyLease {
    Moer::uint64              end_byte_offset{1024 * 1024};
    std::atomic<Moer::uint64> next_byte_offset{0};
};
std::optional<Moer::uint64> LegacyReserve(LegacyLease& lease, Moer::uint64 size);
