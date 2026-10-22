#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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
        uint64_t used_bytes{0};
        uint64_t reserved_bytes{0};
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

    class Checkpoint {
        friend class VulkanDescriptorBufferAllocator;
        const LeaseState* owner{nullptr};
        size_t            block_count{0};
        uint64_t          last_block_used_bytes{0};
    };

    VulkanDescriptorBufferAllocator(uint64_t _capacity, uint64_t _block_size, uint64_t _range_alignment);

    std::shared_ptr<LeaseState> CreateLease() const;
    // Zero bytes succeed without reserving a block. Exhaustion never waits.
    std::optional<uint64_t> ReserveRange(LeaseState& _lease, uint64_t _byte_size);
    Usage                   GetUsage(const LeaseState& _lease) const;
    std::vector<Range>      GetWrittenRanges(const LeaseState& _lease) const;

    // Checkpoints, rewind and release require all relevant recorders to have
    // joined. Rewind additionally requires their recorded commands to be discarded.
    Checkpoint CreateCheckpoint(const LeaseState& _lease) const;
    void       Rewind(LeaseState& _lease, const Checkpoint& _checkpoint);
    void       Release(LeaseState& _lease);

private:
    void ValidateLease(const LeaseState& _lease) const;
    // Caller holds free_ranges_mutex. ReturnRange does not allocate memory.
    std::optional<Range> TakeRange(uint64_t _byte_size);
    void                 ReturnRange(Range _range);

    const uint64_t     capacity;
    const uint64_t     block_size;
    const uint64_t     range_alignment;
    std::vector<Range> free_ranges;
    std::mutex         free_ranges_mutex;
};

} // namespace Moer::Render
