#include "VulkanResourceTracker.h"
#include "VulkanCommand.h"
#include "VulkanPlatform.h"

#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "VulkanRHITrace.h"
#include "log/LogSystem.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "vulkan/vulkan_core.h"

#include <string_view>

namespace Moer::Render {
namespace {

static const char* QueueTypeName(EQueueType queue_type) {
    switch (queue_type) {
        case EQueueType::Graphics:
            return "Graphics";
        case EQueueType::Compute:
            return "Compute";
        case EQueueType::Copy:
            return "Copy";
        case EQueueType::Ignore:
            return "Ignore";
        case EQueueType::Num:
        default:
            return "Unknown";
    }
}

static std::string_view BufferName(const VulkanBuffer* buffer) {
    return buffer != nullptr ? buffer->GetName() : "<null>";
}

static std::string_view TextureName(const VulkanTexture* texture) {
    return texture != nullptr ? texture->GetName() : "<null>";
}

static const char* VkLayoutStr(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:                        return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL:                          return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:         return "COLOR_ATTACHMENT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_STENCIL_READ_ONLY";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:        return "SHADER_READ_ONLY";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:             return "TRANSFER_SRC";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:             return "TRANSFER_DST";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:                  return "PRESENT_SRC";
        case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:                return "READ_ONLY";
        case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:               return "ATTACHMENT";
        default:                                               return "UNKNOWN";
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

void VkTracker::SeedSrcState(
    VulkanTexture*           _texture,
    VkAccessFlagBits2        _access,
    VkImageLayout            _layout,
    VkPipelineStageFlagBits2 _stage
) {
    RHITRACE_RESOURCE_LOG(
        TextureName(_texture),
        "[ResourceTrace][Seed][{}] {} layout={} access=0x{:x} stage=0x{:x}",
        QueueTypeName(queue_type),
        TextureName(_texture),
        VkLayoutStr(_layout),
        uint64(_access),
        uint64(_stage)
    );
    uint8 num_mips   = _texture->GetNumMips();
    uint8 num_arrays = _texture->GetNumArray();
    for (uint8 mip = 0; mip < num_mips; ++mip) {
        auto key = MakeTextureStateKey(_texture, mip, 1, 0, num_arrays);
        if (texture_states.find(key) == texture_states.end()) {
            texture_states[key] = TextureState{
                _access,
                _layout,
                _stage,
                VK_ACCESS_2_NONE,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_PIPELINE_STAGE_2_NONE,
                VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED
            };
        }
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

// §9.3: Initialize tracker src states from preprocess seed_tracker.
// For each known resource, sets the src layout/access/stage from the preprocess end-state.
// For unknown resources (known==false), sets UNDEFINED / NONE per §3.2.
void VkTracker::InitFromSeed(const TrackerSeed& seed) {
    auto queue_to_pass_type = [](EQueueType queue) {
        switch (queue) {
            case EQueueType::Compute:
                return EPassType::Compute;
            case EQueueType::Graphics:
            case EQueueType::Copy:
            case EQueueType::Ignore:
            case EQueueType::Num:
            default:
                return EPassType::Graphics;
        }
    };

    // Texture entries — decompose per-mip (matching RecordState's key convention)
    for (const auto& entry : seed.textures) {
        if (!entry.texture) continue;

        VkImageLayout            src_layout;
        VkAccessFlagBits2        src_access;
        VkPipelineStageFlagBits2 src_stage;

        if (!entry.known || entry.texture_state == ETextureState::UNDEFINED) {
            // §3.2: unknown → UNDEFINED, srcAccessMask = 0
            src_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            src_access = VK_ACCESS_2_NONE;
            src_stage  = VK_PIPELINE_STAGE_2_NONE;
        } else if (entry.texture->b_present) {
            src_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            src_access = VK_ACCESS_2_NONE;
            src_stage  = VK_PIPELINE_STAGE_2_NONE;
        } else {
            // Use WriteTexture for write end-states, ReadTexture for read end-states.
            // has_writer records whether the last access in the previous submit was a write.
            const EPassType pass_type = queue_to_pass_type(entry.owner_queue);
            std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> result;
            if (entry.has_writer) {
                result = WriteTexture(entry.texture, entry.texture_state, pass_type);
            } else {
                result = ReadTexture(entry.texture, entry.texture_state, pass_type);
            }
            src_access = static_cast<VkAccessFlagBits2>(std::get<0>(result));
            src_layout = std::get<1>(result);
            src_stage  = static_cast<VkPipelineStageFlagBits2>(std::get<2>(result));
        }

        // Decompose to per-mip entries (same convention as RecordState)
        const uint8_t resolved_mip_count =
            entry.mip_count == kRemainingSubresource
                ? uint8_t(entry.texture->GetNumMips() - entry.mip_level)
                : entry.mip_count;
        for (uint8_t mip = 0; mip < resolved_mip_count; ++mip) {
            auto key = MakeTextureStateKey(
                entry.texture, entry.mip_level + mip, 1, entry.array_layer, entry.array_count
            );
            TextureState state{};
            state.src_layout       = src_layout;
            state.src_access       = src_access;
            state.src_stage        = src_stage;
            state.dst_access       = VK_ACCESS_2_NONE;
            state.dst_layout       = VK_IMAGE_LAYOUT_UNDEFINED;
            state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
            state.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
            state.dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
            texture_states.emplace(key, state);
            RHITRACE_BARRIER_LOG(
                verbose,
                "[RHITrace][Seed][{}][Texture] name={} handle=0x{:x} known={} owner={} has_writer={} mip={} mip_count={} layer={} layer_count={} src_stage=0x{:x} src_access=0x{:x} src_layout={}",
                QueueTypeName(queue_type),
                TextureName(entry.texture),
                uint64(entry.texture),
                entry.known,
                QueueTypeName(entry.owner_queue),
                entry.has_writer,
                key.mip_level,
                key.mip_count,
                key.array_layer,
                key.array_count,
                uint64(state.src_stage),
                uint64(state.src_access),
                int(state.src_layout)
            );
        }
    }
    // Buffer entries
    for (const auto& entry : seed.buffers) {
        if (!entry.buffer) continue;
        BufferState state{};
        state.dst_access       = VK_ACCESS_2_NONE;
        state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
        state.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
        state.dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
        if (!entry.known || entry.buffer_state == EBufferState::UNDEFINED) {
            state.src_access = VK_ACCESS_2_NONE;
            state.src_stage  = VK_PIPELINE_STAGE_2_NONE;
        } else {
            const EPassType pass_type = queue_to_pass_type(entry.owner_queue);
            std::tuple<VkAccessFlags2, VkPipelineStageFlags2> result;
            if (entry.has_writer) {
                result = WriteBuffer(entry.buffer, entry.buffer_state, pass_type);
            } else {
                result = ReadBuffer(entry.buffer, entry.buffer_state, pass_type);
            }
            state.src_access = static_cast<VkAccessFlagBits2>(std::get<0>(result));
            state.src_stage  = static_cast<VkPipelineStageFlagBits2>(std::get<1>(result));
        }
        buffer_states.emplace(entry.buffer, state);
        RHITRACE_BARRIER_LOG(
            verbose,
            "[RHITrace][Seed][{}][Buffer] name={} handle=0x{:x} known={} owner={} has_writer={} src_stage=0x{:x} src_access=0x{:x}",
            QueueTypeName(queue_type),
            BufferName(entry.buffer),
            uint64(entry.buffer),
            entry.known,
            QueueTypeName(entry.owner_queue),
            entry.has_writer,
            uint64(state.src_stage),
            uint64(state.src_access)
        );
    }
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
            if (state.dst_stage != VK_PIPELINE_STAGE_2_NONE ||
                state.dst_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
                state.src_access = state.dst_access;
                state.src_stage  = state.dst_stage;
                state.src_layout = state.dst_layout;
            }
            state.src_queue_family = _src_queue;
            state.dst_queue_family = _dst_queue;
            state.dst_layout       = _dst_layout;
            state.dst_access       = VK_ACCESS_2_NONE;
            state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
            if (state.src_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                state.src_layout = _src_layout;
            }
        } else {
            TextureState state{
                VK_ACCESS_2_NONE,
                _src_layout,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_NONE,
                _dst_layout,
                VK_PIPELINE_STAGE_2_NONE,
                _src_queue,
                _dst_queue
            };
            GetInitImageLayoutAndAccess(_texture, state.src_layout, state.src_access, queue_type);
            if (state.src_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                state.src_layout = _src_layout;
            }
            texture_states.emplace(key, state);
        }
    }
    exported_textures.insert(_texture);
}

void VkTracker::QueueTransferReleaseResource(VulkanBuffer* _buffer, uint _src_queue, uint _dst_queue) {

    pending_buffers.insert(_buffer);
    if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
        auto& state            = it->second;
        if (state.dst_stage != VK_PIPELINE_STAGE_2_NONE) {
            state.src_access = state.dst_access;
            state.src_stage  = state.dst_stage;
        }
        state.src_queue_family = _src_queue;
        state.dst_queue_family = _dst_queue;
        state.dst_access       = VK_ACCESS_2_NONE;
        state.dst_stage        = VK_PIPELINE_STAGE_2_NONE;
    } else {
        buffer_states[_buffer] = {
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_NONE,
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
    pending_buffers.insert(_buffer);
    if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
        auto& state            = it->second;
        state.src_access       = VK_ACCESS_2_NONE;
        state.src_stage        = VK_PIPELINE_STAGE_2_NONE;
        state.src_queue_family = _src_queue;
        state.dst_queue_family = _dst_queue;
        state.dst_access       = _dst_access;
        state.dst_stage        = _dst_stage;
    } else {
        buffer_states[_buffer] = {
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_NONE,
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
    for (uint8 mip = 0; mip < num_mips; ++mip) {
        auto key = MakeTextureStateKey(_texture, mip, 1, 0, num_arrays);
        pending_textures.insert(key);
        if (auto it = texture_states.find(key); it != texture_states.end()) {
            auto& state            = it->second;
            state.src_access       = VK_ACCESS_2_NONE;
            state.src_stage        = VK_PIPELINE_STAGE_2_NONE;
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
                VK_PIPELINE_STAGE_2_NONE,
                _dst_access,
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

//TODO: support all subresource range tracking (currently only per-mip, full array)
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
    uint8 resolved_mip_count =
        _mip_count == kRemainingSubresource ? uint8(_texture->GetNumMips() - _mip_level) : _mip_count;

    // Decompose multi-mip ranges into per-mip entries to prevent overlapping
    // subresource ranges in barriers (Vulkan requires oldLayout to match the
    // actual current layout; overlapping ranges cause desynchronized tracking).
    // （RecordState的时候，需要把多mip的range分解为单mip的range，否则会把0~5和0~1+2~5识别成完全不同的资源）
    if (resolved_mip_count > 1) {
        for (uint8_t i = 0; i < resolved_mip_count; ++i) {
            RecordState(
                _texture,
                _access,
                _layout,
                _stage,
                _mip_level + i,
                1,
                _array_layer,
                _array_count,
                _src_queue_family,
                _dst_queue_family
            );
        }
        return;
    }

    auto         key = MakeTextureStateKey(_texture, _mip_level, 1, _array_layer, _array_count);
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
            RHITRACE_BARRIER_LOG(
                verbose,
                "[RHITrace][BarrierPlan][{}][Buffer] name={} handle=0x{:x} src_stage=0x{:x} dst_stage=0x{:x} src_access=0x{:x} dst_access=0x{:x} src_q={} dst_q={} offset={} size={}",
                QueueTypeName(queue_type),
                BufferName(buffer),
                uint64(buffer),
                uint64(barrier.srcStageMask),
                uint64(barrier.dstStageMask),
                uint64(barrier.srcAccessMask),
                uint64(barrier.dstAccessMask),
                barrier.srcQueueFamilyIndex,
                barrier.dstQueueFamilyIndex,
                barrier.offset,
                barrier.size
            );

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
            RHITRACE_BARRIER_LOG(
                verbose,
                "[RHITrace][BarrierPlan][{}][FlushBuffer] name={} handle=0x{:x} src_stage=0x{:x} dst_stage=0x{:x} src_access=0x{:x} dst_access=0x{:x} offset={} size={}",
                QueueTypeName(queue_type),
                BufferName(buffer),
                uint64(buffer),
                uint64(barrier.srcStageMask),
                uint64(barrier.dstStageMask),
                uint64(barrier.srcAccessMask),
                uint64(barrier.dstAccessMask),
                barrier.offset,
                barrier.size
            );
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
            RHITRACE_BARRIER_LOG(
                verbose,
                "[RHITrace][BarrierPlan][{}][Texture] name={} handle=0x{:x} src_stage=0x{:x} dst_stage=0x{:x} src_access=0x{:x} dst_access=0x{:x} src_q={} dst_q={} old_layout={} new_layout={} mip={} mip_count={} layer={} layer_count={}",
                QueueTypeName(queue_type),
                TextureName(texture),
                uint64(texture),
                uint64(barrier.srcStageMask),
                uint64(barrier.dstStageMask),
                uint64(barrier.srcAccessMask),
                uint64(barrier.dstAccessMask),
                barrier.srcQueueFamilyIndex,
                barrier.dstQueueFamilyIndex,
                int(barrier.oldLayout),
                int(barrier.newLayout),
                barrier.subresourceRange.baseMipLevel,
                barrier.subresourceRange.levelCount,
                barrier.subresourceRange.baseArrayLayer,
                barrier.subresourceRange.layerCount
            );
            RHITRACE_RESOURCE_LOG(
                TextureName(texture),
                "[ResourceTrace][Barrier][{}] {} : {} -> {} (mip={} layer={} access=0x{:x}->0x{:x} stage=0x{:x}->0x{:x})",
                QueueTypeName(queue_type),
                TextureName(texture),
                VkLayoutStr(barrier.oldLayout),
                VkLayoutStr(barrier.newLayout),
                barrier.subresourceRange.baseMipLevel,
                barrier.subresourceRange.baseArrayLayer,
                uint64(barrier.srcAccessMask),
                uint64(barrier.dstAccessMask),
                uint64(barrier.srcStageMask),
                uint64(barrier.dstStageMask)
            );

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
        if (RHITRACE_BARRIER_ENABLED(basic)) {
            RHITRACE_BARRIER_LOG(
                basic,
                "[RHITrace][BarrierDispatch][{}] buffer_count={} texture_count={} memory_count={}",
                QueueTypeName(queue_type),
                buffer_barriers.size(),
                texture_barriers.size(),
                memory_barriers.size()
            );
        }
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
        RHITRACE_RESOURCE_LOG(
            TextureName(texture),
            "[ResourceTrace][Restore][{}] {} : {} -> {} (mip={} layer={})",
            QueueTypeName(queue_type),
            TextureName(texture),
            VkLayoutStr(state.src_layout),
            VkLayoutStr(layout),
            key.mip_level,
            key.array_layer
        );

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
    exported_buffers.clear();
    exported_textures.clear();
}

} // namespace Moer::Render
