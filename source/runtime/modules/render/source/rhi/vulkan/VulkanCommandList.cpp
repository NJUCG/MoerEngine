//
// Created by 74535 on 2023/10/17.
//

#include "PixelFormat.h"
#include "log/LogSystem.h"
#include "math/Constant.h"
#include "misc/STL.h"
#include "resources/ResourceTransition.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/vulkan/VulkanRHI.h"

#include <volk.h>
#include "VulkanMacroUtils.h"
#include "VulkanCommand.h"
#include "VulkanDevice.h"
#include "VulkanRHIResource.h"
#include "VulkanDescriptor.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanDebug.h"

#include "RHICmdReorderer.h"

#include <cstdint>
#include <optional>
#include <stdint.h>
#include <string>
#include <variant>
#include <vector>
namespace Moer::Render {

    struct VkCmdPreprocessor {
        VkTracker& tracker;

        VkCmdPreprocessor(VkTracker& _tracker) : tracker(_tracker) {
        }

        void VisitCmd(const Command* _cmd) {
            assert(_cmd != nullptr);
            switch (_cmd->Type()) {
                case Command::EType::UploadBuffer:
                    Visit(static_cast<const UploadBufferCmd*>(_cmd));
                    break;
                case Command::EType::UploadTexture:
                    Visit(static_cast<const UploadTextureCmd*>(_cmd));
                    break;
                case Command::EType::BufferToBuffer:
                    Visit(static_cast<const CopyBufferCmd*>(_cmd));
                    break;
                case Command::EType::BufferToTexture:
                    Visit(static_cast<const CopyBufferToTextureCmd*>(_cmd));
                    break;
                case Command::EType::TextureToBuffer:
                    Visit(static_cast<const CopyTextureToBufferCmd*>(_cmd));
                    break;
                case Command::EType::TextureToTexture:
                    Visit(static_cast<const CopyTextureCmd*>(_cmd));
                    break;
                case Command::EType::CopyBackBuffer:
                    Visit(static_cast<const CopyBackBufferCmd*>(_cmd));
                    break;
                case Command::EType::ShaderDispatch:
                    Visit(static_cast<const DispatchCmd*>(_cmd));
                    break;
                case Command::EType::SetDrawState:
                    Visit(static_cast<const SetDrawStateCmd*>(_cmd));
                    break;
                case Command::EType::BuildAccel:
                    break;
                case Command::EType::Barrier:
                    Visit(static_cast<const BarrierCmd*>(_cmd));
                    break;
                case Command::EType::Custom:
                    break;
                case Command::EType::UpdateBindlessArray:
                    break;
                default:
                    assert(false && "Invalid command type");
            }
        }

        void Visit(const UploadBufferCmd* _cmd) {
            auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->Handle());
            tracker.RecordState(vk_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        }
        void Visit(const UploadTextureCmd* _cmd) {
            auto* vk_texture = reinterpret_cast<VulkanTexture*>(_cmd->Handle());
            tracker.RecordState(vk_texture, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->MipLevel());
        }
        void Visit(const CopyBufferCmd* _cmd) {
            auto* vk_src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->SrcHandle());
            auto* vk_dst_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->DstHandle());
            tracker.RecordState(vk_src_buffer, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            tracker.RecordState(vk_dst_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        }
        void Visit(const CopyTextureCmd* _cmd) {
            auto* vk_src_texture = reinterpret_cast<VulkanTexture*>(_cmd->SrcHandle());
            auto* vk_dst_texture = reinterpret_cast<VulkanTexture*>(_cmd->DstHandle());
            tracker.RecordState(vk_src_texture, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->SrcMipLevel());
            tracker.RecordState(vk_dst_texture, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->DstMipLevel());
        }
        void Visit(const CopyBufferToTextureCmd* _cmd) {
            auto* vk_src_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd->SrcHandle());
            auto* vk_dst_texture = reinterpret_cast<VulkanTexture*>(_cmd->DstHandle());
            tracker.RecordState(vk_src_buffer, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            tracker.RecordState(vk_dst_texture, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->MipLevel());
        }

        void Visit(const CopyTextureToBufferCmd* _cmd) {
            auto* vk_src_texture = reinterpret_cast<VulkanTexture*>(_cmd->SrcHandle());
            auto* vk_dst_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd->DstHandle());
            tracker.RecordState(vk_src_texture, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->MipLevel());
            tracker.RecordState(vk_dst_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        }

        void Visit(const CopyBackBufferCmd* _cmd) {
            auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->Handle());
            tracker.RecordState(vk_buffer, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        }

        void Visit(const DispatchCmd* _cmd) {
            std::visit([this](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, uint3>) {
                    return;
                } else if constexpr (std::is_same_v<T, BufferView>) {
                    tracker.RecordState(reinterpret_cast<VulkanBuffer*>(_arg.GetBuffer()), VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
                }
            },
                       _cmd->Param());
        }

        // void Visit(const SetParamsCmd* _cmd) {
        // }
        // void Visit(const SetConstantCmd* _cmd) {
        // }

        void Visit(const BarrierCmd* _cmd) {
            for (auto& barrier : _cmd->ReadBuffers()) {
                auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
                tracker.RecordState(vk_buffer, tracker.ReadBuffer(vk_buffer, barrier.state, barrier.pass_type));
            }
            for (auto& barrier : _cmd->WriteBuffers()) {
                auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
                tracker.RecordState(vk_buffer, tracker.WriteBuffer(vk_buffer, barrier.state, barrier.pass_type));
            }

            for (auto& barrier : _cmd->ReadTextures()) {
                auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                tracker.RecordState(vk_texture, tracker.ReadTexture(vk_texture, barrier.state, barrier.pass_type));
            }
            for (auto& barrier : _cmd->WriteTextures()) {
                auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                tracker.RecordState(vk_texture, tracker.WriteTexture(vk_texture, barrier.state, barrier.pass_type));
            }
        }

        void Visit(const SetDrawStateCmd* _cmd) {
            const auto& vbs = _cmd->VertexBuffers();
            for (const auto& vb : vbs) {
                auto* vk_buffer = ResourceCast(vb.first);
                tracker.RecordState(vk_buffer, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
            }
            const auto& ibs = _cmd->IndexBuffers();
            for (const auto& ib : ibs) {
                auto* vk_buffer = ResourceCast(ib.first);
                tracker.RecordState(vk_buffer, VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
            }

            for (const auto& rt : _cmd->RenderPassInfo().color_attachments) {
                auto* vk_texture = ResourceCast(rt.target);
                auto  action     = rt.action;
                bool  b_load     = GetLoadOp(action) == EAttachmentLoadOp::LOAD;
                bool  b_store    = GetStoreOp(action) == EAttachmentStoreOp::STORE;
                tracker.RecordState(
                    vk_texture,
                    (b_load ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : VK_ACCESS_2_NONE) |
                        (b_store ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    0,
                    1);
            }
            if (_cmd->RenderPassInfo().depth_attachment.Valid()) {
                auto* vk_texture      = ResourceCast(_cmd->RenderPassInfo().depth_attachment.target);
                auto  action          = _cmd->RenderPassInfo().depth_attachment.action;
                bool  b_depth_load    = GetLoadOp(GetDepthAction(action)) == EAttachmentLoadOp::LOAD;
                bool  b_depth_store   = GetStoreOp(GetDepthAction(action)) == EAttachmentStoreOp::STORE;
                bool  b_stencil_load  = GetLoadOp(GetStencilAction(action)) == EAttachmentLoadOp::LOAD;
                bool  b_stencil_store = GetStoreOp(GetStencilAction(action)) == EAttachmentStoreOp::STORE;
                bool  b_read          = b_depth_load || b_stencil_load;
                bool  b_write         = b_depth_store || b_stencil_store;
                tracker.RecordState(
                    vk_texture,
                    (b_read ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT : VK_ACCESS_2_NONE) |
                        (b_write ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE),
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    0,
                    1);
            }
        }

        void Visit(const UpdateBindlessArrayCmd* _cmd) {
            //use dispatch in the future
            // auto* vk_bindless_array = reinterpret_cast<VulkanBindlessArray*>(_cmd->Handle());
            // tracker.RecordState(vk_bindless_array, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        }
    };
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
    VkPipelineStageFlagBits2 GetPipelineStageFromPassType(EPassType _pass) {
        switch (_pass) {
            case EPassType::Graphics:
                return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            case EPassType::Compute:
                return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            case EPassType::Copy:
                return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            default:
                assert(false && "Invalid pass type");
                return VK_PIPELINE_STAGE_2_NONE;
        }
    }
    template<bool _is_src>
    void ResolveTextureBarrierInfo(VkImageMemoryBarrier2& _barrier, ETextureStateFlags _state, EPassType _pass) {

        auto& src_access_flags = _is_src ? _barrier.srcAccessMask : _barrier.dstAccessMask;
        auto& src_stage        = _is_src ? _barrier.srcStageMask : _barrier.dstStageMask;
        auto& layout           = _is_src ? _barrier.oldLayout : _barrier.newLayout;

        auto transfer_state_any_or = [&](ETextureStateFlags _tmp_state, VkAccessFlags2 _access, VkPipelineStageFlags2 _stage, VkImageLayout _layout) {
            if (EnumHasAnyFlag(_state, _tmp_state)) {
                src_access_flags |= _access;
                src_stage = _stage;
                layout    = _layout;
            }
        };
        src_stage        = VK_PIPELINE_STAGE_2_NONE;
        src_access_flags = VK_ACCESS_2_NONE;
        transfer_state_any_or(TS_UNORDERED_READ, VK_ACCESS_2_SHADER_READ_BIT, GetPipelineStageFromPassType(_pass), VK_IMAGE_LAYOUT_GENERAL);
        transfer_state_any_or(TS_UNORDERED_WRITE, VK_ACCESS_2_SHADER_WRITE_BIT, GetPipelineStageFromPassType(_pass), VK_IMAGE_LAYOUT_GENERAL);
        transfer_state_any_or(TS_COLOR_ATTACHMENT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, PS_COLOR_ATTACHMENT_OUTPUT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        transfer_state_any_or(TS_RESOLVE_ATTACHMENT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, PS_COLOR_ATTACHMENT_OUTPUT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (src_stage != VK_PIPELINE_STAGE_2_NONE) {
            return;
        }
        switch (_state) {
            case TS_UNDEFINED:
                src_access_flags = VK_ACCESS_2_NONE;
                src_stage        = VK_PIPELINE_STAGE_2_NONE;
                layout           = VK_IMAGE_LAYOUT_UNDEFINED;
                break;
            case TS_TRANSFER_SRC:
                src_access_flags = VK_ACCESS_TRANSFER_READ_BIT;
                src_stage        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                layout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                break;
            case TS_TRANSFER_DST:
                src_access_flags = VK_ACCESS_TRANSFER_WRITE_BIT;
                src_stage        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                layout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                break;
            case TS_SAMPLED:
                src_access_flags = VK_ACCESS_SHADER_READ_BIT;
                src_stage        = GetPipelineStageFromPassType(_pass);
                layout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                break;
            case TS_DEPTH_STENCIL:
                src_access_flags = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                if (_is_src)
                    src_stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                else
                    src_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
                layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                break;
            case TS_PRESENT:
                if constexpr (_is_src) {
                    src_access_flags = VK_ACCESS_MEMORY_READ_BIT;
                } else {
                    src_access_flags = VK_ACCESS_2_NONE;
                }
                layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                break;
            default:
                assert(false && "Invalid texture usage");
        }
    }
    void VulkanRHICommandListBase::TransitionTextureBase(RHITexture* _texture, ETextureStateFlags _state, EPassType _pass_type, uint8_t _mip_level, uint8_t _mip_cnt) {
        auto* vk_texture = static_cast<VulkanRHITexture*>(_texture);
        if (vk_texture == nullptr) {
            LOG_CRITICAL("TransitionTexture: texture is nullptr!");
            return;
        }

        auto [src_usage, src_pass] = _texture->GetTrackedUsage(_mip_level);
        auto is_depth_stencil      = EnumHasAnyFlag(_state, TS_DEPTH_STENCIL) || _texture->GetFormat() == PF_D32_SFLOAT_S8_UINT || _texture->GetFormat() == PF_D24_UNORM_S8_UINT || _texture->GetFormat() == PF_D16_UNORM_S8_UINT;
        auto aspect                = is_depth_stencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

        if (++m_image_barrier_count > m_image_barriers.size()) {
            m_image_barriers.resize(m_image_barrier_count);
        }

        auto& image_barrier               = m_image_barriers[m_image_barrier_count - 1];
        image_barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        image_barrier.pNext               = nullptr;
        image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image               = vk_texture->GetHandle();
        ResolveTextureBarrierInfo<true>(image_barrier, src_usage, src_pass);
        ResolveTextureBarrierInfo<false>(image_barrier, _state, _pass_type);
        VkImageSubresourceRange& subresource_range = image_barrier.subresourceRange;
        subresource_range.aspectMask               = aspect;
        subresource_range.baseMipLevel             = _mip_level;
        subresource_range.levelCount               = _mip_cnt == RHISubresourceRange::s_all ? VK_REMAINING_MIP_LEVELS : _mip_cnt;
        subresource_range.baseArrayLayer           = 0;
        subresource_range.layerCount               = 1;

        RHISubresourceRange range(ETextureAspectFlags(aspect), _mip_level, _mip_cnt, 0, 1, 0, 1);
        _texture->SetTrackInfo(range, _state, _pass_type);
    }

    void VulkanRHICommandListBase::ExecuteTransitionBase() {
        m_dependency_info.bufferMemoryBarrierCount = m_buffer_barrier_count;
        m_dependency_info.pBufferMemoryBarriers    = m_buffer_barriers.data();
        m_dependency_info.imageMemoryBarrierCount  = m_image_barrier_count;
        m_dependency_info.pImageMemoryBarriers     = m_image_barriers.data();
        m_dependency_info.memoryBarrierCount       = m_memory_barrier_count;
        m_dependency_info.pMemoryBarriers          = m_memory_barriers.data();

        vkCmdPipelineBarrier2(m_command_buffer, &m_dependency_info);
        m_buffer_barrier_count = 0;
        m_image_barrier_count  = 0;
        m_memory_barrier_count = 0;
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
            bool dst_present                                  = (image_barriers[i].newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || image_barriers[i].newLayout == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR);
            bool src_present                                  = (image_barriers[i].oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || image_barriers[i].oldLayout == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR);
            auto srv_queue                                    = src_present ? m_device->GetQueueFamilyIndices().present.value() : VulkanEnumTranslator::METoVkQueueFamilyIndex(_dependency.texture_barriers[i].src_queue_type, m_device);
            auto dst_queue                                    = dst_present ? m_device->GetQueueFamilyIndices().present.value() : VulkanEnumTranslator::METoVkQueueFamilyIndex(_dependency.texture_barriers[i].dst_queue_type, m_device);
            image_barriers[i].srcQueueFamilyIndex             = VulkanEnumTranslator::METoVkQueueFamilyIndex(_dependency.texture_barriers[i].src_queue_type, m_device);
            image_barriers[i].dstQueueFamilyIndex             = VulkanEnumTranslator::METoVkQueueFamilyIndex(_dependency.texture_barriers[i].dst_queue_type, m_device);
            image_barriers[i].image                           = vk_texture->GetHandle();// 2. MARK... layout transition need image has 'VK_IMAGE_USAGE_TRANSFER_DST_BIT'
            image_barriers[i].subresourceRange.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(_dependency.texture_barriers[i].sub_resource_range.aspect);
            image_barriers[i].subresourceRange.baseMipLevel   = _dependency.texture_barriers[i].sub_resource_range.mip_index;
            image_barriers[i].subresourceRange.levelCount     = _dependency.texture_barriers[i].sub_resource_range.num_mips == RHISubresourceRange::s_all ? VK_REMAINING_MIP_LEVELS : _dependency.texture_barriers[i].sub_resource_range.num_mips;// 1. MARK... levelCount + baseMipLevel must <= image mip levels
            image_barriers[i].subresourceRange.baseArrayLayer = _dependency.texture_barriers[i].sub_resource_range.array_index;
            image_barriers[i].subresourceRange.layerCount     = _dependency.texture_barriers[i].sub_resource_range.array_count == RHISubresourceRange::s_all ? VK_REMAINING_ARRAY_LAYERS : _dependency.texture_barriers[i].sub_resource_range.array_count;// 1. MARK... layerCount + baseArrayLayer must <= image array layers
            // _dependency.texture_barriers[i].p_texture->SetLayout(_dependency.texture_barriers[i].sub_resource_range, _dependency.texture_barriers[i].dst_layout);
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

    void VulkanRHIGraphicsCommandList::SetPipelineState(RHIGfxPso* _graphics_pso) {
        auto* vk_pso = static_cast<VulkanRHIGraphicsPipelineState*>(_graphics_pso);
        VK_CHECK_NULLPTR(vk_pso, "SetPipelineState: graphics pipeline state is nullptr!", return);
        vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pso->GetHandle());
        m_current_pipeline_state = vk_pso;
        current_pso              = vk_pso;
    }

    // MARK... current_pipeline_state_design
    void VulkanRHIGraphicsCommandList::SetPipelineState(RHIComputePso* _compute_pso) {
        auto* vk_pso = static_cast<VulkanRHIComputePipelineState*>(_compute_pso);
        VK_CHECK_NULLPTR(vk_pso, "SetPipelineState: compute pipeline state is nullptr!", return);

        vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_pso->GetHandle());
        current_pso = vk_pso;
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
        m_current_pipeline_state = nullptr;
        m_bound_sets             = {};
    }

    void VulkanRHIGraphicsCommandList::ClearState(RHIGfxPso* _graphics_pso) {
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
        PrepareDrawCommand();
        Moer::RHI::Vulkan::DebugUtils::CmdInsertLabel(m_command_buffer, "DrawIndexedInstancedIndirect", {});
        vkCmdDrawIndexedIndirectCount(
            m_command_buffer,
            vk_arg_handle,
            _arg_offset,
            vk_count_handle,
            _count_buffer_offset,
            _max_draw_count,
            _stride);
    }

    void VulkanRHIGraphicsCommandList::Draw(uint32_t _vertex_count, uint32_t _instance_count, uint32_t _start_vertex_location, uint32_t _start_instance_location) {
        PrepareDrawCommand();
        Moer::RHI::Vulkan::DebugUtils::CmdInsertLabel(m_command_buffer, "Draw", {});
        vkCmdDraw(m_command_buffer, _vertex_count, _instance_count, _start_vertex_location, _start_instance_location);
    }

    void VulkanRHIGraphicsCommandList::Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
        PrepareDispatch();
        // Moer::RHI::Vulkan::DebugUtils::CmdInsertLabel(m_command_buffer, "Dispatch", {});
        vkCmdDispatch(m_command_buffer, _group_count_x, _group_count_y, _group_count_z);
    }

    void VulkanRHIGraphicsCommandList::DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) {
        auto* vk_buffer        = static_cast<const VulkanRHIBuffer*>(_buffer);
        auto* vk_buffer_handle = vk_buffer == nullptr ? nullptr : vk_buffer->GetHandle();
        PrepareDispatch();
        Moer::RHI::Vulkan::DebugUtils::CmdInsertLabel(m_command_buffer, "DispatchIndirect", {});
        vkCmdDispatchIndirect(m_command_buffer, vk_buffer_handle, _offset);
    }

    void VulkanRHIGraphicsCommandList::TransitionTexture(RHITexture* _texture, ETextureStateFlags _target_state, EPassType _pass_type, uint8_t _mip_level, uint8_t _mip_cnt) {
        VulkanRHICommandListBase::TransitionTextureBase(_texture, _target_state, _pass_type, _mip_level, _mip_cnt);
    }

    void VulkanRHIGraphicsCommandList::ExecuteTransition() {
        VulkanRHICommandListBase::ExecuteTransitionBase();
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
    void VulkanRHIGraphicsCommandList::FillBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size, uint32_t _data) {
        vkCmdFillBuffer(m_command_buffer, static_cast<VulkanRHIBuffer*>(_buffer)->GetHandle(), _offset, _size, _data);
    }

    void VulkanRHIGraphicsCommandList::ClearDepthStencil() {
        // MARK...
        // to-be implemented
    }

    void VulkanRHIGraphicsCommandList::ClearUAVInt(RHIUAV* _uav, const Moer::Vector4i& _values) {
        // MARK...
        auto* vk_uav = static_cast<VulkanRHITextureUAV*>(_uav);
        VK_CHECK_NULLPTR(vk_uav, "ClearUAVInt: uav is nullptr!", return);
    }

    void VulkanRHIGraphicsCommandList::ClearUAVFloat(RHIUAV* _uav, const Moer::Vector4f& _values) {
        // MARK...
        auto* vk_uav = static_cast<VulkanRHITextureUAV*>(_uav);
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

    void VulkanRHIGraphicsCommandList::BeginLabel(const char* _label) {
        Moer::RHI::Vulkan::DebugUtils::CmdBeginLabel(m_command_buffer, _label, {});
    }

    void VulkanRHIGraphicsCommandList::EndLabel() {
        Moer::RHI::Vulkan::DebugUtils::CmdEndLabel(m_command_buffer);
    }

    VkRenderingAttachmentInfo VulkanRHIGraphicsCommandList::FromColorAttachmentInfo(const RHIRenderPassInfo::ColorAttachmentInfo& _color_attachment_info) const {
        VkRenderingAttachmentInfo attachment_info{};
        attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment_info.pNext = nullptr;

        auto* texture_view = _color_attachment_info.color_attachment_view.texture_view;

        if (texture_view->IsSRV()) {
            auto* texture_srv         = static_cast<VulkanRHITextureSRV*>(texture_view);
            attachment_info.imageView = texture_srv->GetView();
        } else if (texture_view->IsUAV()) {
            auto* texture_uav         = static_cast<VulkanRHITextureUAV*>(texture_view);
            attachment_info.imageView = texture_uav->GetView();
        } else {
            LOG_CRITICAL("Invalid texture view type: {}.", typeid(*texture_view).name());
            return attachment_info;
        }

        attachment_info.imageLayout = VulkanEnumTranslator::METoVKImageLayout(_color_attachment_info.color_attachment_view.required_layout);
        attachment_info.loadOp      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_color_attachment_info.color_attachment_action));
        attachment_info.storeOp     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_color_attachment_info.color_attachment_action));

        const auto& color = std::get<RHIClearAttachment::ClearColorValue>(_color_attachment_info.color_attachment_view.clear_attachment.value);
        std::memcpy(attachment_info.clearValue.color.float32, color.float32, sizeof(color.float32));

        auto* resolve_texture_view = _color_attachment_info.resolve_attachment_view.texture_view;
        if (resolve_texture_view != nullptr) {
            if (resolve_texture_view->IsSRV()) {
                auto* resolve_texture_srv        = static_cast<VulkanRHITextureSRV*>(resolve_texture_view);
                attachment_info.resolveImageView = resolve_texture_srv->GetView();
            } else if (resolve_texture_view->IsUAV()) {
                auto* resolve_texture_uav        = static_cast<VulkanRHITextureUAV*>(resolve_texture_view);
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
            auto* depth_stencil_srv   = static_cast<VulkanRHITextureSRV*>(depth_stencil_view);
            attachment_info.imageView = depth_stencil_srv->GetView();
        } else if (depth_stencil_view->IsUAV()) {
            auto* depth_stencil_uav   = static_cast<VulkanRHITextureUAV*>(depth_stencil_view);
            attachment_info.imageView = depth_stencil_uav->GetView();
        } else {
            LOG_CRITICAL("Invalid depth view type: {}.", typeid(*depth_stencil_view).name());
            return attachment_info;
        }

        attachment_info.imageLayout = VulkanEnumTranslator::METoVKImageLayout(_depth_stencil_attachment_info.depth_stencil_attachment_view.required_layout);
        attachment_info.loadOp      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_depth_stencil_attachment_info.depth_stencil_action));
        attachment_info.storeOp     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_depth_stencil_attachment_info.depth_stencil_action));

        const auto& depth_stencil = std::get<RHIClearAttachment::ClearDepthStencilValue>(_depth_stencil_attachment_info.depth_stencil_attachment_view.clear_attachment.value);
        std::memcpy(&attachment_info.clearValue.depthStencil, &depth_stencil, sizeof(depth_stencil));

        return attachment_info;
    }

    void VulkanRHIGraphicsCommandList::PrepareDrawCommand() {
        VulkanPipelineState* vk_pso        = nullptr;
        auto                 binding_point = current_pso.index() == 0 ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
        if (current_pso.index() == 0) {
            vk_pso = std::get<0>(current_pso);
        } else {
            vk_pso = std::get<1>(current_pso);
        }
        VK_CHECK_NULLPTR(vk_pso, "PreDrawCommand: graphics pipeline state is nullptr!", return);
        auto* vk_resource_cache = vk_pso->GetPipelineResourceCache();
        VK_CHECK_NULLPTR(vk_resource_cache, "PreDrawCommand: graphics pipeline resource cache is nullptr!", return);

        auto* pipeline_layout = vk_pso->GetPipelineLayout();

        const auto* vk_sets_layout = vk_pso->GetDescriptorSetsLayout();
        // 1. update and bind descriptor sets
        if (vk_resource_cache->HasDescriptorSets()) {
            vk_resource_cache->UpdateDescriptorSets(vk_sets_layout);
            if (m_bound_sets != vk_resource_cache->GetDescriptorSets()) {
                vk_resource_cache->BindDescriptorSets(m_command_buffer, binding_point, pipeline_layout);
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

    void VulkanRHIGraphicsCommandList::PrepareDispatch() {
        PrepareDrawCommand();
    }

    VulkanRHIComputeCommandList::VulkanRHIComputeCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level) : VulkanRHICommandListBase(_device, _pool, _level) {}

    VulkanRHIComputeCommandList::~VulkanRHIComputeCommandList() {
    }
    void VulkanRHIComputeCommandList::SetPipelineState(RHIComputePso* _compute_pso) {
        auto* vk_pso = static_cast<VulkanRHIComputePipelineState*>(_compute_pso);
        VK_CHECK_NULLPTR(vk_pso, "SetPipelineState: compute pipeline state is nullptr!", return);

        vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_pso->GetHandle());
        m_current_pipeline_state = vk_pso;
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

    void VulkanRHIComputeCommandList::PrepareDispatch() {
        //some works to do
    }

    void VulkanRHIComputeCommandList::Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
        PrepareDispatch();
        Moer::RHI::Vulkan::DebugUtils::CmdInsertLabel(m_command_buffer, "DrawIndexedInstanced", {});
        vkCmdDispatch(m_command_buffer, _group_count_x, _group_count_y, _group_count_z);
    }

    void VulkanRHIComputeCommandList::DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) {
        PrepareDispatchCommand();
        auto* vk_buffer        = static_cast<const VulkanRHIBuffer*>(_buffer);
        auto* vk_buffer_handle = vk_buffer == nullptr ? nullptr : vk_buffer->GetHandle();
        PrepareDispatch();
        Moer::RHI::Vulkan::DebugUtils::CmdInsertLabel(m_command_buffer, "DrawIndexedInstanced", {});
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
    void VulkanRHIComputeCommandList::PrepareDispatchCommand() {
        const auto* vk_pso = m_current_pipeline_state;
        VK_CHECK_NULLPTR(vk_pso, "PrepareDispatchCommand: compute pipeline state is nullptr!", return);
        auto* vk_resource_cache = vk_pso->GetPipelineResourceCache();
        VK_CHECK_NULLPTR(vk_resource_cache, "PrepareDispatchCommand: compute pipeline resource cache is nullptr!", return);

        auto pipeline_layout = vk_pso->GetPipelineLayout();

        const auto* vk_sets_layout = vk_pso->GetDescriptorSetsLayout();
        // 1. update and bind descriptor sets
        if (vk_resource_cache->HasDescriptorSets()) {
            vk_resource_cache->UpdateDescriptorSets(vk_sets_layout);
            if (m_bound_sets != vk_resource_cache->GetDescriptorSets()) {
                vk_resource_cache->BindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout);
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

    VulkanRHIRayTracingCommandList::VulkanRHIRayTracingCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level) : VulkanRHICommandListBase(_device, _pool, _level) {}

    VulkanRHIRayTracingCommandList::~VulkanRHIRayTracingCommandList() {
    }
    void VulkanRHIRayTracingCommandList::SetPipelineState(RHIRTPso* _raytracing_pso) {
        auto* vk_pso = static_cast<VulkanRHIRayTracingPipelineState*>(_raytracing_pso);
        VK_CHECK_NULLPTR(vk_pso, "SetPipelineState: raytracing pipeline state is nullptr!", return);

        vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vk_pso->GetHandle());
        m_current_pipeline_state = vk_pso;
    }
    void VulkanRHIRayTracingCommandList::BeginRecording() {
        VulkanRHICommandListBase::Begin();
    }

    void VulkanRHIRayTracingCommandList::EndRecording() {
        VulkanRHICommandListBase::End();
    }

    void VulkanRHIRayTracingCommandList::Reset() {
        VulkanRHICommandListBase::Reset();
        m_current_pipeline_state = nullptr;
        m_bound_sets             = {};
    }

    void VulkanRHIRayTracingCommandList::TraceRay(uint32_t _width, uint32_t _height, uint32_t _depth) {
        static PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCmdTraceRaysKHR"));
        PrepareTraceRayCommand();
        vkCmdTraceRaysKHR(m_command_buffer,
                          m_current_pipeline_state->GetRayGenSBT(),
                          m_current_pipeline_state->GetRayMissSBT(),
                          m_current_pipeline_state->GetRayHitSBT(),
                          m_current_pipeline_state->GetRayCallableSBT(),
                          _width,
                          _height,
                          _depth);
    }

    void VulkanRHIRayTracingCommandList::TraceRayIndirect() {
        //todo:need to impl
        PrepareTraceRayCommand();
    }

    void VulkanRHIRayTracingCommandList::CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) {
        VulkanRHICommandListBase::CopyBuffer(_copy_info, _src, _dst);
    }

    void VulkanRHIRayTracingCommandList::CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) {
        VulkanRHICommandListBase::CopyTexture(_copy_info, _src, _dst);
    }

    void VulkanRHIRayTracingCommandList::CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) {
        VulkanRHICommandListBase::CopyBufferToTexture(src_buffer, dst_texture, _info);
    }

    void VulkanRHIRayTracingCommandList::CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) {
        VulkanRHICommandListBase::CopyTextureToBuffer(src_texture, dst_buffer, _info);
    }

    void VulkanRHIRayTracingCommandList::SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) {
        VulkanRHICommandListBase::SetPipelineBarrier(_dependency);
    }
    void VulkanRHIRayTracingCommandList::PrepareTraceRayCommand() {
        const auto* vk_pso = m_current_pipeline_state;
        VK_CHECK_NULLPTR(vk_pso, "PrepareTraceRayCommand: raytracing pipeline state is nullptr!", return);
        auto* vk_resource_cache = vk_pso->GetPipelineResourceCache();
        VK_CHECK_NULLPTR(vk_resource_cache, "PrepareTraceRayCommand: raytracing pipeline resource cache is nullptr!", return);

        auto pipeline_layout = vk_pso->GetPipelineLayout();

        const auto* vk_sets_layout = vk_pso->GetDescriptorSetsLayout();
        // 1. update and bind descriptor sets
        if (vk_resource_cache->HasDescriptorSets()) {
            vk_resource_cache->UpdateDescriptorSets(vk_sets_layout);
            if (m_bound_sets != vk_resource_cache->GetDescriptorSets()) {
                vk_resource_cache->BindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline_layout);
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
}// namespace Moer::Render
#include <shader/ShaderPipeline.h>
namespace Moer::Render {
    struct VulkanPipelineReflection {
        struct Bindings {
            VkShaderStageFlags                        stage;
            std::vector<VkDescriptorSetLayoutBinding> bindings;
        };
        struct ShaderStage {
            VkShaderStageFlagBits stage;
            std::string           entry_point;
            VkShaderModule        module;
        };
        std::vector<Bindings>              bindings;
        std::vector<ShaderStage>           stages;
        std::vector<VkDescriptorSetLayout> descriptor_set_layouts;
        std::vector<VkPushConstantRange>   push_constant_ranges;
    };
    class VulkanGfxPso : VulkanDeviceObject {
    public:
        template<typename T>
        void SetParam() {
        }
        VulkanGfxPso(VulkanDevice* _device, VulkanPipelineReflection&& _reflection, RHIGraphicsPSOCreateInfo&& _init) : VulkanDeviceObject(_device) {
            auto                       reflect = std::move(_reflection);
            VkPipelineLayoutCreateInfo pipeline_layout_info{};
            pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeline_layout_info.pNext                  = nullptr;
            pipeline_layout_info.flags                  = 0;
            pipeline_layout_info.setLayoutCount         = reflect.descriptor_set_layouts.size();
            pipeline_layout_info.pSetLayouts            = reflect.descriptor_set_layouts.data();
            pipeline_layout_info.pushConstantRangeCount = reflect.push_constant_ranges.size();
            pipeline_layout_info.pPushConstantRanges    = reflect.push_constant_ranges.data();
            vkCreatePipelineLayout(m_device->GetDevice(), &pipeline_layout_info, nullptr, &m_pipeline_layout);

            Array<VkPipelineShaderStageCreateInfo> shader_stage_infos(reflect.stages.size());
            for (size_t i = 0; i < reflect.stages.size(); ++i) {
                shader_stage_infos[i].sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                shader_stage_infos[i].pNext               = nullptr;
                shader_stage_infos[i].flags               = 0;
                shader_stage_infos[i].stage               = reflect.stages[i].stage;
                shader_stage_infos[i].module              = reflect.stages[i].module;
                shader_stage_infos[i].pName               = reflect.stages[i].entry_point.c_str();
                shader_stage_infos[i].pSpecializationInfo = nullptr;
            }
        }
        VkPipeline       pipeline;
        VkPipelineLayout m_pipeline_layout;
    };
    //RHICreateGfxPso<Texture, Texture, Buffer>? no RHICreateGfxPso<TPipelineLayout>(auto&& _init_info);
    VkRenderingAttachmentInfo FromColorAttachmentInfo(const ColorAttachment& _attachment) {
        VkRenderingAttachmentInfo attachment_info{};
        attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment_info.pNext = nullptr;

        VulkanTexture* vk_texture = reinterpret_cast<VulkanTexture*>(_attachment.target);
        attachment_info.imageView = vk_texture->GetView();

        attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment_info.loadOp      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_attachment.action));
        attachment_info.storeOp     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_attachment.action));

        // std::memcpy(attachment_info.clearValue.color.float32, color.float32, sizeof(color.float32));
        attachment_info.clearValue.color = {
            _attachment.clear_color.x,
            _attachment.clear_color.y,
            _attachment.clear_color.z,
            _attachment.clear_color.w};

        return attachment_info;
    }

    VkRenderingAttachmentInfo FromDepthAttachmentInfo(const DepthAttachment& _attachment) {
        VkRenderingAttachmentInfo attachment_info{};
        attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment_info.pNext = nullptr;

        VulkanTexture* vk_texture = reinterpret_cast<VulkanTexture*>(_attachment.target);
        attachment_info.imageView = vk_texture->GetView();

        attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment_info.loadOp      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_attachment.action));
        attachment_info.storeOp     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_attachment.action));
        // std::memcpy(attachment_info.clearValue.color.float32, color.float32, sizeof(color.float32));
        attachment_info.clearValue.depthStencil = {_attachment.clear_depth, _attachment.clear_stencil};

        return attachment_info;
    }

    class VkCmdVisitor : VulkanDeviceObject {
        enum class EState {
            Barrier,
            Draw,
            Common
        } state = EState::Common;
        VulkanCmdList&   cmd_list;
        VulkanAllocator& allocator;

    public:
        VkCmdVisitor(VulkanDevice& _device, VulkanAllocator& _allocator, VulkanCmdList& _cmd_list) : VulkanDeviceObject(&_device),
                                                                                                     allocator(_allocator),
                                                                                                     cmd_list(_cmd_list) {}
        void Visit(const UploadBufferCmd& _cmd) {
            auto data_span  = _cmd.Data();
            auto tmp_buffer = allocator.AllocateBuffer(_cmd.ByteSize(), 16);
            cmd_list.CopyData(tmp_buffer, data_span.data(), _cmd.ByteSize());
            VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(_cmd.Handle());
            cmd_list.CopyBuffer(reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
                                buffer,
                                _cmd.ByteSize(),
                                tmp_buffer.GetByteOffset(),
                                _cmd.Offset());
        }

        void Visit(const UploadTextureCmd& _cmd) {
            auto data_span  = _cmd.Data();
            auto tmp_buffer = allocator.AllocateBuffer(data_span.size_bytes(), 16);
            cmd_list.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
            VulkanTexture* texture = reinterpret_cast<VulkanTexture*>(_cmd.Handle());
            cmd_list.CopyBufferToTexture(reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
                                         texture,
                                         data_span.size_bytes(),
                                         tmp_buffer.GetByteOffset(),
                                         _cmd.Offset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const CopyBufferCmd& _cmd) {
            VulkanBuffer* src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
            VulkanBuffer* dst_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

            cmd_list.CopyBuffer(src_buffer,
                                dst_buffer,
                                _cmd.ByteSize(),
                                _cmd.SrcOffset(),
                                _cmd.DstOffset());
        }

        void Visit(const CopyBackBufferCmd& _cmd) {
            VulkanBuffer* src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.Handle());
            auto          tmp_buffer = allocator.AllocateBuffer(_cmd.ByteSize(), 16);
            cmd_list.CopyBuffer(src_buffer,
                                reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
                                _cmd.ByteSize(),
                                _cmd.Offset(),
                                tmp_buffer.GetByteOffset());

            allocator.AddOnComplete([tmp_buffer, &cmd_list(cmd_list), src_data(_cmd.Data())]() {
                cmd_list.CopyData(src_data, tmp_buffer, tmp_buffer.GetByteSize());
            });
        }

        void Visit(const CopyTextureCmd& _cmd) {
            VulkanTexture* src_texture = reinterpret_cast<VulkanTexture*>(_cmd.SrcHandle());
            VulkanTexture* dst_texture = reinterpret_cast<VulkanTexture*>(_cmd.DstHandle());

            cmd_list.CopyTexture(src_texture,
                                 dst_texture,
                                 _cmd.SrcOffset(),
                                 _cmd.DstOffset(),
                                 _cmd.Size(),
                                 _cmd.SrcMipLevel(),
                                 _cmd.DstMipLevel());
        }

        void Visit(const CopyBufferToTextureCmd& _cmd) {
            VulkanBuffer*  src_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
            VulkanTexture* dst_texture = reinterpret_cast<VulkanTexture*>(_cmd.DstHandle());

            cmd_list.CopyBufferToTexture(src_buffer,
                                         dst_texture,
                                         _cmd.ByteSize(),
                                         _cmd.SrcOffset(),
                                         _cmd.DstOffset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const CopyTextureToBufferCmd& _cmd) {
            VulkanTexture* src_texture = reinterpret_cast<VulkanTexture*>(_cmd.SrcHandle());
            VulkanBuffer*  dst_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

            cmd_list.CopyTextureToBuffer(src_texture,
                                         dst_buffer,
                                         _cmd.ByteSize(),
                                         _cmd.SrcOffset(),
                                         _cmd.DstOffset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const DispatchCmd& _cmd) {
            const auto& param = _cmd.Param();

            ComputePipeline pso(_cmd.Pipeline());
            cmd_list.SetPso(pso.handle);
            const auto& args = _cmd.Args();

            cmd_list.BindDescriptors(pso.handle, args);

            if (args.constants.size() > 0) {
                cmd_list.UploadPushConstants(
                    pso.handle,
                    std::span<const uint>(args.constants.data(), args.constants.size()));
            }
            std::visit(
                [&](auto&& _param) {
                    using TParam = std::decay_t<decltype(_param)>;
                    if constexpr (std::is_same_v<TParam, uint3>) {
                        cmd_list.Dispatch(_param.x, _param.y, _param.z);
                    } else if constexpr (std::is_same_v<TParam, DispatchIndirectParam>) {
                        cmd_list.DispatchIndirect(
                            reinterpret_cast<VulkanBuffer*>(_param.indirect.GetBuffer()), _param.indirect.GetByteOffset());
                    }
                },
                param);
        }

        // we don't need to do anything
        void Visit(const BarrierCmd& _cmd) {
            // state                      = EState::Barrier;
            // const auto& read_buffers   = _cmd.ReadBuffers();
            // const auto& write_buffers  = _cmd.WriteBuffers();
            // const auto& read_textures  = _cmd.ReadTextures();
            // const auto& write_textures = _cmd.WriteTextures();

            // for (const auto& buffer : read_buffers) {
            //     auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(buffer.handle);
            //     tracker.RecordState(vk_buffer, tracker.ReadBuffer(vk_buffer, buffer.state, buffer.pass_type));
            // }
            // for (const auto& buffer : write_buffers) {
            //     auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(buffer.handle);
            //     tracker.RecordState(vk_buffer, tracker.WriteBuffer(vk_buffer, buffer.state, buffer.pass_type));
            // }
            // for (const auto& texture : read_textures) {
            //     auto* vk_texture = reinterpret_cast<VulkanTexture*>(texture.handle);
            //     tracker.RecordState(vk_texture, tracker.ReadTexture(vk_texture, texture.state, texture.pass_type));
            // }
            // for (const auto& texture : write_textures) {
            //     auto* vk_texture = reinterpret_cast<VulkanTexture*>(texture.handle);
            //     tracker.RecordState(vk_texture, tracker.WriteTexture(vk_texture, texture.state, texture.pass_type));
            // }
        }

        void Visit(const SetDrawStateCmd& _cmd) {
            state = EState::Draw;

            const auto&    args = _cmd.Args();
            RasterPipeline pso(_cmd.Pipeline());

            const auto&                      pass_info = _cmd.RenderPassInfo();
            Array<VkRenderingAttachmentInfo> color_attachments(pass_info.color_attachments.size());
            for (size_t i = 0; i < pass_info.color_attachments.size(); ++i) {
                color_attachments[i] = FromColorAttachmentInfo(pass_info.color_attachments[i]);
            }
            std::optional<VkRenderingAttachmentInfo> depth_stencil_attachment;
            if (pass_info.depth_attachment.Valid()) {
                depth_stencil_attachment = FromDepthAttachmentInfo(pass_info.depth_attachment);
            }

            VkRenderingInfo dynamic_rendering_info{
                .sType      = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext      = nullptr,
                .flags      = 0,
                .renderArea = {
                    .offset = {pass_info.render_area.offset.x, pass_info.render_area.offset.y},
                    .extent = {pass_info.render_area.extent.width, pass_info.render_area.extent.height}},
                .layerCount           = 1,
                .colorAttachmentCount = uint(pass_info.color_attachments.size()),
                .pColorAttachments    = color_attachments.data(),
                .pDepthAttachment     = depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr,
                .pStencilAttachment   = depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr};

            cmd_list.BeginRendering(std::move(dynamic_rendering_info));

            cmd_list.SetPso(_cmd.Pipeline());

            cmd_list.BindDescriptors(pso.handle, args);

            if (args.constants.size() > 0) {
                cmd_list.UploadPushConstants(
                    pso.handle,
                    std::span<const uint>(args.constants.data(), args.constants.size()));
            }
            const auto& cmd_vertex_buffers = _cmd.VertexBuffers();
            const auto& draw_datas         = _cmd.DrawData();
            const auto& rect               = pass_info.render_area;
            VkViewport  viewport{
                 .x        = float(rect.offset.x),
                 .y        = float(rect.offset.y),
                 .width    = float(rect.extent.width),
                 .height   = float(rect.extent.height),
                 .minDepth = 0.0f,
                 .maxDepth = 1.0f};

            cmd_list.SetViewPort(viewport);
            cmd_list.SetScissor({rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height});
            for (const auto& draw_data : draw_datas) {
                StaticArray<VkBuffer, 4>     vertex_buffers{};
                StaticArray<VkDeviceSize, 4> offsets{};
                for (size_t i = 0; i < draw_data.vtx_cnt; ++i) {
                    vertex_buffers[i] = ResourceCast(draw_data.vtx_views[i].buffer)->GetHandle();
                    offsets[i]        = draw_data.vtx_views[i].offset;
                }
                cmd_list.SetVertexBuffers(0,
                                          draw_data.vtx_cnt,
                                          std::span<VkBuffer>(vertex_buffers.data(),
                                                              draw_data.vtx_cnt),
                                          std::span<VkDeviceSize>(offsets.data(),
                                                                  offsets.size()));

                uint vtx_offset = draw_data.vtx_cnt != 0 ? draw_data.vtx_views[0].offset / draw_data.vtx_views[0].buffer->GetStride() : 0;
                std::visit(
                    [&](auto&& _idx_input) {
                        using IdxType = std::decay_t<decltype(_idx_input)>;
                        if constexpr (std::is_same_v<IdxType, IndexBuffer>) {
                            const auto& index_buffer = _idx_input.buffer;
                            uint64      offset       = index_buffer.GetByteOffset();

                            cmd_list.SetIndexBuffer(
                                reinterpret_cast<VulkanBuffer*>(index_buffer.GetBuffer()),
                                offset,
                                VulkanEnumTranslator::METoVKIndexType(_idx_input.stride));
                            cmd_list.DrawIndexedInstanced(_idx_input.buffer.GetNumElements(),
                                                          draw_data.instance_count,
                                                          0,
                                                          vtx_offset,
                                                          draw_data.instance_offset);
                        } else if constexpr (std::is_same_v<IdxType, uint>) {
                            cmd_list.DrawInstanced(_idx_input,
                                                   draw_data.instance_count,
                                                   vtx_offset,
                                                   draw_data.instance_offset);
                        }
                    },
                    draw_data.idx_view);
            }
            cmd_list.EndRendering();
        }

        void Visit(const UpdateBindlessArrayCmd& _cmd) {
            VulkanBindlessArray* bindless_array = reinterpret_cast<VulkanBindlessArray*>(_cmd.Handle());
            bindless_array->CmdUpdate(_cmd.StealTextureUpdates(), _cmd.StealBufferUpdates());
            allocator.AddOnComplete([bindless_array,
                                     free_slots(_cmd.StealFreeSlots()),
                                     free_buffers(_cmd.StealFreeBuffers()),
                                     free_textures(_cmd.StealFreeTextures())]() {
                bindless_array->OnFree(std::move(free_slots), free_textures, free_buffers);
            });
        }

        // void Visit(const UpdateDrawStateCmd& _cmd) {
        // }

        // void Visit(const SetParamsCmd& _cmd) {
        //     auto&& args      = std::move(_cmd.StealArgs());
        //     auto&  pso       = _cmd.Pso();
        //     auto   set_param = [&](uint _idx, const TArg& _arg) {
        //         if constexpr (std::is_same_v<TArg, TextureView>) {
        //             pso.SetTexture(_idx, std::get<TextureView>(_arg));
        //         } else if constexpr (std::is_same_v<TArg, BufferView>) {
        //             pso.SetBuffer(_idx, std::get<BufferView>(_arg));
        //         }
        //     };
        //     std::visit([&](auto&& _args) {
        //         using TArgs = std::decay_t<decltype(_args)>;
        //         if constexpr (std::is_same_v<TArgs, ArrayArguments>) {
        //             for (size_t i = 0; i < _args.Size(); ++i) {
        //                 set_param(i, _args[i]);
        //             }
        //             cmd_list.UploadDescriptors(pso.handle);

        //             if (_args.constants.size() > 0) {
        //                 cmd_list.UploadPushConstants(pso.handle, std::span<const uint>(_args.constants.data(), _args.constants.size()));
        //             }
        //         } else if constexpr (std::is_same_v<TArgs, Arguments>) {
        //             for (size_t i = 0; i < _args.Size(); ++i) {
        //                 set_param(i, _args[i]);
        //             }
        //             cmd_list.UploadDescriptors(pso.handle);
        //         }
        //     },
        //                args);

        //     // cmd submit params
        // }

        // void Visit(const SetConstantCmd& _cmd) {
        //     auto& pso  = _cmd.Pso();
        //     auto  data = std::move(_cmd.StealData());
        //     cmd_list.UploadPushConstants(pso.handle, std::span<uint>(data.data(), data.size()));
        //     // cmd submit consants
        // }
    };

    VkNativeQueue::VkNativeQueue(EQueueType _type, VulkanDevice& _device) : type(_type) {
        switch (_type) {
            case EQueueType::Graphics:
                queue = _device.GetGraphicsQueue();
                break;
            case EQueueType::Compute:
                queue = _device.GetComputeQueue();
                break;
            case EQueueType::Copy:
                queue = _device.GetTransferQueue();
                break;
            case EQueueType::Num: break;
        }
        assert(queue != VK_NULL_HANDLE && "Invalid queue type!");
    }

    VkNativeQueue::~VkNativeQueue() {
    }

    void VkNativeQueue::Submit(VulkanCmdList& _cmdlist, VkFence _fence) {
        VkSubmitInfo2   submit_info{};
        VkCommandBuffer cmd = _cmdlist.GetHandle();

        VkCommandBufferSubmitInfo cmd_info{.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                           .pNext         = nullptr,
                                           .commandBuffer = cmd};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

        submit_info.pNext                    = nullptr;
        submit_info.waitSemaphoreInfoCount   = wait_infos.size();
        submit_info.pWaitSemaphoreInfos      = wait_infos.data();
        submit_info.signalSemaphoreInfoCount = signal_infos.size();
        submit_info.pSignalSemaphoreInfos    = signal_infos.data();
        submit_info.commandBufferInfoCount   = 1;
        submit_info.pCommandBufferInfos      = &cmd_info;
        vkQueueSubmit2(queue, 1, &submit_info, _fence);
        wait_infos.clear();
        signal_infos.clear();
    }

    void VkNativeQueue::Wait(VulkanFence* _fence, uint64 _fence_val, VkPipelineStageFlags2 _stage) {
        VkSemaphore sem = _fence->GetUnderlyingHandle();
        wait_infos.push_back(VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = sem,
            .value     = _fence_val,
            .stageMask = _stage});
    }

    void VkNativeQueue::Wait(VkSemaphore _sem, VkPipelineStageFlags2 _stage) {
        wait_infos.push_back(VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = _sem,
            .value     = 0,
            .stageMask = _stage});
    }
    void VkNativeQueue::Signal(VulkanFence* _fence, uint64 _fence_val, VkPipelineStageFlags2 _stage) {
        VkSemaphore sem = _fence->GetUnderlyingHandle();
        signal_infos.push_back(VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = sem,
            .value     = _fence_val,
            .stageMask = _stage});
    }
    void VkNativeQueue::Signal(VkSemaphore _sem, VkPipelineStageFlags2 _stage) {
        signal_infos.push_back(VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = _sem,
            .value     = 0,
            .stageMask = _stage});
    }
    void VkCommandQueue::Wait(WaitEvent _evt) {
        auto* fence = reinterpret_cast<VulkanFence*>(_evt.timeline_handle);
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(fence, _evt.value, false);
            queue_cv.notify_one();
        }
    }
    WaitEvent VkCommandQueue::Execute(CmdSubmit&& _submit) {
        auto         allocator_ptr = std::move(GetAllocator());
        auto&        vk_allocator  = *allocator_ptr;
        VkCmdVisitor visitor(vk_device, vk_allocator, vk_allocator.GetCmdList());
        CmdReorderer reorderer{};
        auto&        tracker = vk_allocator.GetTracker();

        VkCmdPreprocessor preprocessor(tracker);

        for (const auto& cmd : _submit.cmds) {
            reorderer.AcceptCmd(cmd.get());
        }
        const auto& cmd_lists = reorderer.m_cmd_lists;
        bool        has_cmd   = !reorderer.m_cmd_lists.empty();
        uint64      last_time = last_frame;

        if (has_cmd) {
            vk_allocator.GetCmdList().Begin();
            vk_device.GetGlobalDescriptorHeap().BeginPushDescriptors(last_time + 1);
        }
        for (const CmdReorderer::LinkedCommandList& cmd_list : cmd_lists) {
            if (cmd_list.head == nullptr) {
                continue;
            }
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                preprocessor.VisitCmd(cmdnode->cmd);
            }
            tracker.ResolveBarriers();
            tracker.DispatchBarriers(vk_allocator.GetCmdList());
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                const auto* cmd = cmdnode->cmd;
                switch (cmd->Type()) {
                    case Command::EType::UploadBuffer:
                        visitor.Visit(static_cast<const UploadBufferCmd&>(*cmd));
                        break;
                    case Command::EType::CopyBackBuffer:
                        visitor.Visit(static_cast<const CopyBackBufferCmd&>(*cmd));
                        break;
                    case Command::EType::BufferToBuffer:
                        visitor.Visit(static_cast<const CopyBufferCmd&>(*cmd));
                        break;
                    case Command::EType::BufferToTexture:
                        visitor.Visit(static_cast<const CopyBufferToTextureCmd&>(*cmd));
                        break;
                    case Command::EType::TextureToBuffer:
                        visitor.Visit(static_cast<const CopyTextureToBufferCmd&>(*cmd));
                        break;
                    case Command::EType::UploadTexture:
                        visitor.Visit(static_cast<const UploadTextureCmd&>(*cmd));
                        break;
                    case Command::EType::TextureToTexture:
                        visitor.Visit(static_cast<const CopyTextureCmd&>(*cmd));
                        break;
                    case Command::EType::ShaderDispatch:
                        visitor.Visit(static_cast<const DispatchCmd&>(*cmd));
                        break;
                    case Command::EType::BuildAccel:
                        break;
                    case Command::EType::Barrier:
                        visitor.Visit(static_cast<const BarrierCmd&>(*cmd));
                        break;
                    case Command::EType::SetDrawState:
                        visitor.Visit(static_cast<const SetDrawStateCmd&>(*cmd));
                        break;
                    // case Command::EType::UpdateDrawState:
                    //     visitor.Visit(static_cast<const UpdateDrawStateCmd&>(*cmd));
                    // case Command::EType::SetParams:
                    //     visitor.Visit(static_cast<const SetParamsCmd&>(*cmd));
                    //     break;
                    // case Command::EType::SetConstants:
                    //     visitor.Visit(static_cast<const SetConstantCmd&>(*cmd));
                    //     break;
                    case Command::EType::Custom: break;
                    case Command::EType::UpdateBindlessArray: {
                        visitor.Visit(static_cast<const UpdateBindlessArrayCmd&>(*cmd));
                        break;
                    };
                }
            }
        }

        if (has_cmd) {
            tracker.RestoreState();
            tracker.DispatchBarriers(vk_allocator.GetCmdList());
            vk_allocator.GetCmdList().End();
            vk_device.GetGlobalDescriptorHeap().EndPushDescriptors(last_time + 1);
            tracker.Reset();
        }
        if (_submit.cmds.empty()) {
            allocators.Push(allocator_ptr.release());
            std::unique_lock<std::mutex> lock(event_mutex);
            if (_submit.callbacks.size() > 0) {
                event_queue.emplace_back(std::move(_submit.callbacks), last_time, true);
                queue_cv.notify_one();
            }
            return {uint64(timeline), last_time};
        } else {
            auto current_timeline = ++last_frame;
            auto end_tag          = queue.GetType() == EQueueType::Graphics ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            queue.Signal(timeline, current_timeline, end_tag);
            for (auto& evt : _submit.wait_events) {
                queue.Wait(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value);
            }
            for (auto& evt : _submit.signal_events) {
                queue.Signal(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value, end_tag);
            }
            queue.Submit(vk_allocator.GetCmdList());

            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(std::move(allocator_ptr), current_timeline, true);
            if (_submit.callbacks.size() > 0) {
                event_queue.emplace_back(std::move(_submit.callbacks), current_timeline, false);
            }
            queue_cv.notify_one();
            return {uint64(timeline), current_timeline};
        }
        return {uint64(timeline), 0ull};
    }

    void VkCommandQueue::Present(SwapchainRef _sc, TextureView _view) {
        VkSwapchain* sc           = ResourceCast(_sc.Get());
        auto         allocator    = std::move(GetAllocator());
        auto&        vk_allocator = *allocator;
        auto&        vk_cmd_list  = vk_allocator.GetCmdList();
        auto&        vk_tracker   = vk_allocator.GetTracker();
        sc->WaitFrameInFlight();
        auto [fence, idx, present_timeline] = sc->AquireNextImage();
        if (idx == UINT32_MAX) {
            //present null
            allocator->Reset();
            allocators.Push(allocator.release());
            return;
        }
        //copy
        auto* vk_src_tex     = static_cast<VulkanTexture*>(_view.texture);
        auto* swaphchain_tex = ResourceCast(sc->GetSwapchainImage(idx).texture);
        {
            vk_cmd_list.Begin();
            vk_tracker.SetPassType(EPassType::Graphics);
            vk_tracker.RecordState(vk_src_tex, vk_tracker.ReadTexture(vk_src_tex, ETextureState::TRANSFER));
            vk_tracker.RecordState(swaphchain_tex, vk_tracker.WriteTexture(swaphchain_tex, ETextureState::TRANSFER));
            vk_tracker.ResolveBarriers();
            vk_tracker.DispatchBarriers(vk_cmd_list);
            //copy
            //todo: need transaction
            vk_cmd_list.CopyTexture(vk_src_tex, swaphchain_tex, _view.extent, {0, 0, 0}, {0, 0, 0}, 0, 0);
            vk_tracker.RecordState(swaphchain_tex,
                                   VK_ACCESS_2_NONE,
                                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                   VK_PIPELINE_STAGE_2_COPY_BIT);
            vk_tracker.ResolveBarriers();
            vk_tracker.DispatchBarriers(vk_cmd_list);

            vk_tracker.RestoreState();
            vk_tracker.DispatchBarriers(vk_cmd_list);
            vk_cmd_list.End();
            vk_tracker.Reset();
            // vk_tracker.PropagateState();
        }

        auto current_timeline = ++last_frame;
        queue.Signal(timeline, current_timeline, VK_PIPELINE_STAGE_2_COPY_BIT);
        queue.Wait(sc->GetImageReadyFence(idx), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
        queue.Signal(sc->GetRenderFinishedFence(), VK_PIPELINE_STAGE_2_COPY_BIT);
        queue.Submit(vk_allocator.GetCmdList(), sc->GetInFlightFence(present_timeline));
        sc->Present(queue.GetHandle(), idx);
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(std::move(allocator), current_timeline, true);
            queue_cv.notify_one();
        }
    }

    void VkCommandQueue::Sync() {
        Complete(last_frame);
    }

    UniquePtr<VulkanAllocator> VkCommandQueue::GetAllocator() {
        if (last_frame > vk_device.cmd_alloc_limits) {
            Complete(last_frame - vk_device.cmd_alloc_limits);
        }
        auto allocator = std::move(UniquePtr<VulkanAllocator>(allocators.Pop()));
        if (allocator) {
            // allocator->ResetCmdList();
            return std::move(allocator);
        }
        return MakeUnique<VulkanAllocator>(&vk_device);
    }

    void VkCommandQueue::ExecuteThread() {
        while (enabled) {
            uint64 timeline;
            bool   b_wake_up = false;

            auto wait_util_reach_timeline = [&timeline, &b_wake_up, this]() {
                if (!b_wake_up) { return; }
                uint64 prev_timeline = executed_frame;
                while (prev_timeline < timeline && !executed_frame.compare_exchange_weak(prev_timeline, timeline)) {
                    std::this_thread::yield();
                }
            };

            auto visit_allocator = [&, &allocators(this->allocators), fence(this->timeline)](UniquePtr<VulkanAllocator>& _allocator) {
                _allocator->Complete(fence, timeline);
                _allocator->Reset();
                allocators.Push(_allocator.release());
                wait_util_reach_timeline();
            };
            auto visit_fence = [&](FencePlaceHoler& _fence) {
                this->timeline->HostWait(timeline);
                wait_util_reach_timeline();
            };
            auto visit_funcs = [&](Array<std::function<void()>>& _funcs) {
                for (auto& func : _funcs) {
                    func();
                }
                wait_util_reach_timeline();
            };
            auto visit_external_fence = [&](VulkanFence* _fence) {
                _fence->HostWait(timeline);
                _fence->Notify(std::max(timeline, _fence->current_value));
            };
            while (true) {
                std::optional<QueueEvent> evt;
                {
                    std::unique_lock<std::mutex> lock(event_mutex);
                    if (!event_queue.empty()) {
                        auto& event = event_queue.front();
                        evt.emplace(std::move(event));
                        event_queue.pop_front();
                    }
                }
                if (!evt.has_value()) {
                    break;
                }
                timeline  = evt->timeline;
                b_wake_up = evt->wake_thread;
                std::visit(
                    [&](auto& _evt) {
                        using TEvent = std::decay_t<decltype(_evt)>;
                        if constexpr (std::is_same_v<TEvent, UniquePtr<VulkanAllocator>>) {
                            visit_allocator(_evt);
                        } else if constexpr (std::is_same_v<TEvent, FencePlaceHoler>) {
                            visit_fence(_evt);
                        } else if constexpr (std::is_same_v<TEvent, Array<std::function<void()>>>) {
                            visit_funcs(_evt);
                        }
                    },
                    evt->event);
            }
            {
                //wait for queue submission
                std::unique_lock<std::mutex> lock(event_mutex);
                while (enabled && event_queue.empty()) {
                    queue_cv.wait(lock);
                }
            }
        }
    }

    void VkCommandQueue::Complete(uint64 _timeline) {
        while (executed_frame < _timeline) {
            std::this_thread::yield();
        }
        vk_device.FlushDeferredReleases();
    }

    void VkCommandQueue::Signal() {
        auto current_timeline = ++last_frame;
        queue.Signal(timeline, current_timeline);
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.push_back({FencePlaceHoler{}, current_timeline, true});
            queue_cv.notify_one();
        }
    }

    VulkanAllocator::VulkanAllocator(VulkanDevice* _device) : VulkanDeviceObject(_device),
                                                              allocator(_device),
                                                              small_allocator(&allocator, small_block_size, 1.5) {

        cmd_allocator.emplace(_device, VK_QUEUE_GRAPHICS_BIT);
        cmd_list.emplace(&cmd_allocator.value(), *_device);
    }

    VulkanAllocator::~VulkanAllocator() {
        small_allocator.Dispose();
        for (auto& handle : large_buffers) {
            allocator.DeAllocate(reinterpret_cast<uint64>(handle));
        }
        large_buffers.clear();
    }
    //
    BufferView VulkanAllocator::AllocateBuffer(uint64 _size, uint _alignment) {
        _size = std::max<uint64>(_size, _alignment);
        if (_size < small_block_size) {
            auto          handle = small_allocator.Allocate(_size, _alignment);
            VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle.handle);
            return {buffer, handle.offset, _size, 1u};
        }
        auto          handle = allocator.Allocate(_size);
        VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
        large_buffers.push_back(buffer);

        return {buffer, 0, _size, 1u};
    }

    void VulkanAllocator::ResetBufferAlloc() {
        small_allocator.Reset();
        for (auto& handle : large_buffers) {
            allocator.DeAllocate(reinterpret_cast<uint64>(handle));
        }
        large_buffers.clear();
    }

    void VulkanAllocator::ResetCmdList() {
        vkResetCommandPool(m_device->GetDevice(), cmd_allocator->GetHandle(), 0);
    }

    void VulkanAllocator::Complete(VulkanFence* _fence, uint64 _wait_val) {
        _fence->HostWait(_wait_val);
        _fence->Notify(std::max(_wait_val, _fence->current_value));
        //execute post complete functions if needed
        for (auto& func : on_complete) {
            func();
        }
    }
    void VulkanAllocator::Reset() {
        ResetBufferAlloc();
        ResetCmdList();
    }

    VulkanAllocator::TmpBufferAllocator::TmpBufferAllocator(VulkanDevice* _device) : VulkanDeviceObject(_device) {}
    uint64 VulkanAllocator::TmpBufferAllocator::Allocate(uint64 _size) {
        VkBufferCreateInfo buffer_info = {
            .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .size                  = _size,
            .usage                 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr};

        VmaAllocationCreateInfo alloc_info{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO};
        VulkanBuffer::BufferAlloc buffer_alloc;
        BufferInfo                info(
            _size,
            1,
            EBufferUsageFlags::CPU_VISIBLE);

        VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_info, &alloc_info, &buffer_alloc.buffer, &buffer_alloc.alloc, nullptr));
        VulkanBuffer* vk_buffer = MoerNew(VulkanBuffer)(info, *m_device, buffer_alloc.buffer, buffer_alloc.alloc, false);

        return reinterpret_cast<uint64>(vk_buffer);
    }

    void VulkanAllocator::TmpBufferAllocator::DeAllocate(uint64 _buffer) {
        auto* buffer = reinterpret_cast<VulkanBuffer*>(_buffer);
        MoerDelete(buffer);
    }

    VulkanAllocator::StackAllocator::StackAllocator(TmpBufferAllocator* _alloc, uint64 _init_cap, double _grouth_factor) : allocator(_alloc), init_capacity(_init_cap), growth_factor(_grouth_factor) {
        capacity = std::max<uint64>(init_capacity, 1);
        allocated_buffers.push_back({allocator->Allocate(capacity), capacity, 0});
    }

    VulkanAllocator::StackAllocator::Chunk VulkanAllocator::StackAllocator::Allocate(uint64 _size, uint _align) {

        auto align_size = std::max<uint64>(_align, _size);
        for (auto& alloc_buf : allocated_buffers) {
            auto offset = (alloc_buf.offset + align_size - 1) & ~(align_size - 1);
            if (alloc_buf.size - offset >= _size) {
                alloc_buf.offset = offset + _size;
                return {alloc_buf.handle, offset};
            }
        }
        if (capacity < align_size) {
            capacity = std::max<uint64>(capacity * growth_factor, align_size);
        }
        auto buffer = allocator->Allocate(capacity);
        allocated_buffers.push_back({buffer, capacity, align_size});
        return {buffer, 0};
    }

    VulkanAllocator::StackAllocator::Chunk VulkanAllocator::StackAllocator::Allocate(uint64 _size) {
        for (auto& alloc_buf : allocated_buffers) {
            if (alloc_buf.size - alloc_buf.offset >= _size) {
                auto offset = alloc_buf.offset;
                alloc_buf.offset += _size;
                return {alloc_buf.handle, offset};
            }
        }
        if (capacity < _size) {
            capacity = std::max<uint64>(capacity * growth_factor, _size);
        }
        auto buffer = allocator->Allocate(capacity);
        allocated_buffers.push_back({buffer, capacity, _size});
        return {buffer, 0};
    }

    void VulkanAllocator::StackAllocator::Reset() {
        if (allocated_buffers.size() == 1) {
            allocated_buffers.back().offset = 0;
        }
        if (allocated_buffers.size() > 1) {
            //pack all staging buffer to one
            uint64 sum_size = 0;
            for (auto& alloc_buf : allocated_buffers) {
                sum_size += alloc_buf.size;
                allocator->DeAllocate(alloc_buf.handle);
            }
            allocated_buffers.clear();
            allocated_buffers.push_back({allocator->Allocate(sum_size), sum_size, 0});
        }
    }

    void VulkanAllocator::StackAllocator::Dispose() {
        for (auto& alloc_buf : allocated_buffers) {
            allocator->DeAllocate(alloc_buf.handle);
        }
        allocated_buffers.clear();
    }
    //
    VulkanCmdAllocator::VulkanCmdAllocator(VulkanDevice* _device, VkQueueFlagBits _queue_type) : VulkanDeviceObject(_device), queue_type(_queue_type) {
        VkCommandPoolCreateInfo pool_info = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .queueFamilyIndex = m_device->GetQueueFamilyIndex(queue_type)};

        VK_CHECK_RESULT(vkCreateCommandPool(_device->GetDevice(), &pool_info, nullptr, &command_pool));

        if (!command_list.has_value()) {
            command_list.emplace(this, *m_device);
        }
    }

    VulkanCmdList::VulkanCmdList(VulkanCmdAllocator* _alloc, VulkanDevice& _device) : allocator(_alloc), device(_device) {
        VkCommandBufferAllocateInfo command_buffer_info = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext              = nullptr,
            .commandPool        = allocator->GetHandle(),
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1};
        VK_CHECK_RESULT(vkAllocateCommandBuffers(device.GetDevice(), &command_buffer_info, &command_buffer));
    }
    void VulkanCmdList::Begin() {
        VkCommandBufferBeginInfo begin_info = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .pInheritanceInfo = nullptr};
        VK_CHECK_RESULT(vkBeginCommandBuffer(command_buffer, &begin_info));
    }
    void VulkanCmdList::End() {
        VK_CHECK_RESULT(vkEndCommandBuffer(command_buffer));
    }
    void VulkanCmdList::CopyBuffer(
        VulkanBuffer* _src,
        VulkanBuffer* _dst,
        uint64        _size,
        uint64        _src_offset,
        uint64        _dst_offset) {

        VkBufferCopy copy_region = {
            .srcOffset = _src_offset,
            .dstOffset = _dst_offset,
            .size      = _size};

        vkCmdCopyBuffer(command_buffer, _src->GetHandle(), _dst->GetHandle(), 1, &copy_region);
    }
    void VulkanCmdList::CopyBufferToTexture(
        VulkanBuffer*  _src,
        VulkanTexture* _dst,
        uint64         _size,
        uint64         _src_offset,
        uint3          _dst_offset,
        uint3          _extent,
        uint32         _mip_level) {
        VkImageAspectFlags aspect      = _src->GetResourceType() == RRT_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkBufferImageCopy  copy_region = {
             .bufferOffset      = _src_offset,
             .bufferRowLength   = 0,
             .bufferImageHeight = 0,
             .imageSubresource  = {
                  .aspectMask     = aspect,
                  .mipLevel       = _mip_level,
                  .baseArrayLayer = 0,
                  .layerCount     = 1},
             .imageOffset = {static_cast<int32_t>(_dst_offset.x), static_cast<int32_t>(_dst_offset.y), static_cast<int32_t>(_dst_offset.z)},
             .imageExtent = {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y), static_cast<uint32_t>(_extent.z)}};

        vkCmdCopyBufferToImage(command_buffer, _src->GetHandle(), _dst->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
    }

    void VulkanCmdList::CopyTextureToBuffer(
        VulkanTexture* _src,
        VulkanBuffer*  _dst,
        uint64         _size,
        uint3          _src_offset,
        uint64         _dst_offset,
        uint3          _extent,
        uint32         _mip_level) {
        VkImageAspectFlags aspect      = _src->GetResourceType() == RRT_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkBufferImageCopy  copy_region = {
             .bufferOffset      = _dst_offset,
             .bufferRowLength   = 0,
             .bufferImageHeight = 0,
             .imageSubresource  = {
                  .aspectMask     = aspect,
                  .mipLevel       = _mip_level,
                  .baseArrayLayer = 0,
                  .layerCount     = 1},
             .imageOffset = {static_cast<int32_t>(_src_offset.x), static_cast<int32_t>(_src_offset.y), static_cast<int32_t>(_src_offset.z)},
             .imageExtent = {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y), static_cast<uint32_t>(_extent.z)}};

        vkCmdCopyImageToBuffer(command_buffer, _src->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _dst->GetHandle(), 1, &copy_region);
    }

    void VulkanCmdList::CopyData(const BufferView& _dst, const void* _data, uint64 _size) {
        auto*        buffer    = static_cast<VulkanBuffer*>(_dst.buffer);
        VmaAllocator allocator = device.GetVmaAllocator();

        void* p_data;
        VK_CHECK_RESULT(vmaMapMemory(allocator, buffer->GetAllocation(), &p_data));
        std::memcpy((byte*)p_data + _dst.GetByteOffset(), _data, _size);
        vmaUnmapMemory(allocator, buffer->GetAllocation());
    }

    void VulkanCmdList::CopyData(const void* _dst, const BufferView& _src, uint64 _size) {
        auto*        buffer    = static_cast<VulkanBuffer*>(_src.buffer);
        VmaAllocator allocator = device.GetVmaAllocator();

        void* p_data;
        VK_CHECK_RESULT(vmaMapMemory(allocator, buffer->GetAllocation(), &p_data));
        std::memcpy((byte*)_dst, (byte*)p_data + _src.GetByteOffset(), _size);
        vmaUnmapMemory(allocator, buffer->GetAllocation());
    }

    void VulkanCmdList::DrawIndexedInstanced(
        uint32_t _index_cnt,
        uint32_t _instance_cnt,
        uint32_t _first_index,
        uint32_t _vertex_offset,
        uint32_t _first_instance) {
        vkCmdDrawIndexed(command_buffer, _index_cnt, _instance_cnt, _first_index, _vertex_offset, _first_instance);
    }

    void VulkanCmdList::DrawInstanced(
        uint32_t _vertex_cnt,
        uint32_t _instance_cnt,
        uint32_t _first_vertex,
        uint32_t _first_instance) {
        vkCmdDraw(command_buffer, _vertex_cnt, _instance_cnt, _first_vertex, _first_instance);
    }

    void VulkanCmdList::DrawIndirectCnt(
        VulkanBuffer* _commands,
        uint64        _commands_offset,
        VulkanBuffer* _count,
        uint64        _count_offset,
        uint32_t      _max_cnt,
        uint32_t      _stride) {
        vkCmdDrawIndexedIndirectCount(command_buffer, _commands->GetHandle(), _commands_offset, _count->GetHandle(), _count_offset, _max_cnt, _stride);
    }

    void VulkanCmdList::CopyTexture(
        VulkanTexture* _src,
        VulkanTexture* _dst,
        uint3          _extent,
        uint3          _src_offset,
        uint3          _dst_offset,
        uint32         _src_mip_level,
        uint32         _dst_mip_level) {
        VkImageCopy copy_region =
            {
                .srcSubresource = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = _src_mip_level,
                    .baseArrayLayer = 0,
                    .layerCount     = 1},
                .srcOffset      = {static_cast<int32_t>(_src_offset.x), static_cast<int32_t>(_src_offset.y), static_cast<int32_t>(_src_offset.z)},
                .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = _dst_mip_level, .baseArrayLayer = 0, .layerCount = 1},
                .dstOffset      = {static_cast<int32_t>(_dst_offset.x), static_cast<int32_t>(_dst_offset.y), static_cast<int32_t>(_dst_offset.z)},
                .extent         = {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y), static_cast<uint32_t>(_extent.z)}};

        vkCmdCopyImage(command_buffer, _src->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _dst->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
    }

    void VulkanCmdList::BeginRendering(VkRenderingInfo&& _info) {
        vkCmdBeginRendering(command_buffer, &_info);
    }

    void VulkanCmdList::EndRendering() {
        vkCmdEndRendering(command_buffer);
    }

    void VulkanCmdList::SetVertexBuffers(uint _first_binding, uint _binding_cnt, std::span<VkBuffer> _buffers, std::span<uint64> offsets) {
        vkCmdBindVertexBuffers(command_buffer, _first_binding, _binding_cnt, _buffers.data(), offsets.data());
    }

    void VulkanCmdList::SetIndexBuffer(VulkanBuffer* _buffer, uint64 _offset, VkIndexType _index_type) {
        vkCmdBindIndexBuffer(command_buffer, _buffer->GetHandle(), _offset, _index_type);
    }

    void VulkanCmdList::SetViewPort(const VkViewport& _view_port) {

        vkCmdSetViewport(command_buffer, 0, 1, &_view_port);
    }

    void VulkanCmdList::SetScissor(const VkRect2D& _scissor) {
        vkCmdSetScissor(command_buffer, 0, 1, &_scissor);
    }

    void VulkanCmdList::Dispatch(uint _group_count_x, uint _group_count_y, uint _group_count_z) {
        vkCmdDispatch(command_buffer, _group_count_x, _group_count_y, _group_count_z);
    }

    void VulkanCmdList::DispatchIndirect(VulkanBuffer* _buffer, uint64 _offset) {
        vkCmdDispatchIndirect(command_buffer, _buffer->GetHandle(), _offset);
    }

    void VulkanCmdList::UploadDescriptors(PipelineHandle& _pso_handle) {
        auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(std::get<VkPipelineHandle>(_pso_handle.handle).handle);

        auto* resource_cache = vk_pso->GetPipelineResourceCache();
        if (resource_cache->HasDescriptorSets()) {
            resource_cache->UpdateDescriptorSets(vk_pso->GetDescriptorSetsLayout());
            resource_cache->BindDescriptorSets(command_buffer, vk_pso->GetPipelineBindPoint(), vk_pso->GetPipelineLayout());
        }
    }

    void VulkanCmdList::UploadPushConstants(
        PipelineHandle&       _pso_handle,
        std::span<const uint> _data) {
        auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(std::get<VkPipelineHandle>(_pso_handle.handle).handle);
        // auto  binding_info               = _pso_handle.binding_infos[_pso_handle.constant_idx];
        // auto [offset, size, stage_flags] = DecodeReflectPushConstant(binding_info);

        // vkCmdPushConstants(command_buffer, vk_pso->GetPipelineLayout(), stage_flags, offset, size, _data.data());
    }

    void VulkanCmdList::BindDescriptors(PipelineHandle& _pso_handle, const ArrayArguments& _args) {
        auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(std::get<VkPipelineHandle>(_pso_handle.handle).handle);

        assert(vk_pso && vk_pso->bind_template != nullptr && "Pipeline state has no bind template!");
        VulkanPipelineParamBinder& bind_template        = *vk_pso->bind_template;
        VulkanDescriptorHeap&      descriptor_heap      = device.GetGlobalDescriptorHeap();
        auto&                      set_binders          = bind_template.set_binders;
        uint64                     g_global_desc_offset = descriptor_heap.current_offset;
        for (auto& [set, binder] : set_binders) {
            std::visit(
                [&](auto& _binder) {
                    using T = std::decay_t<decltype(_binder)>;
                    if constexpr (std::is_same_v<T, VulkanBindlessSetArray>) {
                        BindlessArrayRef     array                           = std::get<BindlessArrayRef>(_args[_binder.param_idx]);
                        VulkanBindlessArray* bindless_array                  = static_cast<VulkanBindlessArray*>(array.Get());
                        bind_template.desc_buffers[_binder.desc_idx].address = bindless_array->bindless_buffer_descs->DeviceAddress();
                    } else if constexpr (std::is_same_v<T, VulkanBindlessSetImage>) {
                        BindlessArrayRef     array                           = std::get<BindlessArrayRef>(_args[_binder.param_idx]);
                        VulkanBindlessArray* bindless_array                  = static_cast<VulkanBindlessArray*>(array.Get());
                        bind_template.desc_buffers[_binder.desc_idx].address = bindless_array->bindless_texture_descs->DeviceAddress();

                    } else if constexpr (std::is_same_v<T, VulkanBindlessSetSampler>) {
                        BindlessArrayRef     array                           = std::get<BindlessArrayRef>(_args[_binder.param_idx]);
                        VulkanBindlessArray* bindless_array                  = static_cast<VulkanBindlessArray*>(array.Get());
                        bind_template.desc_buffers[_binder.desc_idx].address = bindless_array->bindless_texture_descs->DeviceAddress();

                    } else if constexpr (std::is_same_v<T, VulkanDescriptorSetBinder>) {
                        //normal resources
                        for (uint i = 0; i < _binder.writers.size(); ++i) {
                            auto&                       writer   = _binder.writers[i];
                            const VulkanDescriptorInfo& set_info = _binder.bind_infos[i];
                            switch (writer.descriptorType) {
                                case VK_DESCRIPTOR_TYPE_SAMPLER: {
                                    uint64 src_handle = descriptor_heap.GetSamplerDescIdx(std::get<Sampler>(_args[set_info.param_idx]));
                                    descriptor_heap.PushSamplerDesc(src_handle, _binder.binding_infos[set_info.info_idx].offset);
                                    break;
                                }
                                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
                                    uint64 src_handle = descriptor_heap.GetImageDescIdx(&std::get<TextureView>(_args[set_info.param_idx]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                    descriptor_heap.PushImageDesc(src_handle, _binder.binding_infos[set_info.info_idx].offset);
                                    break;
                                }
                                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
                                    uint64 src_handle = descriptor_heap.GetImageDescIdx(&std::get<TextureView>(_args[set_info.param_idx]), VK_IMAGE_LAYOUT_GENERAL);
                                    descriptor_heap.PushImageDesc(src_handle, _binder.binding_infos[set_info.info_idx].offset);

                                    break;
                                }
                                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                                    break;
                                }
                                default: {
                                    assert(false && "Unsupported descriptor type!");
                                }
                            }
                        }
                        //set desc buffer offset
                        bind_template.desc_buffer_offsets[_binder.offset_idx].offset = descriptor_heap.current_offset;
                        descriptor_heap.IncrementOffset(_binder.size);
                        // device.vk_cmd_push_descriptor_set(command_buffer, _binder.bind_point, _binder.push_info.layout, _binder.push_info.set, _binder.writers.size(), _binder.writers.data());
                    }
                },
                binder);
        }
        if (!bind_template.desc_buffers.empty()) {
            vkCmdBindDescriptorBuffersEXT(command_buffer, bind_template.desc_buffers.size(), bind_template.desc_buffers.data());
        }

        for (const auto& desc_info : bind_template.desc_buffer_offsets) {
            uint   buffer_idx = desc_info.buf_idx;
            uint64 offset     = desc_info.offset;
            vkCmdSetDescriptorBufferOffsetsEXT(command_buffer, desc_info.bind_point, desc_info.layout, desc_info.set, 1, &buffer_idx, &offset);
        }

        if (bind_template.push_constants_info.size > 0) {
            bind_template.push_constants_info.pValues = _args.constants.data();
            const auto& push_info                     = &bind_template.push_constants_info;
            vkCmdPushConstants(command_buffer, push_info->layout, push_info->stageFlags, push_info->offset, push_info->size, push_info->pValues);
        }
    }

    void VulkanCmdList::SetPso(const PipelineHandle& _pso_handle) {
        auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(std::get<VkPipelineHandle>(_pso_handle.handle).handle);
        vkCmdBindPipeline(command_buffer, vk_pso->GetPipelineBindPoint(), vk_pso->GetHandle());
    }

}// namespace Moer::Render
