#pragma once

#include "misc/Types.h"
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace Moer::Render {

// CPU bookkeeping for one fixed descriptor buffer. The caller owns GPU storage
// and decides when recorded/submitted work is safe to rewind or release.
class VulkanDescriptorBufferAllocator {
public:
    struct Range {
        uint64 byte_offset;
        uint64 byte_size;
    };

    struct Usage {
        // PrepareRecordings counts the planned write extent, including any
        // conservative allowance for conditional binds, without per-bind state.
        uint64 used_bytes{0};
        uint64 reserved_bytes{0};
    };

    // Immutable segments reserved for one recording command. A context gets its
    // own cursor; no pool or lease lock is needed while writing descriptors.
    struct Recording {
        std::vector<Range> ranges;
        uint64             alignment{1};
    };

    class Cursor {
    public:
        // Non-owning. Recording metadata and its lease must remain valid until
        // recording stops. Reset/replay is legal only for discarded commands.
        Cursor() = default;
        explicit Cursor(const Recording& _recording);
        std::optional<uint64> Allocate(uint64 _byte_size);

    private:
        std::span<const Range> ranges;
        uint64                 alignment{1};
        size_t   index{0};
        uint64                 used{0};
    };

    class LeaseState {
        friend class VulkanDescriptorBufferAllocator;

        struct Block {
            Range    range;
            uint64   used_bytes;
        };

        // Null after retirement, so surviving recorder references cannot reuse
        // storage which has already been returned to the allocator.
        const VulkanDescriptorBufferAllocator* owner{nullptr};
        std::vector<Block>                     blocks;
        mutable std::mutex                     mutex;
    };

    VulkanDescriptorBufferAllocator(
        uint64 _capacity,
        uint64 _block_size,
        uint64 _range_alignment,
        uint64 _allocation_alignment = 0
    );

    std::shared_ptr<LeaseState> CreateLease() const;
    // Zero bytes succeed without reserving a block. Exhaustion never waits.
    std::optional<uint64> ReserveRange(LeaseState& _lease, uint64 _byte_size);
    // Each inner array describes successive binds in one command. Reserve the
    // entire batch atomically before recording; a bind never straddles a segment.
    // Failure leaves both the lease and pool unchanged. No waiting occurs here.
    std::optional<std::vector<Recording>> PrepareRecordings(
        LeaseState&                          _lease,
        std::span<const std::vector<uint64>> _requests
    );
    Usage                   GetUsage(const LeaseState& _lease) const;
    // Possible write extents, including prepared but conditionally unused bytes.
    std::vector<Range>      GetWrittenRanges(const LeaseState& _lease) const;

    // Release only after all recorders have joined and their commands have
    // completed on the GPU or have been discarded.
    void       Release(LeaseState& _lease);

private:
    void ValidateLease(const LeaseState& _lease) const;
    // Caller holds free_ranges_mutex. ReturnRange does not allocate memory.
    std::optional<Range> TakeRange(uint64 _byte_size);
    void                 ReturnRange(Range _range);

    const uint64       capacity;
    const uint64       block_size;
    const uint64       range_alignment;
    const uint64       allocation_alignment;
    std::vector<Range> free_ranges;
    size_t             live_range_count{0};
    std::mutex         free_ranges_mutex;
};

// A null handle means no lease. Sharing keeps bookkeeping alive; only submission
// completion (or discarded work) may explicitly release its GPU storage.
using VulkanDescriptorBufferLease          = std::shared_ptr<VulkanDescriptorBufferAllocator::LeaseState>;

} // namespace Moer::Render
