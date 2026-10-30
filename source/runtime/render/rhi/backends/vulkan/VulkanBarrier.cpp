#include "VulkanBarrier.h"
#include "VulkanCommandList.h"
#include "VulkanEnumTranslator.h"
#include "VulkanRHIResource.h"
#include "misc/Types.h"
#include <stdexcept>

namespace Moer::Render {
void VulkanBarrierBatch::Record(VulkanCommandList& _cmd_list) const {
    if (!Empty()) {
        VkDependencyInfoKHR dependency_info{};
        dependency_info.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
        dependency_info.pNext                    = nullptr;
        dependency_info.bufferMemoryBarrierCount = static_cast<uint32>(buffer_barriers.size());
        dependency_info.pBufferMemoryBarriers    = buffer_barriers.data();
        dependency_info.imageMemoryBarrierCount  = static_cast<uint32>(texture_barriers.size());
        dependency_info.pImageMemoryBarriers     = texture_barriers.data();
        dependency_info.memoryBarrierCount       = static_cast<uint32>(memory_barriers.size());
        dependency_info.pMemoryBarriers          = memory_barriers.data();
        vkCmdPipelineBarrier2(_cmd_list.GetHandle(), &dependency_info);
    }
}

void VulkanBarrierRecorder::Append(
    VulkanBuffer*              _buffer,
    uint64                     _offset,
    uint64                     _byte_size,
    EBarrierQueueTransferPhase _phase,
    uint32                     _src_queue_family,
    uint32                     _dst_queue_family,
    VkPipelineStageFlags2      _src_stage,
    VkAccessFlags2             _src_access,
    VkPipelineStageFlags2      _dst_stage,
    VkAccessFlags2             _dst_access
) {
    if (_buffer == nullptr || _byte_size == 0 || _offset > _buffer->GetByteSize() ||
        _byte_size > _buffer->GetByteSize() - _offset) {
        LOG_CRITICAL("Invalid explicit buffer barrier range: offset={} size={}", _offset, _byte_size);
        throw std::out_of_range("invalid explicit buffer barrier range");
    }
    const BufferByteRangeT<VulkanBuffer> transfer_range{
        .buffer    = _buffer,
        .offset    = _offset,
        .byte_size = _byte_size,
    };
    for (const auto& released_range : explicit_released_buffer_ranges) {
        if (BufferByteRangesOverlap(released_range, transfer_range)) {
            throw std::logic_error(
                "a queue-release buffer barrier must be the final overlapping "
                "access in its submit"
            );
        }
    }
    if (_phase != EBarrierQueueTransferPhase::None && _phase != EBarrierQueueTransferPhase::Release &&
        _phase != EBarrierQueueTransferPhase::Acquire) {
        throw std::invalid_argument("explicit buffer barrier carries an unknown transfer phase");
    }
    const bool ownership = _phase != EBarrierQueueTransferPhase::None;
    if ((ownership &&
         (_src_queue_family == VK_QUEUE_FAMILY_IGNORED || _dst_queue_family == VK_QUEUE_FAMILY_IGNORED ||
          _src_queue_family == _dst_queue_family)) ||
        (!ownership &&
         (_src_queue_family != VK_QUEUE_FAMILY_IGNORED || _dst_queue_family != VK_QUEUE_FAMILY_IGNORED))) {
        throw std::invalid_argument("explicit buffer barrier carries an invalid queue-family pair");
    }

    const VkPipelineStageFlags2 native_src_stage =
        _phase == EBarrierQueueTransferPhase::Acquire ? VK_PIPELINE_STAGE_2_NONE : _src_stage;
    const VkAccessFlags2 native_src_access =
        _phase == EBarrierQueueTransferPhase::Acquire ? VK_ACCESS_2_NONE : _src_access;
    const VkPipelineStageFlags2 native_dst_stage =
        _phase == EBarrierQueueTransferPhase::Release ? VK_PIPELINE_STAGE_2_NONE : _dst_stage;
    const VkAccessFlags2 native_dst_access =
        _phase == EBarrierQueueTransferPhase::Release ? VK_ACCESS_2_NONE : _dst_access;

    pending.buffer_barriers.emplace_back(
        VkBufferMemoryBarrier2{
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = native_src_stage,
            .srcAccessMask       = native_src_access,
            .dstStageMask        = native_dst_stage,
            .dstAccessMask       = native_dst_access,
            .srcQueueFamilyIndex = _src_queue_family,
            .dstQueueFamilyIndex = _dst_queue_family,
            .buffer              = _buffer->GetHandle(),
            .offset              = _offset,
            .size                = _byte_size,
        }
    );

    if (_phase == EBarrierQueueTransferPhase::Release) {
        explicit_released_buffer_ranges.emplace_back(transfer_range);
        return;
    }
}

void VulkanBarrierRecorder::Append(
    VulkanTexture*             _texture,
    VkImageAspectFlags         _aspects,
    uint8                      _mip_level,
    uint8                      _mip_count,
    uint8                      _array_layer,
    uint8                      _array_count,
    EBarrierQueueTransferPhase _phase,
    uint32                     _src_queue_family,
    uint32                     _dst_queue_family,
    VkPipelineStageFlags2      _src_stage,
    VkAccessFlags2             _src_access,
    VkImageLayout              _src_layout,
    VkPipelineStageFlags2      _dst_stage,
    VkAccessFlags2             _dst_access,
    VkImageLayout              _dst_layout
) {
    if (_texture == nullptr) {
        throw std::invalid_argument("explicit texture barrier requires a texture");
    }
    ValidateSubresourceRange(_texture, _mip_level, _mip_count, _array_layer, _array_count);
    const VkImageAspectFlags available_aspects =
        VulkanEnumTranslator::METoVKImageAspectFlags(_texture->GetAspectFlags());
    if (_aspects == 0 || (_aspects & ~available_aspects) != 0) {
        LOG_CRITICAL(
            "Invalid explicit texture aspect mask: requested={:#x} available={:#x}",
            static_cast<uint32>(_aspects),
            static_cast<uint32>(available_aspects)
        );
        throw std::invalid_argument("explicit texture barrier aspect is unavailable");
    }
    if (_phase != EBarrierQueueTransferPhase::None && _phase != EBarrierQueueTransferPhase::Release &&
        _phase != EBarrierQueueTransferPhase::Acquire) {
        throw std::invalid_argument("explicit texture barrier carries an unknown transfer phase");
    }
    const bool ownership = _phase != EBarrierQueueTransferPhase::None;
    if ((ownership &&
         (_src_queue_family == VK_QUEUE_FAMILY_IGNORED || _dst_queue_family == VK_QUEUE_FAMILY_IGNORED ||
          _src_queue_family == _dst_queue_family)) ||
        (!ownership &&
         (_src_queue_family != VK_QUEUE_FAMILY_IGNORED || _dst_queue_family != VK_QUEUE_FAMILY_IGNORED))) {
        throw std::invalid_argument("explicit texture barrier carries an invalid queue-family pair");
    }

    const uint8 resolved_mip_count =
        _mip_count == kRemainingSubresource ? uint8(_texture->GetNumMips() - _mip_level) : _mip_count;
    const uint8 resolved_array_count =
        _array_count == kRemainingSubresource ? uint8(_texture->GetNumArray() - _array_layer) : _array_count;
    const TextureAspectSubresourceRangeT<VulkanTexture> transfer_range{
        .subresource =
            TextureSubresourceKeyT<VulkanTexture>{_texture, _mip_level, resolved_mip_count, _array_layer, resolved_array_count},
        .aspects = _aspects,
    };
    for (const auto& released_range : explicit_released_texture_ranges) {
        if (TextureAspectSubresourceRangesOverlap(released_range, transfer_range)) {
            throw std::logic_error(
                "a queue-release texture barrier must be the final overlapping "
                "access in its submit"
            );
        }
    }
    const VkPipelineStageFlags2 native_src_stage =
        _phase == EBarrierQueueTransferPhase::Acquire ? VK_PIPELINE_STAGE_2_NONE : _src_stage;
    const VkAccessFlags2 native_src_access =
        _phase == EBarrierQueueTransferPhase::Acquire ? VK_ACCESS_2_NONE : _src_access;
    const VkPipelineStageFlags2 native_dst_stage =
        _phase == EBarrierQueueTransferPhase::Release ? VK_PIPELINE_STAGE_2_NONE : _dst_stage;
    const VkAccessFlags2 native_dst_access =
        _phase == EBarrierQueueTransferPhase::Release ? VK_ACCESS_2_NONE : _dst_access;

    pending.texture_barriers.emplace_back(
        VkImageMemoryBarrier2{
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = native_src_stage,
            .srcAccessMask       = native_src_access,
            .dstStageMask        = native_dst_stage,
            .dstAccessMask       = native_dst_access,
            .oldLayout           = _src_layout,
            .newLayout           = _dst_layout,
            .srcQueueFamilyIndex = _src_queue_family,
            .dstQueueFamilyIndex = _dst_queue_family,
            .image               = _texture->GetHandle(),
            .subresourceRange    = VkImageSubresourceRange{
                .aspectMask     = _aspects,
                .baseMipLevel   = _mip_level,
                .levelCount     = resolved_mip_count,
                .baseArrayLayer = _array_layer,
                .layerCount     = resolved_array_count,
            },
        }
    );

    if (_phase == EBarrierQueueTransferPhase::Release) {
        explicit_released_texture_ranges.emplace_back(transfer_range);
        return;
    }
}

void VulkanBarrierRecorder::Flush(VulkanCommandList& commands) {
    pending.Record(commands);
    pending.buffer_barriers.clear();
    pending.texture_barriers.clear();
    pending.memory_barriers.clear();
}
} // namespace Moer::Render
