#pragma once
#include <atomic>
#include <cstdint>
#include <optional>

// The pre-change per-submission cursor, NOT a simulated old GPU renderer.
// Fixed slot acquisition / waiting is deliberately outside the timed region.
struct LegacyLease {
    uint64_t end_byte_offset{1024 * 1024};
    std::atomic<uint64_t> next_byte_offset{0};
};
std::optional<uint64_t> LegacyReserve(LegacyLease& lease, uint64_t size);
