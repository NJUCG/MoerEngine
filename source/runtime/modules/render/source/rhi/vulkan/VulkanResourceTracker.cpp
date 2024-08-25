#include "VulkanResourceTracker.h"
#include "rhi/RHICommon.h"
#include "vulkan/vulkan_core.h"
#include "VulkanCommand.h"
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
    auto VkTracker::ReadBuffer(VulkanBuffer* _buffer, EBufferState _state) -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
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

    auto VkTracker::WriteBuffer(VulkanBuffer* _buffer, EBufferState _state) -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
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
    static constexpr VkPipelineStageFlags2 gfx_tex_read_stage_rules[] = {
        VK_PIPELINE_STAGE_2_NONE,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT};

    static constexpr VkPipelineStageFlags2 cs_tex_read_stage_rules[] = {
        VK_PIPELINE_STAGE_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};

    static constexpr VkPipelineStageFlagBits2 gfx_tex_write_stage_rules[] = {
        VK_PIPELINE_STAGE_2_NONE,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT};

    auto VkTracker::ReadTexture(VulkanTexture* _texture, ETextureState _state) -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {

        static constexpr VkAccessFlags2 gfx_rules[] = {
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

        static constexpr VkImageLayout layout_rules[] = {
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (pass_type == EPassType::Graphics) {
            return {gfx_rules[static_cast<uint32>(_state)], layout_rules[static_cast<uint32>(_state)], gfx_tex_read_stage_rules[static_cast<uint32>(_state)]};
        }
        if (pass_type == EPassType::Compute) {
            return {gfx_rules[static_cast<uint32>(_state)], layout_rules[static_cast<uint32>(_state)], cs_tex_read_stage_rules[static_cast<uint32>(_state)]};
        }
        assert(false && "Invalid pass type");
        return {gfx_rules[static_cast<uint32>(_state)], layout_rules[static_cast<uint32>(_state)], gfx_tex_read_stage_rules[static_cast<uint32>(_state)]};
    }

    auto VkTracker::WriteTexture(VulkanTexture* _texture, ETextureState _state) -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {
        static constexpr VkAccessFlags2 gfx_rules[] = {
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};

        static constexpr VkImageLayout layout_rules[] = {
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_UNDEFINED};// Invalid
        if (pass_type == EPassType::Graphics) {
            return {gfx_rules[static_cast<uint32>(_state)], layout_rules[static_cast<uint32>(_state)], gfx_tex_write_stage_rules[static_cast<uint32>(_state)]};
        }
        if (pass_type == EPassType::Compute) {
            return {gfx_rules[static_cast<uint32>(_state)], layout_rules[static_cast<uint32>(_state)], cs_tex_read_stage_rules[static_cast<uint32>(_state)]};
        }
        return {gfx_rules[static_cast<uint32>(_state)], layout_rules[static_cast<uint32>(_state)], gfx_tex_write_stage_rules[static_cast<uint32>(_state)]};
    }
    void VkTracker::RecordState(VulkanBuffer* _buffer, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage) {
        if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
            auto& state = it->second;
            if (state.dst_access != _access || state.dst_stage != _stage) {
                state.src_access = state.dst_access;
                state.dst_stage  = state.dst_stage;
                state.dst_access = _access;
                state.dst_stage  = _stage;
            }
            return;
        }

        buffer_states[_buffer] = {_buffer->m_access_flags, _buffer->m_stage_flags, _access, _stage};
    }

    void VkTracker::RecordState(VulkanBuffer* _buffer, std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&& _state) {
        RecordState(_buffer, std::get<0>(_state), std::get<1>(_state));
    }

    void VkTracker::RecordState(VulkanTexture* _texture, std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&& _state) {
        RecordState(_texture, std::get<0>(_state), std::get<1>(_state), std::get<2>(_state));
    }
    uint8 Min(uint8 a, uint8 b) {
        return a < b ? a : b;
    }

    void VkTracker::RecordState(VulkanTexture* _texture, VkAccessFlagBits2 _access, VkImageLayout _layout, VkPipelineStageFlagBits2 _stage, uint8_t _mip_level, uint8_t _mip_count) {
        // Range range{_mip_level, _mip_count};
        TextureState state{{_mip_level, _mip_count}, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, _access, _layout, _stage};
        auto         state_iter = texture_states.find(_texture);
        if (state_iter != texture_states.end()) {
            auto& target_state = state_iter->second;
            if (target_state.dst_stage == state.dst_stage && target_state.dst_access == state.dst_access && target_state.dst_layout == state.dst_layout) {
                return;
            }
            target_state.src_access = target_state.dst_access;
            target_state.src_layout = target_state.dst_layout;
            target_state.src_stage  = target_state.dst_stage;
            if (target_state.src_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                target_state.src_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                target_state.src_access = VK_ACCESS_2_NONE;
                target_state.src_stage  = _stage;
            }

            target_state.dst_layout = state.dst_layout;
            target_state.dst_access = state.dst_access;
            target_state.dst_stage  = state.dst_stage;

        } else {
            texture_states[_texture] = {state};
        }
        // const auto& final_state = texture_states.find(_texture)->second;

        // VkImageAspectFlags aspect = _texture->GetResourceType() == RRT_TEXTURE ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        // texture_barriers.emplace_back(
        //     VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        //                           nullptr,
        //                           final_state.src_access,
        //                           final_state.src_access,
        //                           final_state.dst_stage,
        //                           final_state.dst_access,
        //                           final_state.src_layout,
        //                           final_state.dst_layout,
        //                           VK_QUEUE_FAMILY_IGNORED,
        //                           VK_QUEUE_FAMILY_IGNORED,
        //                           _texture->GetHandle(),
        //                           {aspect, _mip_level, _mip_count, 0, 1}});
        // bool  b_has_init_state = !_texture->m_subresource_states.empty();
        // auto  swap_valid_state = [&](TextureState& _state) {
        //     const bool is_present_src = _state.dst_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        //     if (is_present_src) {
        //         _state.src_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        //         _state.src_access = VK_ACCESS_2_NONE;
        //         _state.src_stage  = _stage;
        //     } else {
        //         _state.src_layout = _state.dst_layout;
        //         _state.src_access = _state.dst_access;
        //         _state.src_stage  = _state.dst_stage;
        //     }

        //     _state.dst_layout = _layout;
        //     _state.dst_access = _access;
        //     _state.dst_stage  = _stage;
        // };

        // auto enqueue_barrier = [&](const TextureState& _state) {
        //     texture_barriers.emplace_back(
        //         VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        //                               nullptr,
        //                               _state.src_stage,
        //                               _state.src_access,
        //                               _state.dst_stage,
        //                               _state.dst_access,
        //                               _state.src_layout,
        //                               _state.dst_layout,
        //                               VK_QUEUE_FAMILY_IGNORED,
        //                               VK_QUEUE_FAMILY_IGNORED,
        //                               _texture->GetHandle(),
        //                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, _state.range.mip_level, _state.range.mip_count}});
        // };

        // auto fill_texture_init_state = [&]() {
        //     auto& states = texture_states[_texture];
        //     if (!b_has_init_state) {
        //         states.emplace_back(Range{0, uint8(_texture->GetNumMips())},
        //                             VK_ACCESS_2_NONE,
        //                             VK_IMAGE_LAYOUT_UNDEFINED,
        //                             VK_PIPELINE_STAGE_2_NONE,
        //                             VK_ACCESS_2_NONE,
        //                             VK_IMAGE_LAYOUT_UNDEFINED,
        //                             VK_PIPELINE_STAGE_2_NONE);
        //         return;
        //     }
        //     states.resize(_texture->m_subresource_states.size());

        //     for (uint8 i = 0; i < _texture->m_subresource_states.size(); ++i) {
        //         const auto& subresource = _texture->m_subresource_states[i];
        //         states[i] =
        //             {subresource.mip_level,
        //              subresource.mip_cnt,
        //              VK_ACCESS_2_NONE,
        //              VK_IMAGE_LAYOUT_UNDEFINED,
        //              VK_PIPELINE_STAGE_2_NONE,
        //              subresource.access,
        //              subresource.layout,
        //              subresource.stage};
        //     }
        // };
        // auto iter = texture_states.find(_texture);
        // if (iter == texture_states.end()) {
        //     fill_texture_init_state();
        // }

        // auto& states            = texture_states[_texture];
        // int32 overlap_idx       = -1;
        // bool  b_overlap_new     = false;
        // bool  b_split_cur_range = false;

        // for (auto& state : states) {
        //     if (state.range.Overlaps(range)) {
        //         overlap_idx       = &state - &states[0];
        //         b_overlap_new     = range.Exceeds(state.range);
        //         b_split_cur_range = range.mip_level > state.range.mip_level;
        //         break;
        //     }
        // }
        // // overlap valid range
        // if (overlap_idx != -1) {
        //     auto& state       = states[overlap_idx];
        //     bool  b_need_swap = !(state.dst_access == _access && state.dst_layout == _layout);

        //     if (!b_overlap_new && !b_split_cur_range) {
        //         //perfect overlap
        //         if (b_need_swap) {
        //             swap_valid_state(state);
        //             enqueue_barrier(state);
        //         }
        //         return;
        //     }
        //     if (b_overlap_new) {
        //         //todo: not correct nor elegant
        //         // cover current range and extend
        //         bool b_insert_new = b_split_cur_range && overlap_idx == 0 && b_need_swap;
        //         if (b_insert_new) {
        //             auto tp_state            = state;
        //             tp_state.range.mip_count = Min(range.mip_level - state.range.mip_level, state.range.mip_count);
        //             states.insert(states.begin(), tp_state);
        //             overlap_idx++;
        //         }
        //         auto& overlap_state = states[overlap_idx];
        //         if (b_split_cur_range && !b_insert_new) {
        //             //alter previous range
        //             auto& prev_state           = states[overlap_idx - 1];
        //             prev_state.range.mip_count = Min(range.mip_level - prev_state.range.mip_level, prev_state.range.mip_count);
        //         }
        //         if (b_need_swap) {
        //             overlap_state.range = range;
        //         } else
        //             swap_valid_state(overlap_state);
        //         //need to modify the following states
        //         Range erase_range{static_cast<uint8>(uint8(overlap_idx) + 1), 0};
        //         if (overlap_idx + 1 < states.size()) {
        //             for (int32 i = overlap_idx + 1; i < states.size(); ++i) {
        //                 if (!range.Overlaps(states[i].range)) {
        //                     break;
        //                 }
        //                 if (range.Contains(states[i].range)) {
        //                     erase_range.mip_count += states[i].range.mip_count;
        //                 } else {
        //                     //overlap but not contain
        //                     states[i].range.mip_count = states[i].range.mip_level + states[i].range.mip_count - range.mip_level - range.mip_count;
        //                     states[i].range.mip_level = range.mip_level + range.mip_count;
        //                     break;
        //                 }
        //             }
        //         }
        //         if (erase_range.mip_count > 0) {
        //             states.erase(states.begin() + erase_range.mip_level, states.begin() + erase_range.mip_level + erase_range.mip_count);
        //         }
        //         return;
        //     }
        //     assert(false && "Invalid state");
        //     return;
        // }
        // //decide to insert new state
        // if (states.empty()) {
        //     states.push_back({range, _access, _layout, _stage, _access, _layout, _stage});
        //     return;
        // }
        // if (range.mip_level + range.mip_count < states[0].range.mip_level) {
        //     states.insert(states.begin(), {range, _access, _layout, _stage, _access, _layout, _stage});
        //     return;
        // }
        // if (range.mip_level > states.back().range.mip_level + states.back().range.mip_count) {
        //     states.push_back({range, _access, _layout, _stage, _access, _layout, _stage});
        //     return;
        // }
        // for (int32 i = 0; i < states.size() - 1; ++i) {
        //     if (range.mip_level > states[i].range.mip_level + states[i].range.mip_count && range.mip_level + range.mip_count < states[i + 1].range.mip_level) {
        //         states.insert(states.begin() + i + 1, {range, _access, _layout, _stage, _access, _layout, _stage});
        //         return;
        //     }
        // }
        // assert(false && "Invalid state");
    }

    void VkTracker::ResolveBarriers() {
        for (auto& [buffer, state] : buffer_states) {
            if (state.src_access == state.dst_access && state.src_stage == state.dst_stage) {
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
        }

        for (auto& [texture, state] : texture_states) {
            if (state.src_access == state.dst_access && state.src_stage == state.dst_stage) {
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
            barrier.subresourceRange.baseMipLevel   = state.range.mip_level;
            barrier.subresourceRange.levelCount     = state.range.mip_count;
            barrier.oldLayout                       = state.src_layout;
            barrier.newLayout                       = state.dst_layout;

            state.src_access = state.dst_access;
            state.src_stage  = state.dst_stage;
            state.src_layout = state.dst_layout;
        }
    }

    void VkTracker::DispatchBarriers(VulkanCmdList& _cmdlist) {
        if (!buffer_barriers.empty() || !texture_barriers.empty()) {
            VkDependencyInfoKHR dependency_info{};
            dependency_info.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
            dependency_info.pNext                    = nullptr;
            dependency_info.bufferMemoryBarrierCount = static_cast<uint32>(buffer_barriers.size());
            dependency_info.pBufferMemoryBarriers    = buffer_barriers.data();
            dependency_info.imageMemoryBarrierCount  = static_cast<uint32>(texture_barriers.size());
            dependency_info.pImageMemoryBarriers     = texture_barriers.data();
            vkCmdPipelineBarrier2(_cmdlist.GetHandle(), &dependency_info);
            buffer_barriers.clear();
            texture_barriers.clear();
        }
    }

    void VkTracker::PropagateState() {
        for (auto& [texture, state] : texture_states) {
            //get texture
            texture->state = {state.range.mip_level, state.range.mip_count, state.dst_access, state.dst_layout, state.dst_stage};
        }

        for (auto& [buffer, state] : buffer_states) {
            buffer->m_access_flags = state.dst_access;
            buffer->m_stage_flags  = state.dst_stage;
        }
    }

}// namespace Moer::Render