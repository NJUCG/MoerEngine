#include "VulkanResourceTracker.h"
#include "VulkanSerialGolden.h"
#include "rhi/RHIRecordDiagnostics.h"
#include "VulkanCommand.h"
#include "VulkanPlatform.h"

#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "log/LogSystem.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"

#include <stdexcept>
#include <type_traits>

namespace Moer::Render {
namespace {
template<typename T>
uint64_t BarrierNativeHandleKey(T _handle) {
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(_handle));
    } else {
        return static_cast<uint64_t>(_handle);
    }
}
} // namespace
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
auto VkTracker::ReadBuffer(VulkanBuffer* _buffer, EBufferState _state, EPassType _type)
    -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
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
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
    };

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
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
    };

    uint buffer_index = static_cast<uint32>(_state) + static_cast<uint32>(_type) * uint(EBufferState::Num);
    return {buffer_access_rules[buffer_index], stage_rules[buffer_index]};
}

auto VkTracker::WriteBuffer(VulkanBuffer* _buffer, EBufferState _state, EPassType _type)
    -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
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
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
    };

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
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
    };

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
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
};

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
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
};

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
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
};

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
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
};

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
auto VkTracker::ReadTexture(VulkanTexture* _texture, ETextureState _state, EPassType _type)
    -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {

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
        return {
            tex_read_access_rules[pass_index],
            depth_read_layout_rules[pass_index],
            tex_read_stage_rules[pass_index]
        };
    }

    return {
        tex_read_access_rules[pass_index], tex_read_layout_rules[pass_index], tex_read_stage_rules[pass_index]
    };
}

auto VkTracker::WriteTexture(VulkanTexture* _texture, ETextureState _state, EPassType _type)
    -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {
    static constexpr VkAccessFlags2 gfx_rules[] = {
        VK_ACCESS_2_NONE,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_ACCESS_2_NONE
    };

    static constexpr VkImageLayout layout_rules[] = {
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_UNDEFINED
    }; // Invalid

    auto index      = static_cast<uint32>(_state);
    auto pass_index = static_cast<uint32>(_state) + uint32(ETextureState::Num) * uint32(_type);
    return {gfx_rules[index], layout_rules[index], tex_write_stage_rules[pass_index]};
}

static bool IsWriteState(VkImageLayout _layout, VkAccessFlags2 _access) {
    bool read_layout = _layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                       _layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (read_layout)
        return false;

    bool read_access = _access & VK_ACCESS_2_SHADER_READ_BIT ||
                       _access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT ||
                       _access & VK_ACCESS_2_TRANSFER_READ_BIT;
    bool write_access = _access & VK_ACCESS_2_SHADER_WRITE_BIT ||
                        _access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT ||
                        _access & VK_ACCESS_2_MEMORY_WRITE_BIT || _access & VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (read_access && !write_access)
        return false;
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
    return _access & VK_ACCESS_2_SHADER_WRITE_BIT || _access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT ||
           _access & VK_ACCESS_2_MEMORY_WRITE_BIT || _access & VK_ACCESS_2_TRANSFER_WRITE_BIT;
}

void VkTracker::MarkWriteable(const TextureSubresourceKeyT<VulkanTexture>& _key, bool _writeable) {
    if (_writeable) {
        writed_state_textures.insert(_key);
    } else {
        writed_state_textures.erase(_key);
    }
}

void VkTracker::MarkWriteable(VulkanBuffer* _buffer, bool _writeable) {
    if (_writeable) {
        writed_state_buffers.insert(_buffer);
    } else {
        writed_state_buffers.erase(_buffer);
    }
}

void VkTracker::RecordState(
    VulkanBuffer*            _buffer,
    VkAccessFlagBits2        _access,
    VkPipelineStageFlagBits2 _stage,
    uint32_t                 _src_queue_family,
    uint32_t                 _dst_queue_family
) {
    for (const auto& released_range : explicit_released_buffer_ranges) {
        if (released_range.buffer == _buffer) {
            throw std::logic_error(
                "a queue-release buffer barrier must be the final overlapping "
                "access in its submit"
            );
        }
    }
    if (explicit_partial_buffers.contains(_buffer)) {
        LOG_CRITICAL(
            "A partial-range explicit buffer barrier cannot be followed by "
            "backend-inferred state tracking in the same submit"
        );
        throw std::logic_error(
            "mixed explicit partial buffer barrier and inferred buffer state"
        );
    }
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

    buffer_states[_buffer] = {
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        _access,
        _stage,
        _src_queue_family,
        _dst_queue_family
    };
}

void VkTracker::FlushSrcState(
    VulkanBuffer*            _buffer,
    VkAccessFlagBits2        _access,
    VkPipelineStageFlagBits2 _stage
) {
    if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
        auto& state      = it->second;
        state.src_access = _access;
        state.src_stage  = _stage;
        state.dst_access = VK_ACCESS_2_NONE;
        state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
    }
}

void VkTracker::FlushSrcState(
    VulkanTexture*           _texture,
    VkAccessFlagBits2        _access,
    VkImageLayout            _layout,
    VkPipelineStageFlagBits2 _stage
) {
    for (auto& [key, state] : texture_states) {
        if (key.texture != _texture) {
            continue;
        }
        state.src_access = _access;
        state.src_layout = _layout;
        state.src_stage  = _stage;
        state.dst_access = VK_ACCESS_2_NONE;
        state.dst_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
    }
}

void VkTracker::RecordState(
    VulkanBuffer*                                       _buffer,
    std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&& _state,
    uint32_t                                            _src_queue_family,
    uint32_t                                            _dst_queue_family
) {
    RecordState(_buffer, std::get<0>(_state), std::get<1>(_state), _src_queue_family, _dst_queue_family);
}

void VkTracker::RegisterFlushBuffer(
    const BufferView&        _view,
    VkAccessFlagBits2        _access,
    VkPipelineStageFlagBits2 _stage
) {
    VulkanBuffer* buffer = ResourceCast(_view.buffer);
    buffer_barriers.emplace_back(
        VkBufferMemoryBarrier2{
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
            _view.GetByteSize()
        }
    );
    auto& state = flush_buffer_states[buffer];
    state.dst_access |= _access;
    state.dst_stage |= _stage;
}
void VkTracker::RegisterFlushBufferRange(
    const BufferView&        _view,
    VkAccessFlagBits2        _access,
    VkPipelineStageFlagBits2 _stage,
    VkAccessFlagBits2        _src_access,
    VkPipelineStageFlagBits2 _src_stage
) {
    VulkanBuffer* buffer = ResourceCast(_view.buffer);

    if (flush_buffer_ranges
            .try_emplace(buffer, _view.GetByteOffset(), _view.GetByteSize() + _view.GetByteOffset())
            .second) {
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

void VkTracker::RecordState(
    VulkanTexture*                                                     _texture,
    std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&& _state,
    uint32_t                                                           _src_queue_family,
    uint32_t                                                           _dst_queue_family
) {
    RecordState(
        _texture,
        std::get<0>(_state),
        std::get<1>(_state),
        std::get<2>(_state),
        0,
        _texture->GetNumMips(),
        0,
        _texture->GetNumArray(),
        _src_queue_family,
        _dst_queue_family
    );
}

void GetInitImageLayoutAndAccess(
    VulkanTexture*  _texture,
    VkImageLayout&  _layout,
    VkAccessFlags2& _access,
    EQueueType      _queue
) {
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
void VkTracker::QueueTransferReleaseResource(
    VulkanTexture* _texture,
    uint           _src_queue,
    uint           _dst_queue,
    VkImageLayout  _src_layout,
    VkImageLayout  _dst_layout
) {
    uint8 num_mips   = _texture->GetNumMips();
    uint8 num_arrays = _texture->GetNumArray();
    for (uint8 mip = 0; mip < num_mips; ++mip) {
        auto key = MakeTextureStateKey(_texture, mip, 1, 0, num_arrays);
        pending_textures.insert(key);
        if (auto it = texture_states.find(key); it != texture_states.end()) {
            auto& state            = it->second;
            state.src_queue_family = _src_queue;
            state.dst_queue_family = _dst_queue;
            state.src_layout       = _src_layout;
            state.dst_layout       = _dst_layout;
            state.dst_stage        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        } else {
            TextureState state{
                VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_NONE,
                _dst_layout,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                _src_queue,
                _dst_queue
            };
            GetInitImageLayoutAndAccess(_texture, state.src_layout, state.src_access, queue_type);
            texture_states.emplace(key, state);
        }
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
        buffer_states[_buffer] = {
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            _src_queue,
            _dst_queue
        };
    }
    exported_buffers.insert(_buffer);
}

void VkTracker::QueueTransferAcquireResource(
    VulkanBuffer*            _buffer,
    uint                     _src_queue,
    uint                     _dst_queue,
    VkAccessFlagBits2        _dst_access,
    VkPipelineStageFlagBits2 _dst_stage
) {
    
    // 下列判断主要针对AMD GPU
    // - AMD GPU没有TransferQueue，RHI会把TransferQueue的命令当做GraphicsQueue来执行
    // - 因此，此处会受到RHI发出的GraphicsQueue->GraphicsQueue的指令
    // - 我们要对这种情况做出处理
    if (_src_queue == _dst_queue) {
        buffer_states[_buffer] = {
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_NONE,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED
        };
        return;
    }

    pending_buffers.insert(_buffer);
    if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
        auto& state            = it->second;
        state.src_queue_family = _src_queue;
        state.dst_queue_family = _dst_queue;
        state.dst_access       = _dst_access;
        state.dst_stage        = _dst_stage;
    } else {
        buffer_states[_buffer] = {
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            _dst_access,
            _dst_stage,
            _src_queue,
            _dst_queue
        };
    }
}

void VkTracker::QueueTransferAcquireResource(
    VulkanTexture*           _texture,
    uint                     _src_queue,
    uint                     _dst_queue,
    VkImageLayout            _src_layout,
    VkImageLayout            _dst_layout,
    VkAccessFlagBits2        _dst_access,
    VkPipelineStageFlagBits2 _dst_stage
) {
    uint8 num_mips   = _texture->GetNumMips();
    uint8 num_arrays = _texture->GetNumArray();

    // 下列判断主要针对AMD GPU
    // - AMD GPU没有TransferQueue，RHI会把TransferQueue的命令当做GraphicsQueue来执行
    // - 因此，此处会受到RHI发出的GraphicsQueue->GraphicsQueue的指令
    // - 我们要对这种情况做出处理
    if (_src_queue == _dst_queue) {
        for (uint8 mip = 0; mip < num_mips; ++mip) {
            auto key = MakeTextureStateKey(_texture, mip, 1, 0, num_arrays);
            texture_states[key] = TextureState{
                VK_ACCESS_2_NONE,
                _dst_layout,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_NONE,
                _dst_layout,
                VK_PIPELINE_STAGE_2_NONE,
                VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED
            };
        }
        return;
    }

    for (uint8 mip = 0; mip < num_mips; ++mip) {
        auto key = MakeTextureStateKey(_texture, mip, 1, 0, num_arrays);
        pending_textures.insert(key);
        if (auto it = texture_states.find(key); it != texture_states.end()) {
            auto& state            = it->second;
            state.src_queue_family = _src_queue;
            state.dst_queue_family = _dst_queue;
            state.src_layout       = _src_layout;
            state.dst_layout       = _dst_layout;
            state.dst_access       = _dst_access;
            state.dst_stage        = _dst_stage;
        } else {
            TextureState state{
                VK_ACCESS_2_NONE,
                _src_layout,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_NONE,
                _dst_layout,
                _dst_stage,
                _src_queue,
                _dst_queue
            };
            texture_states.emplace(key, state);
        }
    }
}

uint8 Min(uint8 a, uint8 b) {
    return a < b ? a : b;
}

TextureSubresourceKeyT<VulkanTexture> VkTracker::MakeTextureStateKey(
    VulkanTexture* _texture,
    uint8          _mip_level,
    uint8          _mip_count,
    uint8          _array_layer,
    uint8          _array_count
) const {
    ValidateSubresourceRange(_texture, _mip_level, _mip_count, _array_layer, _array_count);
    uint8 mip_count =
        _mip_count == kRemainingSubresource ? uint8(_texture->GetNumMips() - _mip_level) : _mip_count;
    uint8 array_count =
        _array_count == kRemainingSubresource ? uint8(_texture->GetNumArray() - _array_layer) : _array_count;
    return {_texture, _mip_level, mip_count, _array_layer, array_count};
}

void VkTracker::EmplaceWriteBLAS(uint64 _blas_buf) {
    write_blas_states.insert(_blas_buf);
}

bool VkTracker::ContainsWriteBLAS(uint64 _blas_buf) {
    return write_blas_states.find(_blas_buf) != write_blas_states.end();
}

void VkTracker::RecordState(
    VulkanTexture*           _texture,
    VkAccessFlagBits2        _access,
    VkImageLayout            _layout,
    VkPipelineStageFlagBits2 _stage,
    uint8_t                  _mip_level,
    uint8_t                  _mip_count,
    uint8_t                  _array_layer,
    uint8_t                  _array_count,
    uint32_t                 _src_queue_family,
    uint32_t                 _dst_queue_family
) {
    if (explicit_partial_aspect_textures.contains(_texture)) {
        LOG_CRITICAL(
            "A partial-aspect explicit texture barrier cannot be followed by "
            "backend-inferred state tracking in the same submit"
        );
        throw std::logic_error(
            "mixed explicit partial-aspect barrier and inferred texture state"
        );
    }
    uint8 resolved_mip_count =
        _mip_count == kRemainingSubresource ? uint8(_texture->GetNumMips() - _mip_level) : _mip_count;
    uint8 resolved_array_count =
        _array_count == kRemainingSubresource ?
            uint8(_texture->GetNumArray() - _array_layer) :
            _array_count;
    const TextureAspectSubresourceRangeT<VulkanTexture> requested_range{
        .subresource = MakeTextureStateKey(
            _texture,
            _mip_level,
            resolved_mip_count,
            _array_layer,
            resolved_array_count
        ),
        .aspects = VulkanEnumTranslator::METoVKImageAspectFlags(
            _texture->GetAspectFlags()
        ),
    };
    for (const auto& released_range :
         explicit_released_texture_ranges) {
        if (TextureAspectSubresourceRangesOverlap(
                released_range,
                requested_range
            )) {
            throw std::logic_error(
                "a queue-release texture barrier must be the final overlapping "
                "access in its submit"
            );
        }
    }
    bool has_explicit_texture_state = false;
    for (const auto& explicit_range : explicit_texture_ranges) {
        if (explicit_range.texture == _texture) {
            has_explicit_texture_state = true;
            break;
        }
    }

    // Decompose multi-mip ranges into per-mip entries to prevent overlapping
    // subresource ranges in barriers (Vulkan requires oldLayout to match the
    // actual current layout; overlapping ranges cause desynchronized tracking).
    // （RecordState的时候，需要把多mip的range分解为单mip的range，否则会把0~5和0~1+2~5识别成完全不同的资源）
    // Explicit RDG ranges may legally change array shape between passes (for
    // example layers 0+4 followed by 1+2). Once a texture enters the explicit
    // state domain, use atomic mip/layer tracker keys without widening the
    // native barriers or changing legacy-only submits.
    if (resolved_mip_count > 1 ||
        (has_explicit_texture_state && resolved_array_count > 1)) {
        for (uint8_t i = 0; i < resolved_mip_count; ++i) {
            const uint8 layer_count =
                has_explicit_texture_state ? resolved_array_count : 1;
            for (uint8 layer = 0; layer < layer_count; ++layer) {
                RecordState(
                    _texture,
                    _access,
                    _layout,
                    _stage,
                    _mip_level + i,
                    1,
                    has_explicit_texture_state ?
                        static_cast<uint8>(_array_layer + layer) :
                        _array_layer,
                    has_explicit_texture_state ? 1 : resolved_array_count,
                    _src_queue_family,
                    _dst_queue_family
                );
            }
        }
        return;
    }

    auto         key = MakeTextureStateKey(_texture, _mip_level, 1, _array_layer, _array_count);
    for (const auto& explicit_range : explicit_texture_ranges) {
        if (!TextureSubresourceRangesOverlap(explicit_range, key) ||
            explicit_range == key) {
            continue;
        }
        LOG_CRITICAL(
            "An explicit texture range cannot be followed by an overlapping "
            "backend-inferred range with a different shape: explicit "
            "mip={}+{} layer={}+{}, inferred mip={}+{} layer={}+{}",
            explicit_range.mip_level,
            explicit_range.mip_count,
            explicit_range.array_layer,
            explicit_range.array_count,
            key.mip_level,
            key.mip_count,
            key.array_layer,
            key.array_count
        );
        throw std::logic_error(
            "mixed explicit and inferred texture ranges have incompatible shapes"
        );
    }
    TextureState state{
        VK_ACCESS_2_NONE,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        _access,
        _layout,
        _stage,
        _src_queue_family,
        _dst_queue_family
    };

    auto state_iter = texture_states.find(key);

    pending_textures.insert(key);
    if (state_iter != texture_states.end()) {
        auto& target_state = state_iter->second;

        if (target_state.src_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            target_state.src_access = VK_ACCESS_2_NONE;
        }

        if (target_state.dst_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
            target_state.dst_layout == state.dst_layout) {
            // Same layout already pending — merge access/stage flags
            target_state.dst_access |= state.dst_access;
            target_state.dst_stage |= state.dst_stage;
        } else {
            // Different layout or first record after resolve — overwrite dst.
            // Do NOT promote the previous dst to src: the barrier for
            // src→previous_dst was never generated, so the actual GPU layout
            // is still src, not previous_dst.  Only ResolveBarriers (after
            // emitting a real barrier) may advance src.
            target_state.dst_layout = state.dst_layout;
            target_state.dst_access = state.dst_access;
            target_state.dst_stage  = state.dst_stage;
        }
    } else {
        GetInitImageLayoutAndAccess(_texture, state.src_layout, state.src_access, queue_type);
        texture_states.emplace(key, state);
    }
    bool is_write = IsWriteState(_layout, _access);
    if (state_iter != texture_states.end()) {
        is_write = is_write || state_iter->second.src_layout != state_iter->second.dst_layout;
    } else {
        auto it = texture_states.find(key);
        if (it != texture_states.end())
            is_write = is_write || it->second.src_layout != it->second.dst_layout;
    }
    MarkWriteable(key, is_write);
}

void VkTracker::EmitExplicitBarrier(
    VulkanBuffer*         _buffer,
    uint64                _offset,
    uint64                _byte_size,
    EBarrierQueueTransferPhase _phase,
    uint32_t              _src_queue_family,
    uint32_t              _dst_queue_family,
    VkPipelineStageFlags2 _src_stage,
    VkAccessFlags2        _src_access,
    VkPipelineStageFlags2 _dst_stage,
    VkAccessFlags2        _dst_access
) {
    if (_buffer == nullptr || _byte_size == 0 ||
        _offset > _buffer->GetByteSize() ||
        _byte_size > _buffer->GetByteSize() - _offset) {
        LOG_CRITICAL(
            "Invalid explicit buffer barrier range: offset={} size={}",
            _offset,
            _byte_size
        );
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
    if (_phase != EBarrierQueueTransferPhase::None &&
        _phase != EBarrierQueueTransferPhase::Release &&
        _phase != EBarrierQueueTransferPhase::Acquire) {
        throw std::invalid_argument(
            "explicit buffer barrier carries an unknown transfer phase"
        );
    }
    const bool ownership =
        _phase != EBarrierQueueTransferPhase::None;
    if ((ownership &&
         (_src_queue_family == VK_QUEUE_FAMILY_IGNORED ||
          _dst_queue_family == VK_QUEUE_FAMILY_IGNORED ||
          _src_queue_family == _dst_queue_family)) ||
        (!ownership &&
         (_src_queue_family != VK_QUEUE_FAMILY_IGNORED ||
          _dst_queue_family != VK_QUEUE_FAMILY_IGNORED))) {
        throw std::invalid_argument(
            "explicit buffer barrier carries an invalid queue-family pair"
        );
    }

    const VkPipelineStageFlags2 native_src_stage =
        _phase == EBarrierQueueTransferPhase::Acquire ?
            VK_PIPELINE_STAGE_2_NONE :
            _src_stage;
    const VkAccessFlags2 native_src_access =
        _phase == EBarrierQueueTransferPhase::Acquire ?
            VK_ACCESS_2_NONE :
            _src_access;
    const VkPipelineStageFlags2 native_dst_stage =
        _phase == EBarrierQueueTransferPhase::Release ?
            VK_PIPELINE_STAGE_2_NONE :
            _dst_stage;
    const VkAccessFlags2 native_dst_access =
        _phase == EBarrierQueueTransferPhase::Release ?
            VK_ACCESS_2_NONE :
            _dst_access;

    buffer_barriers.emplace_back(VkBufferMemoryBarrier2{
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
    });

    if (_phase == EBarrierQueueTransferPhase::Release) {
        explicit_released_buffer_ranges.emplace_back(transfer_range);
        return;
    }

    if (_offset == 0 && _byte_size == _buffer->GetByteSize()) {
        buffer_states[_buffer] = BufferState{
            .src_access      = static_cast<VkAccessFlagBits2>(_dst_access),
            .src_stage       = static_cast<VkPipelineStageFlagBits2>(_dst_stage),
            .dst_access      = VK_ACCESS_2_NONE,
            .dst_stage       = VK_PIPELINE_STAGE_2_NONE,
            .src_queue_family = VK_QUEUE_FAMILY_IGNORED,
            .dst_queue_family = VK_QUEUE_FAMILY_IGNORED,
        };
        MarkWriteable(_buffer, IsWriteState(_dst_access));
    } else {
        explicit_partial_buffers.insert(_buffer);
    }
}

void VkTracker::EmitExplicitBarrier(
    VulkanTexture*        _texture,
    VkImageAspectFlags    _aspects,
    uint8                 _mip_level,
    uint8                 _mip_count,
    uint8                 _array_layer,
    uint8                 _array_count,
    EBarrierQueueTransferPhase _phase,
    uint32_t              _src_queue_family,
    uint32_t              _dst_queue_family,
    VkPipelineStageFlags2 _src_stage,
    VkAccessFlags2        _src_access,
    VkImageLayout         _src_layout,
    VkPipelineStageFlags2 _dst_stage,
    VkAccessFlags2        _dst_access,
    VkImageLayout         _dst_layout
) {
    if (_texture == nullptr) {
        throw std::invalid_argument("explicit texture barrier requires a texture");
    }
    ValidateSubresourceRange(
        _texture, _mip_level, _mip_count, _array_layer, _array_count
    );
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
    if (_phase != EBarrierQueueTransferPhase::None &&
        _phase != EBarrierQueueTransferPhase::Release &&
        _phase != EBarrierQueueTransferPhase::Acquire) {
        throw std::invalid_argument(
            "explicit texture barrier carries an unknown transfer phase"
        );
    }
    const bool ownership =
        _phase != EBarrierQueueTransferPhase::None;
    if ((ownership &&
         (_src_queue_family == VK_QUEUE_FAMILY_IGNORED ||
          _dst_queue_family == VK_QUEUE_FAMILY_IGNORED ||
          _src_queue_family == _dst_queue_family)) ||
        (!ownership &&
         (_src_queue_family != VK_QUEUE_FAMILY_IGNORED ||
          _dst_queue_family != VK_QUEUE_FAMILY_IGNORED))) {
        throw std::invalid_argument(
            "explicit texture barrier carries an invalid queue-family pair"
        );
    }

    const uint8 resolved_mip_count =
        _mip_count == kRemainingSubresource ?
            uint8(_texture->GetNumMips() - _mip_level) :
            _mip_count;
    const uint8 resolved_array_count =
        _array_count == kRemainingSubresource ?
            uint8(_texture->GetNumArray() - _array_layer) :
            _array_count;
    const TextureAspectSubresourceRangeT<VulkanTexture> transfer_range{
        .subresource = MakeTextureStateKey(
            _texture,
            _mip_level,
            resolved_mip_count,
            _array_layer,
            resolved_array_count
        ),
        .aspects = _aspects,
    };
    for (const auto& released_range :
         explicit_released_texture_ranges) {
        if (TextureAspectSubresourceRangesOverlap(
                released_range,
                transfer_range
            )) {
            throw std::logic_error(
                "a queue-release texture barrier must be the final overlapping "
                "access in its submit"
            );
        }
    }
    const VkPipelineStageFlags2 native_src_stage =
        _phase == EBarrierQueueTransferPhase::Acquire ?
            VK_PIPELINE_STAGE_2_NONE :
            _src_stage;
    const VkAccessFlags2 native_src_access =
        _phase == EBarrierQueueTransferPhase::Acquire ?
            VK_ACCESS_2_NONE :
            _src_access;
    const VkPipelineStageFlags2 native_dst_stage =
        _phase == EBarrierQueueTransferPhase::Release ?
            VK_PIPELINE_STAGE_2_NONE :
            _dst_stage;
    const VkAccessFlags2 native_dst_access =
        _phase == EBarrierQueueTransferPhase::Release ?
            VK_ACCESS_2_NONE :
            _dst_access;

    texture_barriers.emplace_back(VkImageMemoryBarrier2{
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
        .subresourceRange =
            VkImageSubresourceRange{
                .aspectMask     = _aspects,
                .baseMipLevel   = _mip_level,
                .levelCount     = resolved_mip_count,
                .baseArrayLayer = _array_layer,
                .layerCount     = resolved_array_count,
            },
    });

    if (_phase == EBarrierQueueTransferPhase::Release) {
        explicit_released_texture_ranges.emplace_back(transfer_range);
        return;
    }

    if (_aspects != available_aspects) {
        // The native barrier above remains exact. The legacy inferred tracker
        // cannot represent aspect-split state, so reject any later inferred
        // access instead of silently widening it.
        explicit_partial_aspect_textures.insert(_texture);
        return;
    }

    for (uint8 mip = 0; mip < resolved_mip_count; ++mip) {
        for (uint8 layer = 0; layer < resolved_array_count; ++layer) {
            const TextureSubresourceKeyT<VulkanTexture> key = MakeTextureStateKey(
                _texture,
                static_cast<uint8>(_mip_level + mip),
                1,
                static_cast<uint8>(_array_layer + layer),
                1
            );
            texture_states[key] = TextureState{
                .src_access       = static_cast<VkAccessFlagBits2>(_dst_access),
                .src_layout       = _dst_layout,
                .src_stage        = static_cast<VkPipelineStageFlagBits2>(_dst_stage),
                .dst_access       = VK_ACCESS_2_NONE,
                .dst_layout       = VK_IMAGE_LAYOUT_UNDEFINED,
                .dst_stage        = VK_PIPELINE_STAGE_2_NONE,
                .src_queue_family = VK_QUEUE_FAMILY_IGNORED,
                .dst_queue_family = VK_QUEUE_FAMILY_IGNORED,
            };
            explicit_texture_ranges.insert(key);
            MarkWriteable(key, IsWriteState(_dst_layout, _dst_access));
        }
    }
    if (IsWriteState(_dst_layout, _dst_access)) {
        // Keep the declared aggregate view as a conservative bindless lookup
        // key while the authoritative state table remains mip/layer atomic.
        // HandleBindless performs overlap-based current-write exclusion.
        writed_state_textures.insert(MakeTextureStateKey(
            _texture,
            _mip_level,
            resolved_mip_count,
            _array_layer,
            resolved_array_count
        ));
    }
}

void VkTracker::ResolveBarriers() {

    for (VulkanBuffer* buffer : pending_buffers) {

        //normal buffers with certain ranges and states to flush
        if (auto it = buffer_states.find(buffer); it != buffer_states.end()) {
            auto& state = it->second;
            if (((state.dst_access & VK_ACCESS_2_SHADER_WRITE_BIT) == 0) &&
                state.src_access == state.dst_access && state.src_stage == state.dst_stage) {
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

    for (const auto& key : pending_textures) {
        if (auto it = texture_states.find(key); it != texture_states.end()) {
            auto& state = it->second;
            if (((state.dst_access & VK_ACCESS_2_SHADER_WRITE_BIT) == 0) &&
                state.src_access == state.dst_access && state.src_stage == state.dst_stage &&
                state.src_layout == state.dst_layout) {
                state.dst_access       = VK_ACCESS_2_NONE;
                state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
                state.dst_layout       = VK_IMAGE_LAYOUT_UNDEFINED;
                state.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
                state.dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
                continue;
            }
            VulkanTexture* texture = key.texture;
            texture_barriers.emplace_back();
            VkImageMemoryBarrier2& barrier = texture_barriers.back();
            barrier.sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.pNext                  = nullptr;
            barrier.srcAccessMask          = state.src_access;
            barrier.dstAccessMask          = state.dst_access;
            barrier.srcStageMask           = state.src_stage;
            barrier.dstStageMask           = state.dst_stage;
            barrier.srcQueueFamilyIndex    = state.src_queue_family;
            barrier.dstQueueFamilyIndex    = state.dst_queue_family;
            barrier.image                  = texture->GetHandle();
            barrier.subresourceRange.aspectMask =
                VulkanEnumTranslator::METoVKImageAspectFlags(texture->GetAspectFlags());
            barrier.subresourceRange.baseArrayLayer = key.array_layer;
            barrier.subresourceRange.layerCount     = key.array_count;
            barrier.subresourceRange.baseMipLevel   = key.mip_level;
            barrier.subresourceRange.levelCount     = key.mip_count;
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

BarrierSemanticDiagnostics VkTracker::GetPendingBarrierDiagnostics(
    VulkanSerialGoldenTrace*   _serial_golden,
    uint64_t                   _group_ordinal,
    const SerialQueueFamilyMap& _queue_family_map
) const {
    BarrierSemanticDiagnostics diagnostics;
    diagnostics.buffer_count  = static_cast<uint32>(buffer_barriers.size());
    diagnostics.texture_count = static_cast<uint32>(texture_barriers.size());
    diagnostics.memory_count  = static_cast<uint32>(memory_barriers.size());

    uint64 digest_sum = 0;
    uint64 digest_xor = 0;
    auto add_barrier_digest = [&](uint64 _barrier_digest) {
        const uint64 mixed = StableRecordHash::Mix(_barrier_digest);
        digest_sum += mixed;
        digest_xor ^= mixed;
    };

    const auto queue_roles = [&](uint32_t _src, uint32_t _dst) {
        const uint32_t src = ResolveSerialQueueRole(_src, _queue_family_map);
        const uint32_t dst = ResolveSerialQueueRole(_dst, _queue_family_map);
        return std::tuple{
            src,
            dst,
            IsCompleteSerialQueueRole(src) && IsCompleteSerialQueueRole(dst),
        };
    };

    for (const VkBufferMemoryBarrier2& barrier : buffer_barriers) {
        const StableSubmissionToken resource =
            _serial_golden->ResolveNativeBuffer(BarrierNativeHandleKey(barrier.buffer));
        const auto [src_queue_role, dst_queue_role, queue_roles_complete] = queue_roles(
            barrier.srcQueueFamilyIndex, barrier.dstQueueFamilyIndex
        );
        const SerialBarrierItem serial_item{
            .group_ordinal       = _group_ordinal,
            .resource            = resource,
            .src_stage_mask      = barrier.srcStageMask,
            .dst_stage_mask      = barrier.dstStageMask,
            .src_access_mask     = barrier.srcAccessMask,
            .dst_access_mask     = barrier.dstAccessMask,
            .src_queue_role      = src_queue_role,
            .dst_queue_role      = dst_queue_role,
            .queue_roles_complete = queue_roles_complete,
            .range_offset        = barrier.offset,
            .range_size          = barrier.size,
        };
        _serial_golden->AddBarrier(serial_item);
        if (!resource.complete) {
            _serial_golden->RecordUnresolvedBufferBarrier(serial_item);
        }
        StableRecordHash hash;
        hash.Add(0x4255464645525f42ull);
        hash.Add(barrier.srcStageMask);
        hash.Add(barrier.srcAccessMask);
        hash.Add(barrier.dstStageMask);
        hash.Add(barrier.dstAccessMask);
        hash.Add(src_queue_role);
        hash.Add(dst_queue_role);
        hash.Add(queue_roles_complete ? 1u : 0u);
        hash.Add(barrier.offset);
        hash.Add(barrier.size);
        add_barrier_digest(hash.Value());
    }
    for (const VkImageMemoryBarrier2& barrier : texture_barriers) {
        const StableSubmissionToken resource =
            _serial_golden->ResolveNativeImage(BarrierNativeHandleKey(barrier.image));
        const auto [src_queue_role, dst_queue_role, queue_roles_complete] = queue_roles(
            barrier.srcQueueFamilyIndex, barrier.dstQueueFamilyIndex
        );
        _serial_golden->AddBarrier(SerialBarrierItem{
            .group_ordinal        = _group_ordinal,
            .resource             = resource,
            .src_stage_mask       = barrier.srcStageMask,
            .dst_stage_mask       = barrier.dstStageMask,
            .src_access_mask      = barrier.srcAccessMask,
            .dst_access_mask      = barrier.dstAccessMask,
            .old_state            = static_cast<uint64_t>(barrier.oldLayout),
            .new_state            = static_cast<uint64_t>(barrier.newLayout),
            .src_queue_role       = src_queue_role,
            .dst_queue_role       = dst_queue_role,
            .queue_roles_complete = queue_roles_complete,
            .aspect_mask          = barrier.subresourceRange.aspectMask,
            .base_mip_level       = barrier.subresourceRange.baseMipLevel,
            .level_count          = barrier.subresourceRange.levelCount,
            .base_array_layer     = barrier.subresourceRange.baseArrayLayer,
            .layer_count          = barrier.subresourceRange.layerCount,
        });
        StableRecordHash hash;
        hash.Add(0x494d4147455f4241ull);
        hash.Add(barrier.srcStageMask);
        hash.Add(barrier.srcAccessMask);
        hash.Add(barrier.dstStageMask);
        hash.Add(barrier.dstAccessMask);
        hash.Add(src_queue_role);
        hash.Add(dst_queue_role);
        hash.Add(queue_roles_complete ? 1u : 0u);
        hash.Add(barrier.oldLayout);
        hash.Add(barrier.newLayout);
        hash.Add(barrier.subresourceRange.aspectMask);
        hash.Add(barrier.subresourceRange.baseMipLevel);
        hash.Add(barrier.subresourceRange.levelCount);
        hash.Add(barrier.subresourceRange.baseArrayLayer);
        hash.Add(barrier.subresourceRange.layerCount);
        add_barrier_digest(hash.Value());
    }
    for (const VkMemoryBarrier2& barrier : memory_barriers) {
        _serial_golden->AddBarrier(SerialBarrierItem{
            .group_ordinal   = _group_ordinal,
            .resource        = StableSubmissionToken::Null(),
            .src_stage_mask  = barrier.srcStageMask,
            .dst_stage_mask  = barrier.dstStageMask,
            .src_access_mask = barrier.srcAccessMask,
            .dst_access_mask = barrier.dstAccessMask,
        });
        StableRecordHash hash;
        hash.Add(0x4d454d4f52595f42ull);
        hash.Add(barrier.srcStageMask);
        hash.Add(barrier.srcAccessMask);
        hash.Add(barrier.dstStageMask);
        hash.Add(barrier.dstAccessMask);
        add_barrier_digest(hash.Value());
    }

    StableRecordHash final_hash;
    final_hash.Add(diagnostics.buffer_count);
    final_hash.Add(diagnostics.texture_count);
    final_hash.Add(diagnostics.memory_count);
    final_hash.Add(digest_sum);
    final_hash.Add(digest_xor);
    diagnostics.digest = final_hash.Value();
    return diagnostics;
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
        memory_barriers.clear();
    }
}

void VkTracker::RestoreState() {
    for (auto& [key, state] : texture_states) {
        VulkanTexture* texture = key.texture;
        if (exported_textures.find(texture) != exported_textures.end())
            continue;
        texture->b_has_preferred_state = true;
        VkImageLayout layout           = texture->GetQueuePreferredLayout(queue_type);
        if (texture->b_present)
            continue;
        texture_barriers.emplace_back();
        VkImageMemoryBarrier2& barrier = texture_barriers.back();
        barrier.sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.pNext                  = nullptr;
        barrier.srcAccessMask          = state.src_access;
        barrier.dstAccessMask          = VK_ACCESS_2_NONE;
        barrier.srcStageMask           = state.src_stage;
        barrier.dstStageMask           = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                  = texture->GetHandle();
        barrier.subresourceRange.aspectMask =
            VulkanEnumTranslator::METoVKImageAspectFlags(texture->GetAspectFlags());
        barrier.subresourceRange.baseArrayLayer = key.array_layer;
        barrier.subresourceRange.layerCount     = key.array_count;
        barrier.subresourceRange.baseMipLevel   = key.mip_level;
        barrier.subresourceRange.levelCount     = key.mip_count;
        barrier.oldLayout                       = state.src_layout;
        barrier.newLayout                       = layout;

        state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
        state.dst_access = VK_ACCESS_2_NONE;
        state.dst_layout = layout;
    }
    VkAccessFlags2        src_buffer_access = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 src_buffer_stages = VK_PIPELINE_STAGE_2_NONE;
    for (auto& [buffer, state] : buffer_states) {
        if (exported_buffers.find(buffer) != exported_buffers.end())
            continue;
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
        barrier.dstAccessMask     = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.srcStageMask      = src_buffer_stages;
        barrier.dstStageMask      = last_stage;
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
    flush_buffer_ranges.clear();
    pending_buffers.clear();
    pending_textures.clear();
    explicit_partial_buffers.clear();
    explicit_partial_aspect_textures.clear();
    explicit_released_buffer_ranges.clear();
    explicit_released_texture_ranges.clear();
    explicit_texture_ranges.clear();
    exported_buffers.clear();
    exported_textures.clear();
}

} // namespace Moer::Render
