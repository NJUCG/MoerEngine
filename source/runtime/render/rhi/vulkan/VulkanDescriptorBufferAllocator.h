#pragma once

#include <cstddef>
#include <cstdint>
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
        uint64_t byte_offset;
        uint64_t byte_size;
    };

    struct Usage {
        // PrepareRecordings counts the planned write extent, including any
        // conservative allowance for conditional binds, without per-bind state.
        uint64_t used_bytes{0};
        uint64_t reserved_bytes{0};
    };

    // Immutable segments reserved for one recording command. A context gets its
    // own cursor; no pool or lease lock is needed while writing descriptors.
    struct Recording {
        std::vector<Range> ranges;
        uint64_t alignment{1};
    };

    class Cursor {
    public:
        // Non-owning. Recording metadata and its lease must remain valid until
        // recording stops. Reset/replay is legal only for discarded commands.
        Cursor() = default;
        explicit Cursor(const Recording& _recording);
        std::optional<uint64_t> Allocate(uint64_t _byte_size);

    private:
        std::span<const Range> ranges;
        uint64_t alignment{1};
        size_t   index{0};
        uint64_t used{0};
    };

    class LeaseState {
        friend class VulkanDescriptorBufferAllocator;

        struct Block {
            Range    range;
            uint64_t used_bytes;
        };

        // Null after retirement, so surviving recorder references cannot reuse
        // storage which has already been returned to the allocator.
        const VulkanDescriptorBufferAllocator* owner{nullptr};
        std::vector<Block>                     blocks;
        mutable std::mutex                     mutex;
    };

    VulkanDescriptorBufferAllocator(
        uint64_t _capacity, uint64_t _block_size, uint64_t _range_alignment,
        uint64_t _allocation_alignment = 0
    );

    std::shared_ptr<LeaseState> CreateLease() const;
    // Zero bytes succeed without reserving a block. Exhaustion never waits.
    std::optional<uint64_t> ReserveRange(LeaseState& _lease, uint64_t _byte_size);
    // Each inner array describes successive binds in one command. Reserve the
    // entire batch atomically before recording; a bind never straddles a segment.
    // Failure leaves both the lease and pool unchanged. No waiting occurs here.
    std::optional<std::vector<Recording>> PrepareRecordings(
        LeaseState& _lease, std::span<const std::vector<uint64_t>> _requests
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
    std::optional<Range> TakeRange(uint64_t _byte_size);
    void                 ReturnRange(Range _range);

    const uint64_t     capacity;
    const uint64_t     block_size;
    const uint64_t     range_alignment;
    const uint64_t     allocation_alignment;
    std::vector<Range> free_ranges;
    size_t             live_range_count{0};
    std::mutex         free_ranges_mutex;
};

// A null handle means no lease. Sharing keeps bookkeeping alive; only submission
// completion (or discarded work) may explicitly release its GPU storage.
using VulkanDescriptorBufferLease          = std::shared_ptr<VulkanDescriptorBufferAllocator::LeaseState>;

} // namespace Moer::Render
