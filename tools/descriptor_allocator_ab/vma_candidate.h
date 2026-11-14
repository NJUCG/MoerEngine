#pragma once

#include "VulkanCommon.h"
#include "misc/Types.h"
#include <vk_mem_alloc.h>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Experimental per-request TLSF allocation; not wired into the renderer.
// GPU completion / joined-and-discarded recording must authorize Release/Rewind.
class VmaDescriptorCandidate {
public:
    struct Range {
        Moer::uint64 byte_offset, byte_size;
    };
    struct Usage {
        Moer::uint64 used_bytes = 0, reserved_bytes = 0;
    };
    class LeaseState {
        friend class VmaDescriptorCandidate;
        struct Allocation {
            VmaVirtualAllocation handle;
            Range                written;
            Moer::uint64         reserved;
        };
        const VmaDescriptorCandidate* owner = nullptr;
        std::vector<Allocation> allocations;
    };
    class Checkpoint {
        friend class VmaDescriptorCandidate;
        const LeaseState* owner = nullptr;
        size_t count = 0;
    };
    // reuse_alignment is descriptor_alignment for coherent memory. For
    // non-coherent memory it is max(descriptor_alignment, nonCoherentAtomSize).
    // Rounding BOTH offset and size prevents independently flushed leases
    // from sharing a memory atom, at the cost of per-request padding.
    VmaDescriptorCandidate(Moer::uint64 capacity, Moer::uint64 descriptor_alignment, Moer::uint64 reuse_alignment);
    ~VmaDescriptorCandidate();
    VmaDescriptorCandidate(const VmaDescriptorCandidate&) = delete;
    VmaDescriptorCandidate& operator=(const VmaDescriptorCandidate&) = delete;
    std::shared_ptr<LeaseState> CreateLease() const;
    std::optional<Moer::uint64> ReserveRange(LeaseState&, Moer::uint64 bytes);
    Usage GetUsage(const LeaseState&) const;
    std::vector<Range> GetWrittenRanges(const LeaseState&) const;
    Checkpoint CreateCheckpoint(const LeaseState&) const;
    void Rewind(LeaseState&, const Checkpoint&);
    void Release(LeaseState&);
    Moer::uint64                GetAllocatedBytes() const;

private:
    void Validate(const LeaseState&) const;
    // One lock protects the virtual block AND lease bookkeeping. Virtual VMA
    // blocks have no internal synchronization; removing this lock is unsafe.
    mutable std::mutex mutex;
    VmaVirtualBlock block = VK_NULL_HANDLE;
    const Moer::uint64 capacity, descriptor_alignment, reuse_alignment;
};
