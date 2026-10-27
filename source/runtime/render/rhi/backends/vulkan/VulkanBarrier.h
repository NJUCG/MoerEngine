#pragma once

#include "VulkanCommon.h"
#include "rhi/RHICommand.h"

namespace Moer::Render {
class VulkanBuffer;
class VulkanTexture;
class VulkanCommandList;

struct VulkanBarrierBatch {
    Array<VkBufferMemoryBarrier2> buffer_barriers;
    Array<VkImageMemoryBarrier2>  texture_barriers;
    Array<VkMemoryBarrier2>       memory_barriers;

    [[nodiscard]] bool Empty() const noexcept {
        return buffer_barriers.empty() && texture_barriers.empty() && memory_barriers.empty();
    }

    void Record(VulkanCommandList& _cmd_list) const;
};

template<typename TTexture>
[[nodiscard]] constexpr bool TextureSubresourceRangesOverlap(
    const TextureSubresourceKeyT<TTexture>& lhs,
    const TextureSubresourceKeyT<TTexture>& rhs
) {
    const auto overlaps = [](uint8 first_a, uint8 count_a, uint8 first_b, uint8 count_b) {
        const uint16 end_a = static_cast<uint16>(first_a) + static_cast<uint16>(count_a);
        const uint16 end_b = static_cast<uint16>(first_b) + static_cast<uint16>(count_b);
        return static_cast<uint16>(first_a) < end_b && static_cast<uint16>(first_b) < end_a;
    };
    return lhs.texture == rhs.texture &&
           overlaps(lhs.mip_level, lhs.mip_count, rhs.mip_level, rhs.mip_count) &&
           overlaps(lhs.array_layer, lhs.array_count, rhs.array_layer, rhs.array_count);
}

template<typename TBuffer>
struct BufferByteRangeT {
    TBuffer* buffer{nullptr};
    uint64   offset{0};
    uint64   byte_size{0};
};

template<typename TBuffer>
[[nodiscard]] constexpr bool
BufferByteRangesOverlap(const BufferByteRangeT<TBuffer>& lhs, const BufferByteRangeT<TBuffer>& rhs) {
    if (lhs.buffer != rhs.buffer || lhs.byte_size == 0 || rhs.byte_size == 0) {
        return false;
    }
    return lhs.offset <= rhs.offset ? lhs.byte_size > rhs.offset - lhs.offset :
                                      rhs.byte_size > lhs.offset - rhs.offset;
}

template<typename TTexture>
struct TextureAspectSubresourceRangeT {
    TextureSubresourceKeyT<TTexture> subresource{};
    VkImageAspectFlags               aspects{0};
};

template<typename TTexture>
[[nodiscard]] constexpr bool TextureAspectSubresourceRangesOverlap(
    const TextureAspectSubresourceRangeT<TTexture>& lhs,
    const TextureAspectSubresourceRangeT<TTexture>& rhs
) {
    return (lhs.aspects & rhs.aspects) != 0 &&
           TextureSubresourceRangesOverlap(lhs.subresource, rhs.subresource);
}

// Materializes declared dependencies without inferring application resource state.
// Only release ranges survive Flush: they validate terminal ownership handoffs
// within this recording. Resource lifetime belongs to the command submission.
class VulkanBarrierRecorder {
public:
    void Append(
        VulkanBuffer*              _buffer,
        uint64                     _offset,
        uint64                     _byte_size,
        EBarrierQueueTransferPhase _phase,
        uint32_t                   _src_queue_family,
        uint32_t                   _dst_queue_family,
        VkPipelineStageFlags2      _src_stage,
        VkAccessFlags2             _src_access,
        VkPipelineStageFlags2      _dst_stage,
        VkAccessFlags2             _dst_access
    );
    void Append(
        VulkanTexture*             _texture,
        VkImageAspectFlags         _aspects,
        uint8                      _mip_level,
        uint8                      _mip_count,
        uint8                      _array_layer,
        uint8                      _array_count,
        EBarrierQueueTransferPhase _phase,
        uint32_t                   _src_queue_family,
        uint32_t                   _dst_queue_family,
        VkPipelineStageFlags2      _src_stage,
        VkAccessFlags2             _src_access,
        VkImageLayout              _src_layout,
        VkPipelineStageFlags2      _dst_stage,
        VkAccessFlags2             _dst_access,
        VkImageLayout              _dst_layout
    );

    void Flush(VulkanCommandList& commands);

private:
    VulkanBarrierBatch pending;
    Array<BufferByteRangeT<VulkanBuffer>> explicit_released_buffer_ranges;
    Array<TextureAspectSubresourceRangeT<VulkanTexture>> explicit_released_texture_ranges;
};
} // namespace Moer::Render
