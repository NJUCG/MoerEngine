#include "VulkanDescriptorBufferAllocator.h"
#include "misc/Types.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace Moer::Render {
namespace {

std::optional<uint64> AlignSize(uint64 _size, uint64 _alignment) {
    const uint64 remainder = _size % _alignment;
    const uint64 padding   = remainder == 0 ? 0 : _alignment - remainder;
    if (_size > std::numeric_limits<uint64>::max() - padding) {
        return std::nullopt;
    }
    return _size + padding;
}

} // namespace

VulkanDescriptorBufferAllocator::VulkanDescriptorBufferAllocator(
    uint64 _capacity,
    uint64 _block_size,
    uint64 _range_alignment,
    uint64 _allocation_alignment
) :
    capacity(_capacity),
    block_size(_block_size),
    range_alignment(_range_alignment),
    allocation_alignment(_allocation_alignment ? _allocation_alignment : _block_size) {
    if (capacity == 0 || block_size == 0 || range_alignment == 0 || capacity % block_size != 0 ||
        block_size % range_alignment != 0 || allocation_alignment % range_alignment != 0 ||
        block_size % allocation_alignment != 0) {
        throw std::invalid_argument("invalid descriptor buffer capacity or alignment");
    }
    free_ranges.reserve(1);
    free_ranges.push_back({0, capacity});
}

std::shared_ptr<VulkanDescriptorBufferAllocator::LeaseState>
VulkanDescriptorBufferAllocator::CreateLease() const {
    auto lease   = std::make_shared<LeaseState>();
    lease->owner = this;
    return lease;
}

void VulkanDescriptorBufferAllocator::ValidateLease(const LeaseState& _lease) const {
    if (_lease.owner != this) {
        throw std::logic_error("descriptor lease is retired or belongs to another allocator");
    }
}

std::optional<uint64>
VulkanDescriptorBufferAllocator::ReserveRange(LeaseState& _lease, uint64 _byte_size) {
    std::lock_guard lease_lock(_lease.mutex);
    ValidateLease(_lease);
    if (_byte_size == 0) {
        return 0;
    }
    const auto aligned_size = AlignSize(_byte_size, range_alignment);
    if (!aligned_size || *aligned_size > capacity) {
        return std::nullopt;
    }
    if (!_lease.blocks.empty()) {
        auto& block = _lease.blocks.back();
        if (*aligned_size <= block.range.byte_size - block.used_bytes) {
            const uint64 offset = block.range.byte_offset + block.used_bytes;
            block.used_bytes += *aligned_size;
            return offset;
        }
    }

    const auto reserved_size = AlignSize(*aligned_size, block_size);
    if (!reserved_size || *reserved_size > capacity) {
        return std::nullopt;
    }
    // Prepare bookkeeping before removing space from the free list.
    _lease.blocks.reserve(_lease.blocks.size() + 1);
    std::lock_guard pool_lock(free_ranges_mutex);
    const auto      range = TakeRange(*reserved_size);
    if (!range) {
        return std::nullopt;
    }
    _lease.blocks.push_back({*range, *aligned_size});
    return range->byte_offset;
}

std::optional<VulkanDescriptorBufferAllocator::Range>
VulkanDescriptorBufferAllocator::TakeRange(uint64 _byte_size) {
    const auto it = std::find_if(free_ranges.begin(), free_ranges.end(), [=](const Range& range) {
        return range.byte_size >= _byte_size;
    });
    if (it == free_ranges.end()) {
        return std::nullopt;
    }
    // Reserve before mutation. Any later ordering of releases has enough
    // insertion slots, so retirement never allocates CPU memory.
    const size_t index = static_cast<size_t>(it - free_ranges.begin());
    free_ranges.reserve(free_ranges.size() + live_range_count + 2);
    auto range_it = free_ranges.begin() + index;
    const Range result{range_it->byte_offset, _byte_size};
    range_it->byte_offset += _byte_size;
    range_it->byte_size -= _byte_size;
    if (range_it->byte_size == 0) {
        free_ranges.erase(range_it);
    }
    ++live_range_count;
    return result;
}

VulkanDescriptorBufferAllocator::Cursor::Cursor(const Recording& _recording) :
    ranges(_recording.ranges), alignment(_recording.alignment) {}

std::optional<uint64> VulkanDescriptorBufferAllocator::Cursor::Allocate(uint64 _byte_size) {
    if (_byte_size == 0) {
        return 0;
    }
    const auto size = AlignSize(_byte_size, alignment);
    if (!size) {
        return std::nullopt;
    }
    // Do not consume cursor state on failure.
    for (size_t next = index; next < ranges.size(); ++next) {
        const uint64 offset = next == index ? used : 0;
        if (*size <= ranges[next].byte_size - offset) {
            index = next;
            used = offset + *size;
            return ranges[next].byte_offset + offset;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<VulkanDescriptorBufferAllocator::Recording>>
VulkanDescriptorBufferAllocator::PrepareRecordings(
    LeaseState&                          _lease,
    std::span<const std::vector<uint64>> _requests
) {
    std::lock_guard lease_lock(_lease.mutex);
    ValidateLease(_lease);
    std::lock_guard pool_lock(free_ranges_mutex);

    // Work on a candidate free list. Each entry only loses a prefix, allowing
    // multiple commands to share an atom within this lease, but never across
    // submissions. Only the final prefix is rounded to the flush boundary.
    auto candidate = free_ranges;
    std::vector<Recording> recordings;
    recordings.reserve(_requests.size());
    for (const auto& requests : _requests) {
        auto& recording = recordings.emplace_back();
        recording.alignment = range_alignment;
        for (const auto request : requests) {
            if (request == 0) {
                continue;
            }
            const auto size = AlignSize(request, range_alignment);
            if (!size || *size > capacity) {
                return std::nullopt;
            }
            auto it = std::find_if(candidate.begin(), candidate.end(), [&](const Range& range) {
                return range.byte_size >= *size;
            });
            if (it == candidate.end()) {
                return std::nullopt;
            }
            if (!recording.ranges.empty() &&
                recording.ranges.back().byte_offset + recording.ranges.back().byte_size == it->byte_offset) {
                recording.ranges.back().byte_size += *size;
            } else {
                recording.ranges.push_back({it->byte_offset, *size});
            }
            it->byte_offset += *size;
            it->byte_size -= *size;
        }
    }
    std::vector<LeaseState::Block> blocks;
    for (size_t index = 0; index < candidate.size(); ++index) {
        const auto& original = free_ranges[index];
        auto& remaining = candidate[index];
        const uint64 used      = remaining.byte_offset - original.byte_offset;
        if (used == 0) {
            continue;
        }
        const uint64 reserved = *AlignSize(used, allocation_alignment);
        // Original ranges have atom-aligned boundaries, so rounding fits.
        assert(reserved <= original.byte_size);
        blocks.push_back({{original.byte_offset, reserved}, used});
        remaining = {original.byte_offset + reserved, original.byte_size - reserved};
    }
    std::erase_if(candidate, [](const Range& range) { return range.byte_size == 0; });
    candidate.reserve(candidate.size() + live_range_count + blocks.size() + 1);
    _lease.blocks.reserve(_lease.blocks.size() + blocks.size());
    // Commit only after all potentially throwing allocations have succeeded.
    _lease.blocks.insert(_lease.blocks.end(), blocks.begin(), blocks.end());
    free_ranges.swap(candidate);
    live_range_count += blocks.size();
    return recordings;
}

void VulkanDescriptorBufferAllocator::ReturnRange(Range _range) {
    assert(live_range_count != 0);
    --live_range_count;
    auto it = std::lower_bound(
        free_ranges.begin(), free_ranges.end(), _range.byte_offset, [](const Range& range, uint64 offset) {
            return range.byte_offset < offset;
        }
    );
    assert(_range.byte_offset <= capacity && _range.byte_size <= capacity - _range.byte_offset);
    assert(it == free_ranges.end() || _range.byte_offset + _range.byte_size <= it->byte_offset);
    assert(it == free_ranges.begin() || (it - 1)->byte_offset + (it - 1)->byte_size <= _range.byte_offset);
    if (it != free_ranges.begin() && (it - 1)->byte_offset + (it - 1)->byte_size == _range.byte_offset) {
        --it;
        it->byte_size += _range.byte_size;
    } else {
        it = free_ranges.insert(it, _range);
    }
    const auto next = it + 1;
    if (next != free_ranges.end() && it->byte_offset + it->byte_size == next->byte_offset) {
        it->byte_size += next->byte_size;
        free_ranges.erase(next);
    }
}

VulkanDescriptorBufferAllocator::Usage
VulkanDescriptorBufferAllocator::GetUsage(const LeaseState& _lease) const {
    std::lock_guard lock(_lease.mutex);
    ValidateLease(_lease);
    Usage usage;
    for (const auto& block : _lease.blocks) {
        usage.used_bytes += block.used_bytes;
        usage.reserved_bytes += block.range.byte_size;
    }
    return usage;
}

std::vector<VulkanDescriptorBufferAllocator::Range>
VulkanDescriptorBufferAllocator::GetWrittenRanges(const LeaseState& _lease) const {
    std::lock_guard lock(_lease.mutex);
    ValidateLease(_lease);
    std::vector<Range> ranges;
    ranges.reserve(_lease.blocks.size());
    for (const auto& block : _lease.blocks) {
        if (block.used_bytes != 0) {
            ranges.push_back({block.range.byte_offset, block.used_bytes});
        }
    }
    return ranges;
}

void VulkanDescriptorBufferAllocator::Release(LeaseState& _lease) {
    std::lock_guard lease_lock(_lease.mutex);
    ValidateLease(_lease);
    std::lock_guard pool_lock(free_ranges_mutex);
    for (const auto& block : _lease.blocks) {
        ReturnRange(block.range);
    }
    _lease.blocks.clear();
    _lease.owner = nullptr;
}

} // namespace Moer::Render
