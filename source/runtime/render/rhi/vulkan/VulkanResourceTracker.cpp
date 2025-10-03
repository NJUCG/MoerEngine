#include <volk.h>
#include "VulkanResourceTracker.h"
#include "VulkanCommand.h"

#include "VulkanRHIResource.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "vulkan/vulkan_core.h"
#include "VulkanMacroUtils.h"

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
        static constexpr VkAccessFlags2 buffer_access_rules[] = {
            //GFX PASS ACCESS
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
            VK_ACCESS_2_INDEX_READ_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            //COMPUTE PASS ACCESS
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            //RAYTRACING PASS ACCESS
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};

        static constexpr VkPipelineStageFlags2 stage_rules[] = {
            //GFX PASS STAGES
            VK_PIPELINE_STAGE_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            //COMPUTE PASS STAGES
            VK_PIPELINE_STAGE_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            //RAYTRACING PASS STAGES
            VK_PIPELINE_STAGE_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR};

        uint buffer_index = static_cast<uint32>(_state) + static_cast<uint32>(_type) * uint(EBufferState::Num);
        return {buffer_access_rules[buffer_index], stage_rules[buffer_index]};
    }

    auto VkTracker::WriteBuffer(VulkanBuffer* _buffer, EBufferState _state, EPassType _type) -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
        static constexpr VkAccessFlags2 buffer_access_rules[] = {
            //GFX PASS ACCESS
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            //COMPUTE PASS ACCESS
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            //RAYTRACING PASS ACCESS
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};

        static constexpr VkPipelineStageFlags2 stage_rules[] = {
            //GFX PASS STAGES
            VK_PIPELINE_STAGE_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            //COMPUTE PASS STAGES
            VK_PIPELINE_STAGE_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            //RAYTRACING PASS STAGES
            VK_PIPELINE_STAGE_2_NONE,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR};

        uint buffer_index = static_cast<uint32>(_state) + static_cast<uint32>(_type) * uint(EBufferState::Num);

        return {buffer_access_rules[buffer_index], stage_rules[buffer_index]};
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

    static constexpr VkImageLayout tex_read_layout_rules[] = {
        //GFX PASS LAYOUTS
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        //COMPUTE PASS LAYOUTS
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        //RAYTRACING PASS LAYOUTS
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    static constexpr VkImageLayout depth_read_layout_rules[] = {
        //GFX PASS LAYOUTS
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        //COMPUTE PASS LAYOUTS
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        //RAYTRACING PASS LAYOUTS
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

    static constexpr VkAccessFlags2 tex_read_access_rules[] = {
        //GFX PASS ACCESS
        VK_ACCESS_2_NONE,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        //COMPUTE PASS ACCESS
        VK_ACCESS_2_NONE,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        //RAYTRACING PASS ACCESS
        VK_ACCESS_2_NONE,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT

    };
    auto VkTracker::ReadTexture(VulkanTexture* _texture, ETextureState _state, EPassType _type) -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {

        // static constexpr VkAccessFlags2 gfx_rules[] = {
        //     VK_ACCESS_2_NONE,
        //     VK_ACCESS_2_TRANSFER_READ_BIT,
        //     VK_ACCESS_2_SHADER_READ_BIT,
        //     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
        //     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        //     VK_ACCESS_2_SHADER_READ_BIT,
        //     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

        // static constexpr VkImageLayout gfx_layout_rules[] = {
        //     VK_IMAGE_LAYOUT_UNDEFINED,
        //     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        //     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        //     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        //     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        //     VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        //     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        // static constexpr VkImageLayout depth_layout_rules[] = {
        //     VK_IMAGE_LAYOUT_UNDEFINED,
        //     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        //     VK_IMAGE_LAYOUT_GENERAL,
        //     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        //     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        //     VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        //     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        auto index      = static_cast<uint32>(_state);
        auto pass_index = static_cast<uint32>(_state) + uint32(ETextureState::Num) * uint32(_type);
        if (uint(_texture->GetAspectFlags() & ETextureAspectFlags::DEPTH_SLICE) != 0u) {
            return {tex_read_access_rules[pass_index], depth_read_layout_rules[pass_index], tex_read_stage_rules[pass_index]};
        }

        return {tex_read_access_rules[pass_index], tex_read_layout_rules[pass_index], tex_read_stage_rules[pass_index]};
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

    static bool IsWriteState(VkImageLayout _layout, VkAccessFlags2 _access) {
        bool read_layout = _layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || _layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (read_layout) return false;

        bool read_access  = _access & VK_ACCESS_2_SHADER_READ_BIT || _access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT || _access & VK_ACCESS_2_TRANSFER_READ_BIT;
        bool write_access = _access & VK_ACCESS_2_SHADER_WRITE_BIT || _access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT || _access & VK_ACCESS_2_MEMORY_WRITE_BIT || _access & VK_ACCESS_2_TRANSFER_WRITE_BIT;
        if (read_access && !write_access) return false;
        return true;
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

    void VkTracker::RecordState(VulkanBuffer* _buffer, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage, uint32_t _src_queue_family, uint32_t _dst_queue_family) {
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

        buffer_states[_buffer] = {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _access, _stage, _src_queue_family, _dst_queue_family};
    }

    void VkTracker::FlushSrcState(VulkanBuffer* _buffer, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage) {
        if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
            auto& state      = it->second;
            state.src_access = _access;
            state.src_stage  = _stage;
            state.dst_access = VK_ACCESS_2_NONE;
            state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
        }
    }

    void VkTracker::FlushSrcState(VulkanTexture* _texture, VkAccessFlagBits2 _access, VkImageLayout _layout, VkPipelineStageFlagBits2 _stage) {
        if (auto it = texture_states.find(_texture); it != texture_states.end()) {
            auto& state      = it->second;
            state.src_access = _access;
            state.src_layout = _layout;
            state.src_stage  = _stage;
            state.dst_access = VK_ACCESS_2_NONE;
            state.dst_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
        }
    }

    void VkTracker::RecordState(VulkanBuffer* _buffer, std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&& _state, uint32_t _src_queue_family, uint32_t _dst_queue_family) {
        RecordState(_buffer, std::get<0>(_state), std::get<1>(_state), _src_queue_family, _dst_queue_family);
    }

    void VkTracker::RegisterFlushBuffer(const BufferView& _view, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage) {
        VulkanBuffer* buffer = ResourceCast(_view.buffer);
        buffer_barriers.emplace_back(VkBufferMemoryBarrier2{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            nullptr,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
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
    void VkTracker::RegisterFlushBufferRange(const BufferView&        _view,
                                             VkAccessFlagBits2        _access,
                                             VkPipelineStageFlagBits2 _stage,
                                             VkAccessFlagBits2        _src_access,
                                             VkPipelineStageFlagBits2 _src_stage) {
        VulkanBuffer* buffer = ResourceCast(_view.buffer);

        if (flush_buffer_ranges.try_emplace(buffer, _view.GetByteOffset(), _view.GetByteSize() + _view.GetByteOffset()).second) {
            auto& state = flush_buffer_states[buffer];
            state.dst_access |= _access;
            state.dst_stage |= _stage;
            state.src_access = _src_access;
            state.src_stage  = _src_stage;
            pending_buffers.insert(buffer);
        } else {
            auto& range = flush_buffer_ranges[buffer];
            range.min   = Min(range.min, _view.GetByteOffset());
            range.max   = Max(range.max, _view.GetByteSize() + _view.GetByteOffset());
        }
    }

    void VkTracker::RecordState(VulkanTexture*                                                     _texture,
                                std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&& _state,
                                uint32_t                                                           _src_queue_family,
                                uint32_t                                                           _dst_queue_family) {
        RecordState(_texture, std::get<0>(_state), std::get<1>(_state), std::get<2>(_state), 0, _texture->GetNumMips(), _src_queue_family, _dst_queue_family);
    }

    void GetInitImageLayoutAndAccess(VulkanTexture* _texture, VkImageLayout& _layout, VkAccessFlags2& _access, EQueueType _queue) {
        if (!_texture->b_has_preferred_state) {
            _layout = VK_IMAGE_LAYOUT_UNDEFINED;
            _access = VK_ACCESS_2_NONE;
            return;
        }
        if (_texture->b_present) {
            _layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            _access = VK_ACCESS_2_NONE;
            return;
        }
        if (_texture->b_has_preferred_state) {
            _layout = _texture->GetQueuePreferredLayout(_queue);
            _access = VK_ACCESS_2_NONE;
            return;
        }
    }
    void VkTracker::QueueTransferReleaseResource(VulkanTexture* _texture, uint _src_queue, uint _dst_queue, VkImageLayout _src_layout, VkImageLayout _dst_layout) {
        pending_textures.insert(_texture);
        if (auto it = texture_states.find(_texture); it != texture_states.end()) {
            auto& state            = it->second;
            state.src_queue_family = _src_queue;
            state.dst_queue_family = _dst_queue;
            state.src_layout       = _src_layout;
            state.dst_layout       = _dst_layout;
            state.dst_stage        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        } else {
            TextureState state{VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, _dst_layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _src_queue, _dst_queue};
            GetInitImageLayoutAndAccess(_texture, state.src_layout, state.src_access, queue_type);
            texture_states[_texture] = state;
        }
        exported_textures.insert(_texture);
    }

    void VkTracker::QueueTransferReleaseResource(VulkanBuffer* _buffer, uint _src_queue, uint _dst_queue) {

        pending_buffers.insert(_buffer);
        if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
            auto& state            = it->second;
            state.src_queue_family = _src_queue;
            state.dst_queue_family = _dst_queue;
            state.dst_stage        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        } else {
            buffer_states[_buffer] = {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, _src_queue, _dst_queue};
        }
        exported_buffers.insert(_buffer);
    }

    void VkTracker::QueueTransferAcquireResource(VulkanBuffer* _buffer, uint _src_queue, uint _dst_queue, VkAccessFlagBits2 _dst_access, VkPipelineStageFlagBits2 _dst_stage) {
        pending_buffers.insert(_buffer);
        if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
            auto& state            = it->second;
            state.src_queue_family = _src_queue;
            state.dst_queue_family = _dst_queue;
            state.dst_access       = _dst_access;
            state.dst_stage        = _dst_stage;
        } else {
            buffer_states[_buffer] = {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _dst_access, _dst_stage, _src_queue, _dst_queue};
        }
    }

    void VkTracker::QueueTransferAcquireResource(VulkanTexture* _texture, uint _src_queue, uint _dst_queue, VkImageLayout _src_layout, VkImageLayout _dst_layout, VkAccessFlagBits2 _dst_access, VkPipelineStageFlagBits2 _dst_stage) {
        pending_textures.insert(_texture);
        if (auto it = texture_states.find(_texture); it != texture_states.end()) {
            auto& state            = it->second;
            state.src_queue_family = _src_queue;
            state.dst_queue_family = _dst_queue;
            state.src_layout       = _src_layout;
            state.dst_layout       = _dst_layout;
            state.dst_access       = _dst_access;
            state.dst_stage        = _dst_stage;
        } else {
            //src access and stage are ignored in vulkan
            TextureState state{VK_ACCESS_2_NONE, _src_layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, _dst_layout, _dst_stage, _src_queue, _dst_queue};
            texture_states[_texture] = state;
        }
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

    void VkTracker::RecordState(VulkanTexture*           _texture,
                                VkAccessFlagBits2        _access,
                                VkImageLayout            _layout,
                                VkPipelineStageFlagBits2 _stage,
                                uint8_t                  _mip_level,
                                uint8_t                  _mip_count,
                                uint32_t                 _src_queue_family,
                                uint32_t                 _dst_queue_family) {
        // Range range{_mip_level, _mip_count};
        TextureState state{
            VK_ACCESS_2_NONE,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            _access,
            _layout,
            _stage,
            _src_queue_family,
            _dst_queue_family};

        auto state_iter = texture_states.find(_texture);

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
                // target_state.src_stage  = _stage;
            }
            if (target_state.dst_layout != VK_IMAGE_LAYOUT_UNDEFINED && target_state.dst_layout != state.dst_layout) {
                //need push barriers

                // FIXME: Comment the assertion for multiple times of gbuffer passes
                // assert(target_state.dst_stage == state.dst_stage && "state transition error");
            }
            target_state.dst_layout = state.dst_layout;
            target_state.dst_access = state.dst_access;
            target_state.dst_stage  = state.dst_stage;

        } else {
            // bool b_init      = _texture->b_has_init_state;
            // bool b_preferred = _texture->b_has_preferred_state;

            // if (b_preferred || b_init) {
            //     if (_texture->b_present) {
            //         state.src_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            //         state.src_access = VK_ACCESS_2_NONE;
            //         // state.src_stage  = _stage;
            //     } else if (b_preferred)
            //         state.src_layout = _texture->GetPreferredLayout();
            //     else if (b_init) {
            //         state.src_layout = _texture->GetInitlayout();
            //     }
            // }
            GetInitImageLayoutAndAccess(_texture, state.src_layout, state.src_access, queue_type);

            texture_states[_texture] = {state};
        }
        bool is_write = IsWriteState(_layout, _access) || state.src_layout != state.dst_layout;
        MarkWriteable(_texture, is_write);
    }

    void VkTracker::ResolveBarriers() {

        for (VulkanBuffer* buffer : pending_buffers) {

            //normal buffers with certain ranges and states to flush
            if (auto it = buffer_states.find(buffer); it != buffer_states.end()) {
                auto& state = it->second;
                if (((state.dst_access & VK_ACCESS_2_SHADER_WRITE_BIT) == 0) && state.src_access == state.dst_access && state.src_stage == state.dst_stage) {
                    state.dst_access       = VK_ACCESS_2_NONE;
                    state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
                    state.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
                    state.dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
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
                barrier.srcQueueFamilyIndex     = state.src_queue_family;
                barrier.dstQueueFamilyIndex     = state.dst_queue_family;
                barrier.buffer                  = buffer->GetHandle();
                barrier.offset                  = 0;
                barrier.size                    = VK_WHOLE_SIZE;

                state.src_access       = state.dst_access;
                state.src_stage        = state.dst_stage;
                state.dst_access       = VK_ACCESS_2_NONE;
                state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
                state.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
                state.dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
            }

            //internal buffers with certain ranges and states to flush
            if (auto it = flush_buffer_ranges.find(buffer); it != flush_buffer_ranges.end()) {
                auto state = flush_buffer_states[buffer];
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
                barrier.offset                  = it->second.min;
                barrier.size                    = it->second.max - it->second.min;
            }
        }

        for (VulkanTexture* texture : pending_textures) {
            if (auto it = texture_states.find(texture); it != texture_states.end()) {
                auto& state = it->second;
                if (((state.dst_access & VK_ACCESS_2_SHADER_WRITE_BIT) == 0) && state.src_access == state.dst_access && state.src_stage == state.dst_stage && state.src_layout == state.dst_layout) {
                    state.dst_access       = VK_ACCESS_2_NONE;
                    state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
                    state.dst_layout       = VK_IMAGE_LAYOUT_UNDEFINED;
                    state.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
                    state.dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
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
                barrier.srcQueueFamilyIndex             = state.src_queue_family;
                barrier.dstQueueFamilyIndex             = state.dst_queue_family;
                barrier.image                           = texture->GetHandle();
                barrier.subresourceRange.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(texture->GetAspectFlags());
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount     = 1;
                barrier.subresourceRange.baseMipLevel   = 0;
                barrier.subresourceRange.levelCount     = texture->GetNumMips();
                barrier.oldLayout                       = state.src_layout;
                barrier.newLayout                       = state.dst_layout;

                state.src_access = state.dst_access;
                state.src_stage  = state.dst_stage;
                state.src_layout = state.dst_layout;

                state.dst_access       = VK_ACCESS_2_NONE;
                state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
                state.dst_layout       = VK_IMAGE_LAYOUT_UNDEFINED;
                state.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
                state.dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
            }
        }
        pending_buffers.clear();
        pending_textures.clear();
        flush_buffer_ranges.clear();
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
            if (exported_textures.find(texture) != exported_textures.end()) continue;
            texture->b_has_preferred_state = true;
            VkImageLayout layout           = texture->GetQueuePreferredLayout(queue_type);
            if (texture->b_present) continue;
            texture_barriers.emplace_back();
            VkImageMemoryBarrier2& barrier          = texture_barriers.back();
            barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.pNext                           = nullptr;
            barrier.srcAccessMask                   = state.src_access;
            barrier.dstAccessMask                   = VK_ACCESS_2_NONE;
            barrier.srcStageMask                    = state.src_stage;
            barrier.dstStageMask                    = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.image                           = texture->GetHandle();
            barrier.subresourceRange.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(texture->GetAspectFlags());
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount     = 1;
            barrier.subresourceRange.baseMipLevel   = 0;
            barrier.subresourceRange.levelCount     = texture->GetNumMips();
            barrier.oldLayout                       = state.src_layout;
            barrier.newLayout                       = layout;

            state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
            state.dst_access = VK_ACCESS_2_NONE;
            state.dst_layout = layout;
        }
        VkAccessFlags2        src_buffer_access = VK_ACCESS_2_NONE;
        VkPipelineStageFlags2 src_buffer_stages = VK_PIPELINE_STAGE_2_NONE;
        for (auto& [buffer, state] : buffer_states) {
            if (exported_buffers.find(buffer) != exported_buffers.end()) continue;
            src_buffer_access |= state.src_access;
            src_buffer_stages |= state.src_stage;
            // state.dst_access = VK_ACCESS_2_NONE;
            // state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;

            // VkBufferMemoryBarrier2& barrier = buffer_barriers.emplace_back();
            // barrier.sType                   = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            // barrier.pNext                   = nullptr;
            // barrier.srcAccessMask           = state.src_access;
            // barrier.dstAccessMask           = VK_ACCESS_2_NONE;
            // barrier.srcStageMask            = state.src_stage;
            // barrier.dstStageMask            = last_stage;
            // barrier.srcQueueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
            // barrier.dstQueueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
            // barrier.buffer                  = buffer->GetHandle();
            // barrier.offset                  = 0;
            // barrier.size                    = VK_WHOLE_SIZE;
        }

        for (auto& [buffer, state] : flush_buffer_states) {
            src_buffer_access |= state.dst_access;
            src_buffer_stages |= state.dst_stage;

            // VkBufferMemoryBarrier2& barrier = buffer_barriers.emplace_back();
            // barrier.sType                   = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            // barrier.pNext                   = nullptr;
            // barrier.srcAccessMask           = state.dst_access;
            // barrier.dstAccessMask           = VK_ACCESS_2_NONE;
            // barrier.srcStageMask            = state.dst_stage;
            // barrier.dstStageMask            = last_stage;
            // barrier.srcQueueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
            // barrier.dstQueueFamilyIndex     = VK_QUEUE_FAMILY_IGNORED;
            // barrier.buffer                  = buffer->GetHandle();
            // barrier.offset                  = 0;
            // barrier.size                    = VK_WHOLE_SIZE;
        }
        if (!buffer_states.empty() || !flush_buffer_states.empty()) {
            //memory barrier
            memory_barriers.emplace_back();
            VkMemoryBarrier2& barrier = memory_barriers.back();
            barrier.sType             = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.pNext             = nullptr;
            barrier.srcAccessMask     = src_buffer_access;
            barrier.dstAccessMask     = VK_ACCESS_2_MEMORY_READ_BIT |
                                    VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.srcStageMask = src_buffer_stages;
            barrier.dstStageMask = last_stage;
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
        write_blas_states.clear();
        flush_buffer_states.clear();
        exported_buffers.clear();
        exported_textures.clear();
    }

}// namespace Moer::Render