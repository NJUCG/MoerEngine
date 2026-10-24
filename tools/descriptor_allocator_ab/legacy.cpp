#include "legacy.h"

// ReserveDescriptorRange from 486b3a76 (also 08b04df5^), after its null check.
// Only the class/signature and member-access syntax have been adapted.
std::optional<uint64_t> LegacyReserve(LegacyLease& lease, uint64_t size) {
    uint64_t begin = lease.next_byte_offset.load(std::memory_order_acquire);
    while (true) {
        if (begin > lease.end_byte_offset || size > lease.end_byte_offset - begin) {
            return std::nullopt;
        }
        const uint64_t end = begin + size;
        if (lease.next_byte_offset.compare_exchange_weak(
                begin, end, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return begin;
        }
    }
}
