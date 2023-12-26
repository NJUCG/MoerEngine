//
// Created by 74535 on 2023/10/17.
//

#include "math/Constant.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "VulkanCommand.h"

#include "VulkanDevice.h"
#include "VulkanRHIResource.h"
#include "VulkanDescriptor.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanDebug.h"

#include <vulkan/vulkan_core.h>

VulkanRHICommandListBase::VulkanRHICommandListBase(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level) : VulkanDeviceObject(_device) {
    VkCommandBufferAllocateInfo buffer_alloc_info{};
    m_current_command_pool               = _pool;
    buffer_alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    buffer_alloc_info.pNext              = nullptr;
    buffer_alloc_info.commandPool        = m_current_command_pool;
    buffer_alloc_info.level              = _level;
    buffer_alloc_info.commandBufferCount = 1;

    m_level = _level;

    VK_CHECK_RESULT(vkAllocateCommandBuffers(m_device->GetDevice(), &buffer_alloc_info, &m_command_buffer));
}

VulkanRHICommandListBase::~VulkanRHICommandListBase() {
    vkFreeCommandBuffers(m_device->GetDevice(), m_current_command_pool, 1, &m_command_buffer);
}

void VulkanRHICommandListBase::Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
    vkCmdDispatch(m_command_buffer, _group_count_x, _group_count_y, _group_count_z);
}

void VulkanRHICommandListBase::DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) {
    auto* vk_buffer        = static_cast<const VulkanRHIBuffer*>(_buffer);
    auto* vk_buffer_handle = vk_buffer == nullptr ? nullptr : vk_buffer->GetHandle();
    vkCmdDispatchIndirect(m_command_buffer, vk_buffer_handle, _offset);
}

void VulkanRHICommandListBase::CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) {
    auto* vk_src_buffer = static_cast<const VulkanRHIBuffer*>(_src);
    auto* vk_dst_buffer = static_cast<const VulkanRHIBuffer*>(_dst);
    if (vk_src_buffer == nullptr || vk_dst_buffer == nullptr) {
        LOG_CRITICAL("CopyBuffer: src or dst buffer is nullptr!");
        return;
    }

    Moer::Array<VkBufferCopy> copy_regions(_copy_info.regions.size());
    for (uint32_t i = 0; i < copy_regions.size(); ++i) {
        copy_regions[i].srcOffset = _copy_info.regions[i].src_offset;
        copy_regions[i].dstOffset = _copy_info.regions[i].dst_offset;
        copy_regions[i].size      = _copy_info.regions[i].size;
    }

    vkCmdCopyBuffer(
        m_command_buffer,
        vk_src_buffer->GetHandle(),
        vk_dst_buffer->GetHandle(),
        copy_regions.size(),
        copy_regions.data());
}

void VulkanRHICommandListBase::CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) {
    auto* vk_src_texture = static_cast<const VulkanRHITexture*>(_src);
    auto* vk_dst_texture = static_cast<const VulkanRHITexture*>(_dst);
    if (vk_src_texture == nullptr || vk_dst_texture == nullptr) {
        LOG_CRITICAL("CopyTexture: src or dst texture is nullptr!");
        return;
    }

    VkImageCopy copy_region;
    copy_region.srcSubresource.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_copy_info.src_slice.aspect);
    copy_region.srcSubresource.mipLevel       = _copy_info.src_slice.mip_index;
    copy_region.srcSubresource.baseArrayLayer = _copy_info.src_slice.array_index;
    copy_region.srcSubresource.layerCount     = _copy_info.src_slice.array_count;
    copy_region.srcOffset.x                   = _copy_info.src_offset.x;
    copy_region.srcOffset.y                   = _copy_info.src_offset.y;
    copy_region.srcOffset.z                   = _copy_info.src_offset.z;
    copy_region.dstSubresource.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_copy_info.dst_slice.aspect);
    copy_region.dstSubresource.mipLevel       = _copy_info.dst_slice.mip_index;
    copy_region.dstSubresource.baseArrayLayer = _copy_info.dst_slice.array_index;
    copy_region.dstSubresource.layerCount     = _copy_info.dst_slice.array_count;
    copy_region.dstOffset.x                   = _copy_info.dst_offset.x;
    copy_region.dstOffset.y                   = _copy_info.dst_offset.y;
    copy_region.dstOffset.z                   = _copy_info.dst_offset.z;
    copy_region.extent.width                  = _copy_info.extent.width;
    copy_region.extent.height                 = _copy_info.extent.height;
    copy_region.extent.depth                  = _copy_info.extent.depth;

    vkCmdCopyImage(
        m_command_buffer,
        vk_src_texture->GetHandle(),
        VulkanEnumTranslator::METoVKImageLayout(_copy_info.src_layout),
        vk_dst_texture->GetHandle(),
        VulkanEnumTranslator::METoVKImageLayout(_copy_info.dst_layout),
        1,
        &copy_region);
}

void VulkanRHICommandListBase::CopyBufferToTexture(RHIBuffer*                        src_buffer,
                                                   RHITexture*                       dst_texture,
                                                   const RHICopyBufferToTextureInfo& _info) {
    auto* vk_src_buffer  = static_cast<const VulkanRHIBuffer*>(src_buffer);
    auto* vk_dst_texture = static_cast<const VulkanRHITexture*>(dst_texture);

    VkBufferImageCopy copy_region;
    copy_region.bufferOffset                    = _info.buffer_offset;
    copy_region.bufferRowLength                 = _info.buffer_row_length;
    copy_region.bufferImageHeight               = _info.buffer_texture_height;
    copy_region.imageSubresource.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_info.texture_slice.aspect);
    copy_region.imageSubresource.mipLevel       = _info.texture_slice.mip_index;
    copy_region.imageSubresource.baseArrayLayer = _info.texture_slice.array_index;
    copy_region.imageSubresource.layerCount     = _info.texture_slice.array_count;
    copy_region.imageOffset.x                   = _info.texture_offset.x;
    copy_region.imageOffset.y                   = _info.texture_offset.y;
    copy_region.imageOffset.z                   = _info.texture_offset.z;
    copy_region.imageExtent.width               = _info.texture_extent.width;
    copy_region.imageExtent.height              = _info.texture_extent.height;
    copy_region.imageExtent.depth               = _info.texture_extent.depth;

    vkCmdCopyBufferToImage(
        m_command_buffer,
        vk_src_buffer->GetHandle(),
        vk_dst_texture->GetHandle(),
        VulkanEnumTranslator::METoVKImageLayout(_info.dst_layout),
        1,
        &copy_region);
}

void VulkanRHICommandListBase::CopyTextureToBuffer(RHITexture*                       src_texture,
                                                   RHIBuffer*                        dst_buffer,
                                                   const RHICopyTextureToBufferInfo& _info) {
    auto* vk_src_texture = static_cast<const VulkanRHITexture*>(src_texture);
    auto* vk_dst_buffer  = static_cast<const VulkanRHIBuffer*>(dst_buffer);

    VkBufferImageCopy copy_region;
    copy_region.bufferOffset                    = _info.buffer_offset;
    copy_region.bufferRowLength                 = _info.buffer_row_length;
    copy_region.bufferImageHeight               = _info.buffer_texture_height;
    copy_region.imageSubresource.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_info.texture_slice.aspect);
    copy_region.imageSubresource.mipLevel       = _info.texture_slice.mip_index;
    copy_region.imageSubresource.baseArrayLayer = _info.texture_slice.array_index;
    copy_region.imageSubresource.layerCount     = _info.texture_slice.array_count;
    copy_region.imageOffset.x                   = _info.texture_offset.x;
    copy_region.imageOffset.y                   = _info.texture_offset.y;
    copy_region.imageOffset.z                   = _info.texture_offset.z;
    copy_region.imageExtent.width               = _info.texture_extent.width;
    copy_region.imageExtent.height              = _info.texture_extent.height;
    copy_region.imageExtent.depth               = _info.texture_extent.depth;

    vkCmdCopyImageToBuffer(
        m_command_buffer,
        vk_src_texture->GetHandle(),
        VulkanEnumTranslator::METoVKImageLayout(_info.src_layout),
        vk_dst_buffer->GetHandle(),
        1,
        &copy_region);
}

void VulkanRHICommandListBase::BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) {
    auto* vk_src_texture = static_cast<const VulkanRHITexture*>(_src);
    auto* vk_dst_texture = static_cast<const VulkanRHITexture*>(_dst);
    if (vk_src_texture == nullptr || vk_dst_texture == nullptr) {
        LOG_CRITICAL("BlitTexture: src or dst texture is nullptr!");
        return;
    }
    VkBlitImageInfo2 blit_info{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
    blit_info.srcImage       = vk_src_texture->GetHandle();
    blit_info.srcImageLayout = VulkanEnumTranslator::METoVKImageLayout(_blit_info.src_layout);
    blit_info.dstImage       = vk_dst_texture->GetHandle();
    blit_info.dstImageLayout = VulkanEnumTranslator::METoVKImageLayout(_blit_info.dst_layout);

    VkImageBlit2 blit_region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
    blit_region.srcSubresource.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_blit_info.src_slice.aspect);
    blit_region.srcSubresource.mipLevel       = _blit_info.src_slice.mip_index;
    blit_region.srcSubresource.baseArrayLayer = _blit_info.src_slice.array_index;
    blit_region.srcSubresource.layerCount     = _blit_info.src_slice.array_count;
    blit_region.srcOffsets[0].x               = _blit_info.src_offsets[0].x;
    blit_region.srcOffsets[0].y               = _blit_info.src_offsets[0].y;
    blit_region.srcOffsets[0].z               = _blit_info.src_offsets[0].z;
    blit_region.srcOffsets[1].x               = _blit_info.src_offsets[1].x;
    blit_region.srcOffsets[1].y               = _blit_info.src_offsets[1].y;
    blit_region.srcOffsets[1].z               = _blit_info.src_offsets[1].z;
    blit_region.dstSubresource.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_blit_info.dst_slice.aspect);
    blit_region.dstSubresource.mipLevel       = _blit_info.dst_slice.mip_index;
    blit_region.dstSubresource.baseArrayLayer = _blit_info.dst_slice.array_index;
    blit_region.dstSubresource.layerCount     = _blit_info.dst_slice.array_count;
    blit_region.dstOffsets[0].x               = _blit_info.dst_offsets[0].x;
    blit_region.dstOffsets[0].y               = _blit_info.dst_offsets[0].y;
    blit_region.dstOffsets[0].z               = _blit_info.dst_offsets[0].z;
    blit_region.dstOffsets[1].x               = _blit_info.dst_offsets[1].x;
    blit_region.dstOffsets[1].y               = _blit_info.dst_offsets[1].y;
    blit_region.dstOffsets[1].z               = _blit_info.dst_offsets[1].z;

    blit_info.regionCount = 1;
    blit_info.pRegions    = &blit_region;

    vkCmdBlitImage2(m_command_buffer, &blit_info);
}

void VulkanRHICommandListBase::ResolveTexture(const RHIResolveTextureInfo& _resolove_info, RHITexture* _src, RHITexture* _dst) {
    auto* vk_src_texture = static_cast<const VulkanRHITexture*>(_src);
    auto* vk_dst_texture = static_cast<const VulkanRHITexture*>(_dst);
    if (vk_src_texture == nullptr || vk_dst_texture == nullptr) {
        LOG_CRITICAL("ResolveTexture: src or dst texture is nullptr!");
        return;
    }
    VkResolveImageInfo2 resolve_info{VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2};

    VkImageResolve2 resolve_region{VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2};
    resolve_region.srcSubresource.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_resolove_info.src_slice.aspect);
    resolve_region.srcSubresource.mipLevel       = _resolove_info.src_slice.mip_index;
    resolve_region.srcSubresource.baseArrayLayer = _resolove_info.src_slice.array_index;
    resolve_region.srcSubresource.layerCount     = _resolove_info.src_slice.array_count;
    resolve_region.srcOffset.x                   = _resolove_info.src_offset.x;
    resolve_region.srcOffset.y                   = _resolove_info.src_offset.y;
    resolve_region.srcOffset.z                   = _resolove_info.src_offset.z;
    resolve_region.dstSubresource.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_resolove_info.dst_slice.aspect);
    resolve_region.dstSubresource.mipLevel       = _resolove_info.dst_slice.mip_index;
    resolve_region.dstSubresource.baseArrayLayer = _resolove_info.dst_slice.array_index;
    resolve_region.dstSubresource.layerCount     = _resolove_info.dst_slice.array_count;
    resolve_region.dstOffset.x                   = _resolove_info.dst_offset.x;
    resolve_region.dstOffset.y                   = _resolove_info.dst_offset.y;
    resolve_region.dstOffset.z                   = _resolove_info.dst_offset.z;
    resolve_region.extent.width                  = _resolove_info.extent.width;
    resolve_region.extent.height                 = _resolove_info.extent.height;
    resolve_region.extent.depth                  = _resolove_info.extent.depth;

    resolve_info.srcImage       = vk_src_texture->GetHandle();
    resolve_info.srcImageLayout = VulkanEnumTranslator::METoVKImageLayout(_resolove_info.src_layout);
    resolve_info.dstImage       = vk_dst_texture->GetHandle();
    resolve_info.dstImageLayout = VulkanEnumTranslator::METoVKImageLayout(_resolove_info.dst_layout);
    resolve_info.regionCount    = 1;
    resolve_info.pRegions       = &resolve_region;
    vkCmdResolveImage2(m_command_buffer, &resolve_info);
}

void VulkanRHICommandListBase::SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) {
    Moer::Array<VkMemoryBarrier2>       memory_barriers(_dependency.memory_barriers.size());
    Moer::Array<VkBufferMemoryBarrier2> buffer_barriers(_dependency.buffer_barriers.size());
    Moer::Array<VkImageMemoryBarrier2>  image_barriers(_dependency.texture_barriers.size());

    for (uint32_t i = 0; i < memory_barriers.size(); ++i) {
        memory_barriers[i].sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        memory_barriers[i].pNext         = nullptr;
        memory_barriers[i].srcStageMask  = VulkanEnumTranslator::METoVkPipelineStageFlags2(_dependency.memory_barriers[i].src_stage);
        memory_barriers[i].srcAccessMask = VulkanEnumTranslator::METoVkAccessFlags2(_dependency.memory_barriers[i].src_access);
        memory_barriers[i].dstStageMask  = VulkanEnumTranslator::METoVkPipelineStageFlags2(_dependency.memory_barriers[i].dst_stage);
        memory_barriers[i].dstAccessMask = VulkanEnumTranslator::METoVkAccessFlags2(_dependency.memory_barriers[i].dst_access);
    }

    VulkanRHIBuffer* vk_buffer = nullptr;
    for (uint32_t i = 0; i < buffer_barriers.size(); ++i) {
        vk_buffer = static_cast<VulkanRHIBuffer*>(_dependency.buffer_barriers[i].p_buffer);
        VK_CHECK_NULLPTR(vk_buffer, "SetPipelineBarrier->VkBufferMemoryBarrier2: VulkanRHIBuffer is nullptr!", continue);

        buffer_barriers[i].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        buffer_barriers[i].pNext               = nullptr;
        buffer_barriers[i].srcStageMask        = VulkanEnumTranslator::METoVkPipelineStageFlags2(_dependency.buffer_barriers[i].src_stage);
        buffer_barriers[i].srcAccessMask       = VulkanEnumTranslator::METoVkAccessFlags2(_dependency.buffer_barriers[i].src_access);
        buffer_barriers[i].dstStageMask        = VulkanEnumTranslator::METoVkPipelineStageFlags2(_dependency.buffer_barriers[i].dst_stage);
        buffer_barriers[i].dstAccessMask       = VulkanEnumTranslator::METoVkAccessFlags2(_dependency.buffer_barriers[i].dst_access);
        buffer_barriers[i].srcQueueFamilyIndex = VulkanEnumTranslator::METoVkQueueFamilyIndex(_dependency.buffer_barriers[i].src_queue_type, m_device);
        buffer_barriers[i].dstQueueFamilyIndex = VulkanEnumTranslator::METoVkQueueFamilyIndex(_dependency.buffer_barriers[i].dst_queue_type, m_device);
        buffer_barriers[i].buffer              = vk_buffer->GetHandle();
        buffer_barriers[i].offset              = _dependency.buffer_barriers[i].offset;
        buffer_barriers[i].size                = _dependency.buffer_barriers[i].size == Moer::MAX_INT64 ? VK_WHOLE_SIZE : _dependency.buffer_barriers[i].size;
    }

    VulkanRHITexture* vk_texture = nullptr;
    for (uint32_t i = 0; i < image_barriers.size(); ++i) {
        vk_texture = static_cast<VulkanRHITexture*>(_dependency.texture_barriers[i].p_texture);
        VK_CHECK_NULLPTR(vk_texture, "SetPipelineBarrier->VkImageMemoryBarrier2: VulkanRHITexture is nullptr!", continue);

        image_barriers[i].sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        image_barriers[i].pNext                           = nullptr;
        image_barriers[i].srcStageMask                    = VulkanEnumTranslator::METoVkPipelineStageFlags2(_dependency.texture_barriers[i].src_stage);
        image_barriers[i].srcAccessMask                   = VulkanEnumTranslator::METoVkAccessFlags2(_dependency.texture_barriers[i].src_access);
        image_barriers[i].dstStageMask                    = VulkanEnumTranslator::METoVkPipelineStageFlags2(_dependency.texture_barriers[i].dst_stage);
        image_barriers[i].dstAccessMask                   = VulkanEnumTranslator::METoVkAccessFlags2(_dependency.texture_barriers[i].dst_access);
        image_barriers[i].oldLayout                       = VulkanEnumTranslator::METoVKImageLayout(_dependency.texture_barriers[i].src_layout);
        image_barriers[i].newLayout                       = VulkanEnumTranslator::METoVKImageLayout(_dependency.texture_barriers[i].dst_layout);
        image_barriers[i].srcQueueFamilyIndex             = VulkanEnumTranslator::METoVkQueueFamilyIndex(_dependency.texture_barriers[i].src_queue_type, m_device);
        image_barriers[i].dstQueueFamilyIndex             = VulkanEnumTranslator::METoVkQueueFamilyIndex(_dependency.texture_barriers[i].dst_queue_type, m_device);
        image_barriers[i].image                           = vk_texture->GetHandle();// 2. MARK... layout transition need image has 'VK_IMAGE_USAGE_TRANSFER_DST_BIT'
        image_barriers[i].subresourceRange.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_dependency.texture_barriers[i].sub_resource_range.aspect);
        image_barriers[i].subresourceRange.baseMipLevel   = _dependency.texture_barriers[i].sub_resource_range.mip_index;
        image_barriers[i].subresourceRange.levelCount     = _dependency.texture_barriers[i].sub_resource_range.num_mips == RHISubresourceRange::s_all ? VK_REMAINING_MIP_LEVELS : _dependency.texture_barriers[i].sub_resource_range.num_mips;// 1. MARK... levelCount + baseMipLevel must <= image mip levels
        image_barriers[i].subresourceRange.baseArrayLayer = _dependency.texture_barriers[i].sub_resource_range.array_index;
        image_barriers[i].subresourceRange.layerCount     = _dependency.texture_barriers[i].sub_resource_range.array_count == RHISubresourceRange::s_all ? VK_REMAINING_ARRAY_LAYERS : _dependency.texture_barriers[i].sub_resource_range.array_count;// 1. MARK... layerCount + baseArrayLayer must <= image array layers
    }

    VkDependencyInfo dependency_info{};
    dependency_info.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency_info.pNext                    = nullptr;
    dependency_info.dependencyFlags          = 0;
    dependency_info.memoryBarrierCount       = memory_barriers.size();
    dependency_info.pMemoryBarriers          = memory_barriers.data();
    dependency_info.bufferMemoryBarrierCount = buffer_barriers.size();
    dependency_info.pBufferMemoryBarriers    = buffer_barriers.data();
    dependency_info.imageMemoryBarrierCount  = image_barriers.size();
    dependency_info.pImageMemoryBarriers     = image_barriers.data();

    vkCmdPipelineBarrier2(m_command_buffer, &dependency_info);
}

void VulkanRHICommandListBase::Begin() {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext            = nullptr;
    begin_info.flags            = 0;
    begin_info.pInheritanceInfo = nullptr;

    VK_CHECK_RESULT(vkBeginCommandBuffer(m_command_buffer, &begin_info));
}

void VulkanRHICommandListBase::End() {
    VK_CHECK_RESULT(vkEndCommandBuffer(m_command_buffer));
}

void VulkanRHICommandListBase::Reset() {
    vkResetCommandBuffer(m_command_buffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
}

VulkanRHIGraphicsCommandList::VulkanRHIGraphicsCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level) : VulkanRHICommandListBase(_device, _pool, _level) {}

VulkanRHIGraphicsCommandList::~VulkanRHIGraphicsCommandList() {
}

void VulkanRHIGraphicsCommandList::SetPipelineState(RHIGraphicsPipelineState* _graphics_pso) {
    auto* vk_pso = static_cast<VulkanRHIGraphicsPipelineState*>(_graphics_pso);
    VK_CHECK_NULLPTR(vk_pso, "SetPipelineState: graphics pipeline state is nullptr!", return);

    vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pso->GetHandle());
    m_current_pipeline_state = vk_pso;
}

void VulkanRHIGraphicsCommandList::BeginRecording() {
    VulkanRHICommandListBase::Begin();
    m_bound_sets.clear();
}

void VulkanRHIGraphicsCommandList::EndRecording() {
    VulkanRHICommandListBase::End();
}

void VulkanRHIGraphicsCommandList::Reset() {
    VulkanRHICommandListBase::Reset();
}

void VulkanRHIGraphicsCommandList::ClearState(RHIGraphicsPipelineState* _graphics_pso) {
    // MARK...
    // need to implemented
    auto* vk_pipelie_state = static_cast<const VulkanRHIGraphicsPipelineState*>(_graphics_pso);
    VK_CHECK_NULLPTR(vk_pipelie_state, "ClearState: graphics pipeline state is nullptr!", return);
}

void VulkanRHIGraphicsCommandList::DrawIndexedInstanced(
    uint32_t _index_count,
    uint32_t _instance_count,
    uint32_t _start_index_location,
    uint32_t _start_vertex_location,
    uint32_t _start_instance_location) {

    PrepareDrawCommand();
    Moer::RHI::Vulkan::DebugUtils::CmdInsertLabel(m_command_buffer, "DrawIndexedInstanced", {});
    vkCmdDrawIndexed(
        m_command_buffer,
        _index_count,
        _instance_count,
        _start_index_location,
        _start_vertex_location,
        _start_instance_location);
}

void VulkanRHIGraphicsCommandList::DrawIndexedIndirect(RHIBuffer* _argument_buffer, uint64_t _arg_offset, RHIBuffer* _count_buffer, uint64_t _count_buffer_offset, uint32_t _max_draw_count, uint32_t _stride) {
    auto* vk_arg_buffer   = static_cast<const VulkanRHIBuffer*>(_argument_buffer);
    auto* vk_count_buffer = static_cast<const VulkanRHIBuffer*>(_count_buffer);
    auto* vk_arg_handle   = vk_arg_buffer == nullptr ? nullptr : vk_arg_buffer->GetHandle();
    auto* vk_count_handle = vk_count_buffer == nullptr ? nullptr : vk_count_buffer->GetHandle();

    vkCmdDrawIndexedIndirectCount(
        m_command_buffer,
        vk_arg_handle,
        _arg_offset,
        vk_count_handle,
        _count_buffer_offset,
        _max_draw_count,
        _stride);
}

void VulkanRHIGraphicsCommandList::Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
    vkCmdDispatch(m_command_buffer, _group_count_x, _group_count_y, _group_count_z);
}

void VulkanRHIGraphicsCommandList::DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) {
    auto* vk_buffer        = static_cast<const VulkanRHIBuffer*>(_buffer);
    auto* vk_buffer_handle = vk_buffer == nullptr ? nullptr : vk_buffer->GetHandle();
    vkCmdDispatchIndirect(m_command_buffer, vk_buffer_handle, _offset);
}

void VulkanRHIGraphicsCommandList::CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) {
    VulkanRHICommandListBase::CopyBuffer(_copy_info, _src, _dst);
}

void VulkanRHIGraphicsCommandList::CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) {
    VulkanRHICommandListBase::CopyTexture(_copy_info, _src, _dst);
}

void VulkanRHIGraphicsCommandList::CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) {
    VulkanRHICommandListBase::CopyBufferToTexture(src_buffer, dst_texture, _info);
}

void VulkanRHIGraphicsCommandList::CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) {
    VulkanRHICommandListBase::CopyTextureToBuffer(src_texture, dst_buffer, _info);
}

void VulkanRHIGraphicsCommandList::BlitTexture(const RHIBlitTextureInfo& _blit_info,
                                               RHITexture*               _src,
                                               RHITexture*               _dst) {
    VulkanRHICommandListBase::BlitTexture(_blit_info, _src, _dst);
}

void VulkanRHIGraphicsCommandList::ResolveTexture(
    const RHIResolveTextureInfo& _resolove_info,
    RHITexture*                  _src,
    RHITexture*                  _dst) {
    VulkanRHICommandListBase::ResolveTexture(_resolove_info, _src, _dst);
}

void VulkanRHIGraphicsCommandList::SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) {
    VulkanRHICommandListBase::SetPipelineBarrier(_dependency);
}

void VulkanRHIGraphicsCommandList::SetCullMode(ERasterizerCullMode _cull_mode) {
    vkCmdSetCullMode(m_command_buffer, VulkanEnumTranslator::METoVKCullModeFlags(_cull_mode));
}

void VulkanRHIGraphicsCommandList::SetPrimitiveTopology(EPrimitiveTopology _topology) {
    vkCmdSetPrimitiveTopology(m_command_buffer, VulkanEnumTranslator::METoVKPrimitiveTopology(_topology));
}

void VulkanRHIGraphicsCommandList::SetViewPorts(uint32_t num_viewports, const ViewPort* p_viewports) {
    Moer::Array<VkViewport> vk_viewports(num_viewports);
    for (uint32_t i = 0; i < num_viewports; ++i) {
        vk_viewports[i].x        = p_viewports[i].x;
        vk_viewports[i].y        = p_viewports[i].y;
        vk_viewports[i].width    = p_viewports[i].width;
        vk_viewports[i].height   = p_viewports[i].height;
        vk_viewports[i].minDepth = p_viewports[i].min_depth;
        vk_viewports[i].maxDepth = p_viewports[i].max_depth;
    }
    vkCmdSetViewportWithCount(m_command_buffer, num_viewports, vk_viewports.data());
}

void VulkanRHIGraphicsCommandList::SetViewPort(const ViewPort& _viewport) {
    VkViewport vk_viewport{};
    vk_viewport.x        = _viewport.x;
    vk_viewport.y        = _viewport.y;
    vk_viewport.width    = _viewport.width;
    vk_viewport.height   = _viewport.height;
    vk_viewport.minDepth = _viewport.min_depth;
    vk_viewport.maxDepth = _viewport.max_depth;
    vk_viewport.y += vk_viewport.height;
    vk_viewport.height = -vk_viewport.height;

    vkCmdSetViewport(m_command_buffer, 0, 1, &vk_viewport);
}

void VulkanRHIGraphicsCommandList::SetScissors(uint32_t num_scissors, const Rect2D* p_scissors) {
    Moer::Array<VkRect2D> vk_scissors(num_scissors);
    for (uint32_t i = 0; i < num_scissors; ++i) {
        vk_scissors[i].offset.x      = p_scissors[i].offset.x;
        vk_scissors[i].offset.y      = p_scissors[i].offset.y;
        vk_scissors[i].extent.width  = p_scissors[i].extent.width;
        vk_scissors[i].extent.height = p_scissors[i].extent.height;
    }

    vkCmdSetScissorWithCount(m_command_buffer, num_scissors, vk_scissors.data());
}

void VulkanRHIGraphicsCommandList::SetScissor(const Rect2D& _scissor) {
    VkRect2D vk_scissor{};
    vk_scissor.offset.x      = _scissor.offset.x;
    vk_scissor.offset.y      = _scissor.offset.y;
    vk_scissor.extent.width  = _scissor.extent.width;
    vk_scissor.extent.height = _scissor.extent.height;
    vkCmdSetScissor(m_command_buffer, 0, 1, &vk_scissor);
}

void VulkanRHIGraphicsCommandList::SetBlendFactors(const float* _factors) {
    vkCmdSetBlendConstants(m_command_buffer, _factors);
}

void VulkanRHIGraphicsCommandList::BindVertexBuffers(uint32_t _start_index, uint32_t _num_buffers, const RHIBufferRef* p_vertex_buffers, const uint32_t* _offsets) {
    Moer::Array<VkBuffer>     buffers(_num_buffers);
    Moer::Array<VkDeviceSize> offsets(_num_buffers);
    for (uint32_t i = 0; i < _num_buffers; ++i) {
        auto* vk_buffer = static_cast<const VulkanRHIBuffer*>(p_vertex_buffers[i].Get());
        VK_CHECK_NULLPTR(vk_buffer, "BindVertexBuffers: vertex buffer is nullptr!", continue);
        buffers[i] = vk_buffer->GetHandle();
        offsets[i] = _offsets[i];
    }
    vkCmdBindVertexBuffers(m_command_buffer, _start_index, _num_buffers, buffers.data(), offsets.data());
}

void VulkanRHIGraphicsCommandList::BindIndexBuffer(const RHIBuffer* p_index_buffer, uint32_t _offset, EIndexElementType _type) {
    auto* vk_index_buffer = static_cast<const VulkanRHIBuffer*>(p_index_buffer);
    VK_CHECK_NULLPTR(vk_index_buffer, "BindIndexBuffer: index buffer is nullptr!", return);
    vkCmdBindIndexBuffer(
        m_command_buffer,
        vk_index_buffer->GetHandle(),
        _offset,
        VulkanRHIBuffer::METoVKIndexType(_type));
}

void VulkanRHIGraphicsCommandList::ClearDepthStencil() {
    // MARK...
    // to-be implemented
}

void VulkanRHIGraphicsCommandList::ClearUAVInt(RHIUnorderedAccessView* _uav, const Moer::Vector4i& _values) {
    // MARK...
    auto* vk_uav = static_cast<VulkanRHIUnorderedAccessView*>(_uav);
    VK_CHECK_NULLPTR(vk_uav, "ClearUAVInt: uav is nullptr!", return);
}

void VulkanRHIGraphicsCommandList::ClearUAVFloat(RHIUnorderedAccessView* _uav, const Moer::Vector4f& _values) {
    // MARK...
    auto* vk_uav = static_cast<VulkanRHIUnorderedAccessView*>(_uav);
    VK_CHECK_NULLPTR(vk_uav, "ClearUAVFloat: uav is nullptr!", return);
}

void VulkanRHIGraphicsCommandList::BeginRenderPass(const RHIRenderPassInfo& _pass_info, const char* _pass_name) {
    VkRenderingInfo dynamic_rendering_info{};
    dynamic_rendering_info.sType                    = VK_STRUCTURE_TYPE_RENDERING_INFO;
    dynamic_rendering_info.pNext                    = nullptr;
    dynamic_rendering_info.flags                    = 0;
    dynamic_rendering_info.renderArea.offset.x      = _pass_info.render_area.offset.x;
    dynamic_rendering_info.renderArea.offset.y      = _pass_info.render_area.offset.y;
    dynamic_rendering_info.renderArea.extent.width  = _pass_info.render_area.extent.width;
    dynamic_rendering_info.renderArea.extent.height = _pass_info.render_area.extent.height;
    dynamic_rendering_info.layerCount               = _pass_info.multi_view_count == 0 ? 1 : _pass_info.multi_view_count;
    dynamic_rendering_info.viewMask                 = 0;

    const uint32_t num_color_attachments = _pass_info.GetNumColorAttachments();

    Moer::Array<VkRenderingAttachmentInfo> color_attachments(num_color_attachments);
    for (uint32_t i = 0; i < num_color_attachments; ++i) {
        color_attachments[i] = FromColorAttachmentInfo(_pass_info.color_attachments[i]);
    }
    VkRenderingAttachmentInfo depth_stencil_attachment = FromDepthStencilAttachmentInfo(_pass_info.depth_stencil_attachment);

    dynamic_rendering_info.colorAttachmentCount = num_color_attachments;
    dynamic_rendering_info.pColorAttachments    = color_attachments.data();
    dynamic_rendering_info.pDepthAttachment     = depth_stencil_attachment.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_NULL_HANDLE : &depth_stencil_attachment;
    dynamic_rendering_info.pStencilAttachment   = depth_stencil_attachment.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_NULL_HANDLE : &depth_stencil_attachment;
    Moer::RHI::Vulkan::DebugUtils::CmdBeginLabel(m_command_buffer, _pass_name, {});

    vkCmdBeginRendering(m_command_buffer, &dynamic_rendering_info);
}

void VulkanRHIGraphicsCommandList::EndRenderPass() {
    Moer::RHI::Vulkan::DebugUtils::CmdEndLabel(m_command_buffer);
    vkCmdEndRendering(m_command_buffer);
}

void VulkanRHIGraphicsCommandList::NextSubpass() {
}

void VulkanRHIGraphicsCommandList::BeginQuery(RHIRenderQuery* _query) {
}

void VulkanRHIGraphicsCommandList::EndQuery(RHIRenderQuery* _query) {
}

void VulkanRHIGraphicsCommandList::GetQueryData(ERenderQueryType _query_type, uint32_t _first_index, uint32_t _num_queries, RHIBuffer* _dst_buffer, uint64_t _dst_offset) {
}

void VulkanRHIGraphicsCommandList::ExecuteSubCommands(uint32_t _num, RHIGraphicsCommandList* _sub_commands) {
}

#pragma region ray-tracing

void VulkanRHIGraphicsCommandList::BuildAccelerationStructure(RHIBuffer* _instance_data, uint64_t _instance_offset, bool _b_update, RHIBuffer* _scratch, RHIBuffer* _scratch_offset) {
}

#pragma endregion

VkRenderingAttachmentInfo VulkanRHIGraphicsCommandList::FromColorAttachmentInfo(const RHIRenderPassInfo::ColorAttachmentInfo& _color_attachment_info) const {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    auto* texture_view = _color_attachment_info.color_attachment_view.texture_view;

    if (texture_view->IsSRV()) {
        auto* texture_srv         = static_cast<VulkanRHIShaderResourceView*>(texture_view);
        attachment_info.imageView = texture_srv->GetView();
    } else if (texture_view->IsUAV()) {
        auto* texture_uav         = static_cast<VulkanRHIUnorderedAccessView*>(texture_view);
        attachment_info.imageView = texture_uav->GetView();
    } else {
        LOG_CRITICAL("Invalid texture view type: {}.", typeid(*texture_view).name());
        return attachment_info;
    }

    attachment_info.imageLayout                 = VulkanEnumTranslator::METoVKImageLayout(_color_attachment_info.color_attachment_view.required_layout);
    attachment_info.loadOp                      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_color_attachment_info.color_attachment_action));
    attachment_info.storeOp                     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_color_attachment_info.color_attachment_action));
    attachment_info.clearValue.color.float32[0] = _color_attachment_info.color_attachment_view.clear_attachment.value.color.float32[0];
    attachment_info.clearValue.color.float32[1] = _color_attachment_info.color_attachment_view.clear_attachment.value.color.float32[1];
    attachment_info.clearValue.color.float32[2] = _color_attachment_info.color_attachment_view.clear_attachment.value.color.float32[2];
    attachment_info.clearValue.color.float32[3] = _color_attachment_info.color_attachment_view.clear_attachment.value.color.float32[3];

    auto* resolve_texture_view = _color_attachment_info.resolve_attachment_view.texture_view;
    if (resolve_texture_view != nullptr) {
        if (resolve_texture_view->IsSRV()) {
            auto* resolve_texture_srv        = static_cast<VulkanRHIShaderResourceView*>(resolve_texture_view);
            attachment_info.resolveImageView = resolve_texture_srv->GetView();
        } else if (resolve_texture_view->IsUAV()) {
            auto* resolve_texture_uav        = static_cast<VulkanRHIUnorderedAccessView*>(resolve_texture_view);
            attachment_info.resolveImageView = resolve_texture_uav->GetView();
        } else {
            LOG_CRITICAL("Invalid resolve texture view type: {}.", typeid(*resolve_texture_view).name());
            return attachment_info;
        }
        attachment_info.resolveMode        = VK_RESOLVE_MODE_AVERAGE_BIT;
        attachment_info.resolveImageLayout = VulkanEnumTranslator::METoVKImageLayout(_color_attachment_info.resolve_attachment_view.required_layout);
    }

    return attachment_info;
}

VkRenderingAttachmentInfo VulkanRHIGraphicsCommandList::FromDepthStencilAttachmentInfo(const RHIRenderPassInfo::DepthStencilAttachmentInfo& _depth_stencil_attachment_info) const {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    auto* depth_stencil_view = _depth_stencil_attachment_info.depth_stencil_attachment_view.texture_view;
    if (depth_stencil_view == nullptr) {
        return attachment_info;
    }
    if (depth_stencil_view->IsSRV()) {
        auto* depth_stencil_srv   = static_cast<VulkanRHIShaderResourceView*>(depth_stencil_view);
        attachment_info.imageView = depth_stencil_srv->GetView();
    } else if (depth_stencil_view->IsUAV()) {
        auto* depth_stencil_uav   = static_cast<VulkanRHIUnorderedAccessView*>(depth_stencil_view);
        attachment_info.imageView = depth_stencil_uav->GetView();
    } else {
        LOG_CRITICAL("Invalid depth view type: {}.", typeid(*depth_stencil_view).name());
        return attachment_info;
    }

    attachment_info.imageLayout                     = VulkanEnumTranslator::METoVKImageLayout(_depth_stencil_attachment_info.depth_stencil_attachment_view.required_layout);
    attachment_info.loadOp                          = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_depth_stencil_attachment_info.depth_stencil_action));
    attachment_info.storeOp                         = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_depth_stencil_attachment_info.depth_stencil_action));
    attachment_info.clearValue.depthStencil.depth   = _depth_stencil_attachment_info.depth_stencil_attachment_view.clear_attachment.value.depth_stencil.depth;
    attachment_info.clearValue.depthStencil.stencil = _depth_stencil_attachment_info.depth_stencil_attachment_view.clear_attachment.value.depth_stencil.stencil;

    return attachment_info;
}

void VulkanRHIGraphicsCommandList::PrepareDrawCommand() {
    const auto* vk_pso = m_current_pipeline_state;
    VK_CHECK_NULLPTR(vk_pso, "PreDrawCommand: graphics pipeline state is nullptr!", return);
    auto* vk_resource_cache = vk_pso->GetPipelineResourceCache();
    VK_CHECK_NULLPTR(vk_resource_cache, "PreDrawCommand: graphics pipeline resource cache is nullptr!", return);

    auto pipeline_layout = vk_pso->GetPipelineLayout();

    const auto* vk_sets_layout = vk_pso->GetDescriptorSetsLayout();
    // 1. update and bind descriptor sets
    if (vk_resource_cache->HasDescriptorSets()) {
        vk_resource_cache->UpdateDescriptorSets(m_device, vk_sets_layout);
        if (m_bound_sets != vk_resource_cache->GetDescriptorSets()) {
            vk_resource_cache->BindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout);
            m_bound_sets = vk_resource_cache->GetDescriptorSets();
        }
    }

    // 2. push constants
    if (vk_resource_cache->HasPushConstants()) {
        for (const auto& constant_info : vk_resource_cache->GetConstantsToPush()) {
            vkCmdPushConstants(
                m_command_buffer,
                pipeline_layout,
                constant_info.flags,
                constant_info.byte_offset_in_raw_data,
                constant_info.size,
                constant_info.raw_data.data());
        }
        vk_resource_cache->ResetToPush();
    }
}

VulkanRHIComputeCommandList::VulkanRHIComputeCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level) : VulkanRHICommandListBase(_device, _pool, _level) {}

VulkanRHIComputeCommandList::~VulkanRHIComputeCommandList() {
}

void VulkanRHIComputeCommandList::BeginRecording() {
    VulkanRHICommandListBase::Begin();
}

void VulkanRHIComputeCommandList::EndRecording() {
    VulkanRHICommandListBase::End();
}

void VulkanRHIComputeCommandList::Reset() {
    VulkanRHICommandListBase::Reset();
}

void VulkanRHIComputeCommandList::Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
    vkCmdDispatch(m_command_buffer, _group_count_x, _group_count_y, _group_count_z);
}

void VulkanRHIComputeCommandList::DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) {
    auto* vk_buffer        = static_cast<const VulkanRHIBuffer*>(_buffer);
    auto* vk_buffer_handle = vk_buffer == nullptr ? nullptr : vk_buffer->GetHandle();
    vkCmdDispatchIndirect(m_command_buffer, vk_buffer_handle, _offset);
}

void VulkanRHIComputeCommandList::CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) {
    VulkanRHICommandListBase::CopyBuffer(_copy_info, _src, _dst);
}

void VulkanRHIComputeCommandList::CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) {
    VulkanRHICommandListBase::CopyTexture(_copy_info, _src, _dst);
}

void VulkanRHIComputeCommandList::CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) {
    VulkanRHICommandListBase::CopyBufferToTexture(src_buffer, dst_texture, _info);
}

void VulkanRHIComputeCommandList::CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) {
    VulkanRHICommandListBase::CopyTextureToBuffer(src_texture, dst_buffer, _info);
}

void VulkanRHIComputeCommandList::SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) {
    VulkanRHICommandListBase::SetPipelineBarrier(_dependency);
}

VulkanRHICopyCommandList::VulkanRHICopyCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level) : VulkanRHICommandListBase(_device, _pool, _level) {}

VulkanRHICopyCommandList::~VulkanRHICopyCommandList() {
}

void VulkanRHICopyCommandList::BeginRecording() {
    VulkanRHICommandListBase::Begin();
}

void VulkanRHICopyCommandList::EndRecording() {
    VulkanRHICommandListBase::End();
}

void VulkanRHICopyCommandList::Reset() {
    VulkanRHICommandListBase::Reset();
}

void VulkanRHICopyCommandList::CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) {
    VulkanRHICommandListBase::CopyBuffer(_copy_info, _src, _dst);
}

void VulkanRHICopyCommandList::CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) {
    VulkanRHICommandListBase::CopyTexture(_copy_info, _src, _dst);
}

void VulkanRHICopyCommandList::CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) {
    VulkanRHICommandListBase::CopyBufferToTexture(src_buffer, dst_texture, _info);
}

void VulkanRHICopyCommandList::CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) {
    VulkanRHICommandListBase::CopyTextureToBuffer(src_texture, dst_buffer, _info);
}

void VulkanRHICopyCommandList::SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) {
    VulkanRHICommandListBase::SetPipelineBarrier(_dependency);
}