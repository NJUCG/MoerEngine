#include <volk.h>
#include "VulkanResourceTracker.h"
#include "VulkanCommand.h"

#include "VulkanRHIResource.h"
#include "rhi/RHICommon.h"
#include "vulkan/vulkan_core.h"

namespace Moer::Render {
    /**
     * @brief 
    enum class EBufferState : uint32{
        UNDEFINED,
        TRANSFER,
        VERTEX,
        INDEX,
        INDIRECT,
        UNORDERED_ACCESS
    };
    //one state a time
    enum class ETextureState : uint32{
        UNDEFINED,
        TRANSFER,
        SHADER_RESOURCE,
        RENDER_TARGET,
        DEPTH_STENCIL,
        UNORDERED_ACCESS,
        SAMPLED
    };
     * 
     * @param _buffer 
     * @param _usage 
     * @return std::tuple<VkAccessFlags2, VkPipelineStageFlags2> 
     */
    auto VkTracker::ReadBuffer(VulkanBuffer* _buffer, EBufferState _state, EPassType _type) -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
        static constexpr VkAccessFlags2 gfx_rules[] = {
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
            VK_ACCESS_2_INDEX_READ_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};

        static constexpr VkPipelineStageFlags2 stage_rules[] = {
            VK_PIPELINE_STAGE_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT};
        return {gfx_rules[static_cast<uint32>(_state)], stage_rules[static_cast<uint32>(_state)]};
    }

    auto VkTracker::WriteBuffer(VulkanBuffer* _buffer, EBufferState _state, EPassType _type) -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
        static constexpr VkAccessFlags2 gfx_rules[] = {
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};

        static constexpr VkPipelineStageFlags2 stage_rules[] = {
            VK_PIPELINE_STAGE_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT};
        return {gfx_rules[static_cast<uint32>(_state)], stage_rules[static_cast<uint32>(_state)]};
    }
    static constexpr VkPipelineStageFlags2 tex_read_stage_rules[] = {
        //GFX PASS STAGES
        VK_PIPELINE_STAGE_2_NONE,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        //COMPUTE PASS STAGES
        VK_PIPELINE_STAGE_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};

    static constexpr VkPipelineStageFlagBits2 tex_write_stage_rules[] = {
        //GFX PASS STAGES
        VK_PIPELINE_STAGE_2_NONE,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        //COMPUTE PASS STAGES
        VK_PIPELINE_STAGE_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};

    auto VkTracker::ReadTexture(VulkanTexture* _texture, ETextureState _state, EPassType _type) -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {

        static constexpr VkAccessFlags2 gfx_rules[] = {
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

        static constexpr VkImageLayout layout_rules[] = {
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        auto index      = static_cast<uint32>(_state);
        auto pass_index = static_cast<uint32>(_state) + uint32(ETextureState::Num) * uint32(_type);

        return {gfx_rules[index], layout_rules[index], tex_read_stage_rules[pass_index]};
    }

    auto VkTracker::WriteTexture(VulkanTexture* _texture, ETextureState _state, EPassType _type) -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {
        static constexpr VkAccessFlags2 gfx_rules[] = {
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_ACCESS_2_NONE};

        static constexpr VkImageLayout layout_rules[] = {
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_UNDEFINED};// Invalid

        auto index      = static_cast<uint32>(_state);
        auto pass_index = static_cast<uint32>(_state) + uint32(ETextureState::Num) * uint32(_type);
        return {gfx_rules[index], layout_rules[index], tex_write_stage_rules[pass_index]};
    }

    static bool IsWriteState(VkImageLayout layout) {
        return layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && layout != VK_IMAGE_LAYOUT_GENERAL;
        // switch (layout) {
        //     case VK_IMAGE_LAYOUT_GENERAL:
        //     case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        //     case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        //     case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        //         return true;
        //     default:
        //         return false;
        // }
    }

    static bool IsWriteState(VkAccessFlags2 _access) {
        return _access & VK_ACCESS_2_SHADER_WRITE_BIT ||
               _access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT ||
               _access & VK_ACCESS_2_MEMORY_WRITE_BIT ||
               _access & VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }

    void VkTracker::MarkWriteable(VulkanTexture* _texture, bool _writeable) {
        if (_writeable) {
            writed_state_textures.insert(_texture);
        } else {
            writed_state_textures.erase(_texture);
        }
    }

    void VkTracker::MarkWriteable(VulkanBuffer* _buffer, bool _writeable) {
        if (_writeable) {
            writed_state_buffers.insert(_buffer);
        } else {
            writed_state_buffers.erase(_buffer);
        }
    }

    void VkTracker::RecordState(VulkanBuffer* _buffer, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage) {
        MarkWriteable(_buffer, IsWriteState(_access));
        pending_buffers.insert(_buffer);

        if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
            auto& state = it->second;
            // if (state.dst_access != _access || state.dst_stage != _stage) {
            //     state.src_access = state.dst_access;
            //     state.src_stage  = state.dst_stage;
            //     state.dst_access = _access;
            //     state.dst_stage  = _stage;
            // }
            // if (state.dst_stage != VK_PIPELINE_STAGE_2_NONE && state.dst_stage != _stage) {
            //     //need push barriers
            //     // ResolveBarriers();
            //     // pending_buffers.insert(_buffer);
            //     // assert(state.dst_stage == _stage && "state transition error");
            //     return;
            // }

            state.dst_access |= _access;
            state.dst_stage |= _stage;

            return;
        }

        buffer_states[_buffer] = {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, _access, _stage};
    }

    void VkTracker::RecordState(VulkanBuffer* _buffer, std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&& _state) {
        RecordState(_buffer, std::get<0>(_state), std::get<1>(_state));
    }

    void VkTracker::RegisterFlushBuffer(const BufferView& _view, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage) {
        VulkanBuffer* buffer = ResourceCast(_view.buffer);
        buffer_barriers.emplace_back(VkBufferMemoryBarrier2{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            nullptr,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            VK_ACCESS_2_NONE,
            _stage,
            _access,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            buffer->GetHandle(),
            _view.GetByteOffset(),
            _view.GetByteSize()});
        auto& state = flush_buffer_states[buffer];
        state.dst_access |= _access;
        state.dst_stage |= _stage;
    }

    void VkTracker::RecordState(VulkanTexture* _texture, std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&& _state) {
        RecordState(_texture, std::get<0>(_state), std::get<1>(_state), std::get<2>(_state));
    }
    uint8 Min(uint8 a, uint8 b) {
        return a < b ? a : b;
    }

    void VkTracker::EmplaceWriteBLAS(uint64 _blas_buf) {
        write_blas_states.insert(_blas_buf);
    }

    bool VkTracker::ContainsWriteBLAS(uint64 _blas_buf) {
        return write_blas_states.find(_blas_buf) != write_blas_states.end();
    }

    void VkTracker::RecordState(VulkanTexture* _texture, VkAccessFlagBits2 _access, VkImageLayout _layout, VkPipelineStageFlagBits2 _stage, uint8_t _mip_level, uint8_t _mip_count) {
        // Range range{_mip_level, _mip_count};
        TextureState state{
            VK_ACCESS_2_NONE,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            _access,
            _layout,
            _stage};

        auto state_iter = texture_states.find(_texture);
        bool is_write   = IsWriteState(_layout);
        MarkWriteable(_texture, is_write);
        pending_textures.insert(_texture);
        if (state_iter != texture_states.end()) {
            auto& target_state = state_iter->second;

            if (target_state.dst_stage == state.dst_stage && target_state.dst_access == state.dst_access && target_state.dst_layout == state.dst_layout) {
                return;
            }
            // target_state.src_access = target_state.dst_access;
            // target_state.src_layout = target_state.dst_layout;
            // target_state.src_stage  = target_state.dst_stage;
            if (target_state.src_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                target_state.src_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                target_state.src_access = VK_ACCESS_2_NONE;
                target_state.src_stage  = _stage;
            }
            if (target_state.dst_layout != VK_IMAGE_LAYOUT_UNDEFINED && target_state.dst_layout != state.dst_layout) {
                //need push barriers
                assert(target_state.dst_stage == state.dst_stage && "state transition error");
            }
            target_state.dst_layout = state.dst_layout;
            target_state.dst_access = state.dst_access;
            target_state.dst_stage  = state.dst_stage;

        } else {
            if (_texture->b_has_init_state) {
                state.src_layout = _texture->GetPreferredLayout();
                if (_texture->b_present) {
                    state.src_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                    state.src_access = VK_ACCESS_2_NONE;
                    state.src_stage  = _stage;
                }
            }
            texture_states[_texture] = {state};
        }
    }

    void VkTracker::ResolveBarriers() {

        for (VulkanBuffer* buffer : pending_buffers) {

            if (auto it = buffer_states.find(buffer); it != buffer_states.end()) {
                auto& state = it->second;
                if (state.src_access == state.dst_access && state.src_stage == state.dst_stage) {
                    state.dst_access = VK_ACCESS_2_NONE;
                    state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
                    continue;
                }
                buffer_barriers.emplace_back();
                VkBufferMemoryBarrier2& barrier = buffer_barriers.back();
                barrier.sType                   = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                barrier.pNext                   = nullptr;
                barrier.srcAccessMask           = state.src_access;
                barrier.dstAccessMask           = state.dst_access;
                barrier.srcStageMask            = state.src_stage;
                barrier.dstStageMask            = state.dst_stage;
                barrier.srcQueueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer                  = buffer->GetHandle();
                barrier.offset                  = 0;
                barrier.size                    = buffer->GetByteSize();

                state.src_access = state.dst_access;
                state.src_stage  = state.dst_stage;
                state.dst_access = VK_ACCESS_2_NONE;
                state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
            }
        }

        // for (auto& [buffer, state] : buffer_states) {
        //     if (state.src_access == state.dst_access && state.src_stage == state.dst_stage) {
        //         continue;
        //     }
        //     buffer_barriers.emplace_back();
        //     VkBufferMemoryBarrier2& barrier = buffer_barriers.back();
        //     barrier.sType                   = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        //     barrier.pNext                   = nullptr;
        //     barrier.srcAccessMask           = state.src_access;
        //     barrier.dstAccessMask           = state.dst_access;
        //     barrier.srcStageMask            = state.src_stage;
        //     barrier.dstStageMask            = state.dst_stage;
        //     barrier.srcQueueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
        //     barrier.dstQueueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
        //     barrier.buffer                  = buffer->GetHandle();
        //     barrier.offset                  = 0;
        //     barrier.size                    = buffer->GetByteSize();

        //     state.src_access = state.dst_access;
        //     state.src_stage  = state.dst_stage;
        //     state.dst_access = VK_ACCESS_2_NONE;
        //     state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
        // }

        for (VulkanTexture* texture : pending_textures) {
            if (auto it = texture_states.find(texture); it != texture_states.end()) {
                auto& state = it->second;
                if (state.src_access == state.dst_access && state.src_stage == state.dst_stage) {
                    state.dst_access = VK_ACCESS_2_NONE;
                    state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
                    state.dst_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                    continue;
                }
                texture_barriers.emplace_back();
                VkImageMemoryBarrier2& barrier          = texture_barriers.back();
                barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.pNext                           = nullptr;
                barrier.srcAccessMask                   = state.src_access;
                barrier.dstAccessMask                   = state.dst_access;
                barrier.srcStageMask                    = state.src_stage;
                barrier.dstStageMask                    = state.dst_stage;
                barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
                barrier.image                           = texture->GetHandle();
                barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount     = 1;
                barrier.subresourceRange.baseMipLevel   = 0;
                barrier.subresourceRange.levelCount     = texture->GetNumMips();
                barrier.oldLayout                       = state.src_layout;
                barrier.newLayout                       = state.dst_layout;

                state.src_access = state.dst_access;
                state.src_stage  = state.dst_stage;
                state.src_layout = state.dst_layout;

                state.dst_access = VK_ACCESS_2_NONE;
                state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
                state.dst_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
        }

        // for (auto& [texture, state] : texture_states) {
        //     if (state.src_access == state.dst_access && state.src_stage == state.dst_stage) {
        //         continue;
        //     }
        //     texture_barriers.emplace_back();
        //     VkImageMemoryBarrier2& barrier          = texture_barriers.back();
        //     barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        //     barrier.pNext                           = nullptr;
        //     barrier.srcAccessMask                   = state.src_access;
        //     barrier.dstAccessMask                   = state.dst_access;
        //     barrier.srcStageMask                    = state.src_stage;
        //     barrier.dstStageMask                    = state.dst_stage;
        //     barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        //     barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        //     barrier.image                           = texture->GetHandle();
        //     barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        //     barrier.subresourceRange.baseArrayLayer = 0;
        //     barrier.subresourceRange.layerCount     = 1;
        //     barrier.subresourceRange.baseMipLevel   = 0;
        //     barrier.subresourceRange.levelCount     = texture->GetNumMips();
        //     barrier.oldLayout                       = state.src_layout;
        //     barrier.newLayout                       = state.dst_layout;

        //     state.src_access = state.dst_access;
        //     state.src_stage  = state.dst_stage;
        //     state.src_layout = state.dst_layout;

        //     state.dst_access = VK_ACCESS_2_NONE;
        //     state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
        //     state.dst_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        // }

        pending_buffers.clear();
        pending_textures.clear();
    }

    void VkTracker::DispatchBarriers(VulkanCmdList& _cmdlist) {
        if (!buffer_barriers.empty() || !texture_barriers.empty() || !memory_barriers.empty()) {
            VkDependencyInfoKHR dependency_info{};
            dependency_info.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
            dependency_info.pNext                    = nullptr;
            dependency_info.bufferMemoryBarrierCount = static_cast<uint32>(buffer_barriers.size());
            dependency_info.pBufferMemoryBarriers    = buffer_barriers.data();
            dependency_info.imageMemoryBarrierCount  = static_cast<uint32>(texture_barriers.size());
            dependency_info.pImageMemoryBarriers     = texture_barriers.data();
            dependency_info.memoryBarrierCount       = static_cast<uint32>(memory_barriers.size());
            dependency_info.pMemoryBarriers          = memory_barriers.data();
            vkCmdPipelineBarrier2(_cmdlist.GetHandle(), &dependency_info);
            buffer_barriers.clear();
            texture_barriers.clear();
        }
    }

    void VkTracker::RestoreState() {
        for (auto& [texture, state] : texture_states) {
            texture->b_has_init_state = true;
            if (texture->b_present) continue;
            texture_barriers.emplace_back();
            VkImageMemoryBarrier2& barrier          = texture_barriers.back();
            barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.pNext                           = nullptr;
            barrier.srcAccessMask                   = state.src_access;
            barrier.dstAccessMask                   = VK_ACCESS_2_NONE;
            barrier.srcStageMask                    = state.src_stage;
            barrier.dstStageMask                    = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.image                           = texture->GetHandle();
            barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount     = 1;
            barrier.subresourceRange.baseMipLevel   = 0;
            barrier.subresourceRange.levelCount     = texture->GetNumMips();
            barrier.oldLayout                       = state.src_layout;
            barrier.newLayout                       = texture->GetPreferredLayout();

            state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
            state.dst_access = VK_ACCESS_2_NONE;
            state.dst_layout = texture->GetPreferredLayout();
        }
        VkAccessFlags2        src_buffer_access = VK_ACCESS_2_NONE;
        VkPipelineStageFlags2 src_buffer_stages = VK_PIPELINE_STAGE_2_NONE;
        for (auto& [buffer, state] : buffer_states) {
            src_buffer_access |= state.src_access;
            src_buffer_stages |= state.src_stage;
            state.dst_access = VK_ACCESS_2_NONE;
            state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
        }

        for (auto& [buffer, state] : flush_buffer_states) {
            src_buffer_access |= state.dst_access;
            src_buffer_stages |= state.dst_stage;
        }
        if (!buffer_states.empty() || !flush_buffer_states.empty()) {
            //memory barrier
            memory_barriers.emplace_back();
            VkMemoryBarrier2& barrier = memory_barriers.back();
            barrier.sType             = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.pNext             = nullptr;
            barrier.srcAccessMask     = src_buffer_access;
            barrier.dstAccessMask     = VK_ACCESS_2_NONE;
            barrier.srcStageMask      = src_buffer_stages;
            barrier.dstStageMask      = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }

        // flush_buffer_states.clear();

        //write_states_set.clear();
    }

    void VkTracker::Reset() {
        buffer_barriers.clear();
        texture_barriers.clear();
        memory_barriers.clear();
        buffer_states.clear();
        texture_states.clear();
        writed_state_textures.clear();
        writed_state_buffers.clear();
    }

}// namespace Moer::Render