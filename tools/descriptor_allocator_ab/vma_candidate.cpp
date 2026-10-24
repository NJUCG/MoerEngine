#define VMA_IMPLEMENTATION 1
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "vma_candidate.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
std::optional<uint64_t> Align(uint64_t size, uint64_t alignment) {
    const uint64_t padding = (alignment - size % alignment) % alignment;
    if (size > std::numeric_limits<uint64_t>::max() - padding) return {};
    return size + padding;
}
bool PowerOfTwo(uint64_t value) { return value && !(value & (value - 1)); }
}

VmaDescriptorCandidate::VmaDescriptorCandidate(uint64_t size, uint64_t alignment, uint64_t reuse)
    : capacity(size), descriptor_alignment(alignment), reuse_alignment(reuse) {
    if (!capacity || !PowerOfTwo(alignment) || !PowerOfTwo(reuse) || reuse < alignment || capacity % reuse)
        throw std::invalid_argument("invalid virtual descriptor pool alignment/capacity");
    VmaVirtualBlockCreateInfo info{};
    info.size = capacity;
    // Default algorithm is TLSF. Linear allocation cannot handle arbitrary
    // interleaved submission retirement while preserving live offsets.
    if (vmaCreateVirtualBlock(&info, &block) != VK_SUCCESS)
        throw std::runtime_error("vmaCreateVirtualBlock failed");
}

VmaDescriptorCandidate::~VmaDescriptorCandidate() {
    // CPU experiment teardown only. This does not authorize GPU reclamation.
    vmaClearVirtualBlock(block);
    vmaDestroyVirtualBlock(block);
}

std::shared_ptr<VmaDescriptorCandidate::LeaseState> VmaDescriptorCandidate::CreateLease() const {
    auto lease = std::make_shared<LeaseState>();
    lease->owner = this;
    return lease;
}

void VmaDescriptorCandidate::Validate(const LeaseState& lease) const {
    if (lease.owner != this) throw std::logic_error("retired or foreign virtual descriptor lease");
}

std::optional<uint64_t> VmaDescriptorCandidate::ReserveRange(LeaseState& lease, uint64_t bytes) {
    std::lock_guard lock(mutex);
    Validate(lease);
    if (!bytes) return 0;
    const auto written = Align(bytes, descriptor_alignment);
    const auto reserved = Align(bytes, reuse_alignment);
    if (!written || !reserved || *reserved > capacity) return {};
    auto& records = lease.allocations;
    // Prepare wrapper bookkeeping BEFORE mutating VMA. Geometric growth avoids
    // quadratic copying for thousands of bindings. This only protects C++
    // bookkeeping OOM: bundled VMA does not gracefully handle its own CPU OOM.
    if (records.size() == records.capacity())
        records.reserve(std::max(size_t{8}, records.size() * 2));
    VmaVirtualAllocationCreateInfo info{};
    info.size = *reserved;
    info.alignment = reuse_alignment;
    VmaVirtualAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    const VkResult result = vmaVirtualAllocate(block, &info, &allocation, &offset);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) return {};
    if (result != VK_SUCCESS) throw std::runtime_error("unexpected virtual allocation failure");
    records.push_back({allocation, {offset, *written}, *reserved});
    return offset;
}

VmaDescriptorCandidate::Usage VmaDescriptorCandidate::GetUsage(const LeaseState& lease) const {
    std::lock_guard lock(mutex);
    Validate(lease);
    Usage usage;
    for (const auto& record : lease.allocations) {
        usage.used_bytes += record.written.byte_size;
        usage.reserved_bytes += record.reserved;
    }
    return usage;
}

std::vector<VmaDescriptorCandidate::Range>
VmaDescriptorCandidate::GetWrittenRanges(const LeaseState& lease) const {
    std::lock_guard lock(mutex);
    Validate(lease);
    std::vector<Range> ranges;
    ranges.reserve(lease.allocations.size());
    for (const auto& record : lease.allocations) {
        // Only join physically adjacent written spans. Never flush across a
        // gap that could belong to another submission.
        if (!ranges.empty() && ranges.back().byte_offset + ranges.back().byte_size == record.written.byte_offset)
            ranges.back().byte_size += record.written.byte_size;
        else ranges.push_back(record.written);
    }
    return ranges;
}

VmaDescriptorCandidate::Checkpoint VmaDescriptorCandidate::CreateCheckpoint(const LeaseState& lease) const {
    std::lock_guard lock(mutex);
    Validate(lease);
    Checkpoint checkpoint;
    checkpoint.owner = &lease;
    checkpoint.count = lease.allocations.size();
    return checkpoint;
}

void VmaDescriptorCandidate::Rewind(LeaseState& lease, const Checkpoint& checkpoint) {
    std::lock_guard lock(mutex);
    Validate(lease);
    if (checkpoint.owner != &lease || checkpoint.count > lease.allocations.size())
        throw std::logic_error("invalid virtual descriptor checkpoint");
    while (lease.allocations.size() > checkpoint.count) {
        vmaVirtualFree(block, lease.allocations.back().handle);
        lease.allocations.pop_back();
    }
}

void VmaDescriptorCandidate::Release(LeaseState& lease) {
    std::lock_guard lock(mutex);
    Validate(lease);
    for (const auto& record : lease.allocations) vmaVirtualFree(block, record.handle);
    lease.allocations.clear();
    lease.owner = nullptr;
}

uint64_t VmaDescriptorCandidate::GetAllocatedBytes() const {
    std::lock_guard lock(mutex);
    VmaStatistics statistics{};
    vmaGetVirtualBlockStatistics(block, &statistics);
    return statistics.allocationBytes;
}
