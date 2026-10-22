#include "VulkanDescriptorBufferAllocator.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace Moer::Render {
namespace {

std::optional<uint64_t> AlignSize(uint64_t _size, uint64_t _alignment) {
    const uint64_t remainder = _size % _alignment;
    const uint64_t padding   = remainder == 0 ? 0 : _alignment - remainder;
    if (_size > std::numeric_limits<uint64_t>::max() - padding) {
        return std::nullopt;
    }
    return _size + padding;
}

} // namespace

VulkanDescriptorBufferAllocator::VulkanDescriptorBufferAllocator(
    uint64_t _capacity,
    uint64_t _block_size,
    uint64_t _range_alignment
) :
    capacity(_capacity),
    block_size(_block_size),
    range_alignment(_range_alignment) {
    if (capacity == 0 || block_size == 0 || range_alignment == 0 || capacity % block_size != 0 ||
        block_size % range_alignment != 0) {
        throw std::invalid_argument("invalid descriptor buffer capacity or alignment");
    }
    // At most one free range per block, plus an insertion slot while coalescing.
    // Retirement and failed-recording cleanup must not allocate CPU memory.
    free_ranges.reserve(capacity / block_size + 1);
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

std::optional<uint64_t>
VulkanDescriptorBufferAllocator::ReserveRange(LeaseState& _lease, uint64_t _byte_size) {
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
            const uint64_t offset = block.range.byte_offset + block.used_bytes;
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
VulkanDescriptorBufferAllocator::TakeRange(uint64_t _byte_size) {
    const auto it = std::find_if(free_ranges.begin(), free_ranges.end(), [=](const Range& range) {
        return range.byte_size >= _byte_size;
    });
    if (it == free_ranges.end()) {
        return std::nullopt;
    }
    const Range result{it->byte_offset, _byte_size};
    it->byte_offset += _byte_size;
    it->byte_size -= _byte_size;
    if (it->byte_size == 0) {
        free_ranges.erase(it);
    }
    return result;
}

void VulkanDescriptorBufferAllocator::ReturnRange(Range _range) {
    auto it = std::lower_bound(
        free_ranges.begin(), free_ranges.end(), _range.byte_offset, [](const Range& range, uint64_t offset) {
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

VulkanDescriptorBufferAllocator::Checkpoint
VulkanDescriptorBufferAllocator::CreateCheckpoint(const LeaseState& _lease) const {
    std::lock_guard lock(_lease.mutex);
    ValidateLease(_lease);
    Checkpoint checkpoint;
    checkpoint.owner                 = &_lease;
    checkpoint.block_count           = _lease.blocks.size();
    checkpoint.last_block_used_bytes = _lease.blocks.empty() ? 0 : _lease.blocks.back().used_bytes;
    return checkpoint;
}

void VulkanDescriptorBufferAllocator::Rewind(LeaseState& _lease, const Checkpoint& _checkpoint) {
    std::lock_guard lease_lock(_lease.mutex);
    ValidateLease(_lease);
    if (_checkpoint.owner != &_lease || _checkpoint.block_count > _lease.blocks.size() ||
        (_checkpoint.block_count != 0 &&
         _checkpoint.last_block_used_bytes > _lease.blocks[_checkpoint.block_count - 1].used_bytes)) {
        throw std::logic_error("invalid descriptor allocation checkpoint");
    }
    std::lock_guard pool_lock(free_ranges_mutex);
    while (_lease.blocks.size() > _checkpoint.block_count) {
        ReturnRange(_lease.blocks.back().range);
        _lease.blocks.pop_back();
    }
    if (!_lease.blocks.empty()) {
        _lease.blocks.back().used_bytes = _checkpoint.last_block_used_bytes;
    }
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
