#include "VulkanResourceTracker.h"
#include "VulkanCommand.h"
#include "VulkanPlatform.h"

#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "VulkanRHITrace.h"
#include "log/LogSystem.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "string/StringConvert.h"
#include "vulkan/vulkan_core.h"

namespace Moer::Render {
namespace {

// Set via debugger to trace a specific resource: s_barrier_debug_name = L"denoised_specular_lighting"
// Debug: set to e.g. L"denoised" to trace barrier state for matching resources.
// Filter uses StringView (wide) comparison — no UTF-8 conversion.
static const wchar_t* s_barrier_debug_filter = L""; // set to e.g. L"denoised" to enable

static bool MatchDebugName(StringView name) {
    return s_barrier_debug_filter && s_barrier_debug_filter[0] &&
           name.find(StringView(s_barrier_debug_filter)) != StringView::npos;
}

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

static Utf8String BufferName(const VulkanBuffer* buffer) {
    return buffer != nullptr ? PlatformToUtf8(buffer->GetName()) : Utf8String("<null>");
}

static Utf8String TextureName(const VulkanTexture* texture) {
    return texture != nullptr ? PlatformToUtf8(texture->GetName()) : Utf8String("<null>");
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

static VkPipelineStageFlags2 ShaderStageForPass(EPassType pass_type) {
    switch (pass_type) {
        case EPassType::Graphics:
            return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case EPassType::Compute:
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case EPassType::Raytracing:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case EPassType::Copy:
        default:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
}

static EPassType PassTypeForQueue(EQueueType queue) {
    switch (queue) {
        case EQueueType::Compute:
            return EPassType::Compute;
        case EQueueType::Copy:
            return EPassType::Copy;
        case EQueueType::Graphics:
        case EQueueType::Ignore:
        case EQueueType::Num:
        default:
            return EPassType::Graphics;
    }
}

static ERHIResourceLastAccessKind LastAccessKind(bool access_write) {
    return access_write ? ERHIResourceLastAccessKind::Write : ERHIResourceLastAccessKind::Read;
}

static VkPipelineStageFlags2 AttachmentStageForTexture(ETextureState state, EPassType pass_type) {
    if (pass_type != EPassType::Graphics) {
        return ShaderStageForPass(pass_type);
    }
    switch (state) {
        case ETextureState::RENDER_TARGET:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case ETextureState::DEPTH_STENCIL_READ:
        case ETextureState::DEPTH_STENCIL_WRITE:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        default:
            return ShaderStageForPass(pass_type);
    }
}

auto VkTracker::ReadBuffer(VulkanBuffer* _buffer, EBufferState _state, EPassType _type)
    -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
    (void)_buffer;
    switch (_state) {
        case EBufferState::UNDEFINED:
            return {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
        case EBufferState::TRANSFER_SRC:
            return {VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT};
        case EBufferState::TRANSFER_DST:
            return {VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT};
        case EBufferState::VERTEX_BUFFER:
            return {VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT};
        case EBufferState::INDEX_BUFFER:
            return {VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT};
        case EBufferState::INDIRECT_ARGUMENT:
            return {VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT};
        case EBufferState::SHADER_RESOURCE:
            return {VK_ACCESS_2_SHADER_READ_BIT, ShaderStageForPass(_type)};
        case EBufferState::UNORDERED_ACCESS:
            return {VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, ShaderStageForPass(_type)};
        case EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT:
            return {
                VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            };
        case EBufferState::ACCELERATION_STRUCTURE_READ:
            return {
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                    ShaderStageForPass(_type)
            };
        case EBufferState::ACCELERATION_STRUCTURE_WRITE:
            return {
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            };
        default:
            return {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
    }
}

auto VkTracker::WriteBuffer(VulkanBuffer* _buffer, EBufferState _state, EPassType _type)
    -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2> {
    (void)_buffer;
    switch (_state) {
        case EBufferState::UNDEFINED:
            return {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
        case EBufferState::TRANSFER_DST:
            return {VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT};
        case EBufferState::UNORDERED_ACCESS:
            return {VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, ShaderStageForPass(_type)};
        case EBufferState::ACCELERATION_STRUCTURE_WRITE:
            return {
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            };
        default:
            return ReadBuffer(_buffer, _state, _type);
    }
}

auto VkTracker::ReadTexture(VulkanTexture* _texture, ETextureState _state, EPassType _type)
    -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {
    auto preferred_read_layout = [_texture](VkImageLayout layout) {
        if (_texture->GetPreferredLayout() != VK_IMAGE_LAYOUT_GENERAL) {
            return layout;
        }
        if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
            layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
            return VK_IMAGE_LAYOUT_GENERAL;
        }
        return layout;
    };

    switch (_state) {
        case ETextureState::UNDEFINED:
            return {VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE};
        case ETextureState::TRANSFER_SRC:
            return {
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT
            };
        case ETextureState::TRANSFER_DST:
            return {
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT
            };
        case ETextureState::SHADER_RESOURCE:
            return {
                VK_ACCESS_2_SHADER_READ_BIT,
                preferred_read_layout(
                    uint(_texture->GetAspectFlags() & ETextureAspectFlags::DEPTH_SLICE) != 0u ?
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                ),
                ShaderStageForPass(_type)
            };
        case ETextureState::SAMPLED:
            return {
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                preferred_read_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                ShaderStageForPass(_type)
            };
        case ETextureState::RENDER_TARGET:
            return {
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                AttachmentStageForTexture(_state, _type)
            };
        case ETextureState::DEPTH_STENCIL_READ:
            return {
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                preferred_read_layout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
                AttachmentStageForTexture(_state, _type)
            };
        case ETextureState::DEPTH_STENCIL_WRITE:
            return {
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                AttachmentStageForTexture(_state, _type)
            };
        case ETextureState::UNORDERED_ACCESS:
            return {
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL,
                ShaderStageForPass(_type)
            };
        default:
            return {VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE};
    }
}

auto VkTracker::WriteTexture(VulkanTexture* _texture, ETextureState _state, EPassType _type)
    -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> {
    switch (_state) {
        case ETextureState::UNDEFINED:
            return {VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE};
        case ETextureState::TRANSFER_DST:
            return {
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT
            };
        case ETextureState::RENDER_TARGET:
            return {
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                AttachmentStageForTexture(_state, _type)
            };
        case ETextureState::DEPTH_STENCIL_WRITE:
            return {
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                AttachmentStageForTexture(_state, _type)
            };
        case ETextureState::UNORDERED_ACCESS:
            return {
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL,
                ShaderStageForPass(_type)
            };
        default:
            auto read_state = ReadTexture(_texture, _state, _type);
            return {
                VK_ACCESS_2_MEMORY_WRITE_BIT,
                std::get<1>(read_state),
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
            };
    }
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

void VkTracker::SetBufferOverlap(VulkanBuffer* _buffer, bool _enabled) {
    if (_buffer == nullptr) {
        return;
    }
    if (_enabled) {
        buffer_overlap_states.insert(_buffer);
    } else {
        buffer_overlap_states.erase(_buffer);
    }
}

void VkTracker::RecordState(
    VulkanBuffer*            _buffer,
    VkAccessFlagBits2        _access,
    VkPipelineStageFlagBits2 _stage,
    uint32_t                 _src_queue_family,
    uint32_t                 _dst_queue_family
) {
    if (!buffer_states.contains(_buffer)) {
        LoadPersistentState(_buffer);
    }
    const bool is_write = IsWriteState(_access);
    MarkWriteable(_buffer, is_write);
    pending_buffers.insert(_buffer);

    if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
        auto& state = it->second;
        // BufferOverlap only relaxes write-after-write ordering for the tagged buffer
        // inside the current command list segment.
        if (is_write && buffer_overlap_states.contains(_buffer) && IsWriteState(state.src_access)) {
            state.src_access = VK_ACCESS_2_NONE;
            state.src_stage  = VK_PIPELINE_STAGE_2_NONE;
        }
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

void VkTracker::EmitLocalTransition(
    VulkanBuffer*            _buffer,
    VkAccessFlagBits2        _access,
    VkPipelineStageFlagBits2 _stage,
    uint32_t                 _src_queue_family,
    uint32_t                 _dst_queue_family
) {
    RecordState(_buffer, _access, _stage, _src_queue_family, _dst_queue_family);
}

void VkTracker::EmitLocalTransition(
    VulkanBuffer*                                       _buffer,
    std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&& _state,
    uint32_t                                            _src_queue_family,
    uint32_t                                            _dst_queue_family
) {
    RecordState(_buffer, std::move(_state), _src_queue_family, _dst_queue_family);
}

void VkTracker::EmitLocalTransition(
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
    RecordState(
        _texture,
        _access,
        _layout,
        _stage,
        _mip_level,
        _mip_count,
        _array_layer,
        _array_count,
        _src_queue_family,
        _dst_queue_family
    );
}

void VkTracker::EmitLocalTransition(
    VulkanTexture*                                                     _texture,
    std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&& _state,
    uint32_t                                                           _src_queue_family,
    uint32_t                                                           _dst_queue_family
) {
    RecordState(_texture, std::move(_state), _src_queue_family, _dst_queue_family);
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

void VkTracker::LoadPersistentState(VulkanBuffer* buffer) {
    if (buffer == nullptr || buffer_states.find(buffer) != buffer_states.end()) {
        return; // already tracked
    }

    const BufferPersistentState ps = buffer->GetPersistentState();
    if (!ps.known) {
        // Truly unknown — use UNDEFINED as src.
        buffer_states[buffer] = BufferState{
            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED
        };
        return;
    }

    if (ps.state == EBufferState::UNDEFINED) {
        // Known initial UNDEFINED — use NONE access/stage.
        buffer_states[buffer] = BufferState{
            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED
        };
        return;
    }

    const bool foreign_queue = ps.owner_queue != EQueueType::Ignore && ps.owner_queue != queue_type;
    const EPassType pass_type = (foreign_queue ? queue_type : ps.owner_queue) == EQueueType::Compute ?
                                    EPassType::Compute : EPassType::Graphics;

    auto [access, stage] = (ps.last_access_kind == ERHIResourceLastAccessKind::Write) ?
                               WriteBuffer(buffer, ps.state, pass_type) :
                               ReadBuffer(buffer, ps.state, pass_type);

    VkAccessFlagBits2        src_access = static_cast<VkAccessFlagBits2>(access);
    VkPipelineStageFlagBits2 src_stage  = static_cast<VkPipelineStageFlagBits2>(stage);
    if (foreign_queue) {
        src_access = VK_ACCESS_2_NONE;
        src_stage  = VK_PIPELINE_STAGE_2_NONE;
    }

    buffer_states[buffer] = BufferState{
        src_access, src_stage,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED
    };

    RHITRACE_BARRIER_LOG(
        verbose,
        "[RHITrace][LoadPersistent][{}][Buffer] name={} handle=0x{:x} known={} owner={} has_writer={} src_stage=0x{:x} src_access=0x{:x}",
        QueueTypeName(queue_type),
        BufferName(buffer),
        uint64(buffer),
        ps.known,
        QueueTypeName(ps.owner_queue),
        ps.last_access_kind == ERHIResourceLastAccessKind::Write,
        uint64(src_stage),
        uint64(src_access)
    );
}

void VkTracker::LoadPersistentState(
    VulkanTexture* texture,
    uint8 mip_level, uint8 mip_count,
    uint8 array_layer, uint8 array_count
) {
    if (texture == nullptr) return;

    const uint8 resolved_mip_count =
        mip_count == kRemainingSubresource ? uint8(texture->GetNumMips() - mip_level) : mip_count;

    for (uint8 mip = 0; mip < resolved_mip_count; ++mip) {
        auto key = MakeTextureStateKey(texture, uint8(mip_level + mip), 1, array_layer, array_count);
        if (texture_states.find(key) != texture_states.end()) {
            continue; // already tracked
        }

        const TexturePersistentState ps = texture->GetPersistentState();
        const bool dbg = MatchDebugName(texture->GetName());

        if (!ps.known) {
            if (dbg) LOG_INFO(MOER_TEXT("[Barrier] {} mip={}: UNKNOWN → src=UNDEFINED"), texture->GetName(), key.mip_level);
            texture_states[key] = TextureState{
                VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED
            };
            continue;
        }

        if (ps.state == ETextureState::UNDEFINED) {
            if (dbg) LOG_INFO(MOER_TEXT("[Barrier] {} mip={}: known=UNDEFINED → src=UNDEFINED"), texture->GetName(), key.mip_level);
            texture_states[key] = TextureState{
                VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED
            };
            continue;
        }

        if (dbg) LOG_INFO(MOER_TEXT("[Barrier] {} mip={}: known=%d owner=%hs writer=%d"), texture->GetName(), key.mip_level, int(ps.state), QueueTypeName(ps.owner_queue), ps.last_access_kind == ERHIResourceLastAccessKind::Write);

        const bool foreign_queue = ps.owner_queue != EQueueType::Ignore && ps.owner_queue != queue_type;
        const EPassType pass_type = (foreign_queue ? queue_type : ps.owner_queue) == EQueueType::Compute ?
                                        EPassType::Compute : EPassType::Graphics;

        std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> result;
        if (ps.last_access_kind == ERHIResourceLastAccessKind::Write) {
            result = WriteTexture(texture, ps.state, pass_type);
        } else {
            result = ReadTexture(texture, ps.state, pass_type);
        }
        VkAccessFlagBits2        src_access = static_cast<VkAccessFlagBits2>(std::get<0>(result));
        VkImageLayout            src_layout = std::get<1>(result);
        VkPipelineStageFlagBits2 src_stage  = static_cast<VkPipelineStageFlagBits2>(std::get<2>(result));

        if (foreign_queue) {
            src_access = VK_ACCESS_2_NONE;
            src_stage  = VK_PIPELINE_STAGE_2_NONE;
        }

        if (texture->b_present) {
            src_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            src_access = VK_ACCESS_2_NONE;
            src_stage  = VK_PIPELINE_STAGE_2_NONE;
        }

        texture_states[key] = TextureState{
            src_access, src_layout, src_stage,
            VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED
        };

        RHITRACE_BARRIER_LOG(
            verbose,
            "[RHITrace][LoadPersistent][{}][Texture] name={} handle=0x{:x} mip={} layer={} known={} owner={} has_writer={} src_stage=0x{:x} src_access=0x{:x} src_layout={}",
            QueueTypeName(queue_type),
            TextureName(texture),
            uint64(texture),
            key.mip_level,
            key.array_layer,
            ps.known,
            QueueTypeName(ps.owner_queue),
            ps.last_access_kind == ERHIResourceLastAccessKind::Write,
            uint64(src_stage),
            uint64(src_access),
            int(src_layout)
        );
    }
}

// §9.3: Initialize tracker src states from preprocess seed_tracker.
// For each known resource, sets the src layout/access/stage from the preprocess end-state.
// For unknown resources (known==false), sets UNDEFINED / NONE per §3.2.
#if 0 // Deprecated: InitFromSeed replaced by LoadPersistentState {
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
            const bool      foreign_queue = entry.owner_queue != EQueueType::Ignore && entry.owner_queue != queue_type;
            const EPassType pass_type     = queue_to_pass_type(foreign_queue ? queue_type : entry.owner_queue);
            std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> result;
            if (entry.has_writer) {
                result = WriteTexture(entry.texture, entry.texture_state, pass_type);
            } else {
                result = ReadTexture(entry.texture, entry.texture_state, pass_type);
            }
            src_access = static_cast<VkAccessFlagBits2>(std::get<0>(result));
            src_layout = std::get<1>(result);
            src_stage  = static_cast<VkPipelineStageFlagBits2>(std::get<2>(result));
            if (foreign_queue) {
                src_access = VK_ACCESS_2_NONE;
                src_stage  = VK_PIPELINE_STAGE_2_NONE;
            }
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
            const bool      foreign_queue = entry.owner_queue != EQueueType::Ignore && entry.owner_queue != queue_type;
            const EPassType pass_type     = queue_to_pass_type(foreign_queue ? queue_type : entry.owner_queue);
            std::tuple<VkAccessFlags2, VkPipelineStageFlags2> result;
            if (entry.has_writer) {
                result = WriteBuffer(entry.buffer, entry.buffer_state, pass_type);
            } else {
                result = ReadBuffer(entry.buffer, entry.buffer_state, pass_type);
            }
            state.src_access = static_cast<VkAccessFlagBits2>(std::get<0>(result));
            state.src_stage  = static_cast<VkPipelineStageFlagBits2>(std::get<1>(result));
            if (foreign_queue) {
                state.src_access = VK_ACCESS_2_NONE;
                state.src_stage  = VK_PIPELINE_STAGE_2_NONE;
            }
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

#endif // InitFromSeed
void VkTracker::SetTrackedState(
    VulkanBuffer* buffer,
    EBufferState  state,
    EQueueType    owner_queue,
    bool          access_write
) {
    if (buffer == nullptr) {
        return;
    }

    const EPassType pass_type = PassTypeForQueue(owner_queue);
    const auto      vk_state  = access_write ? WriteBuffer(buffer, state, pass_type) :
                                                ReadBuffer(buffer, state, pass_type);
    buffer_states[buffer] = BufferState{
        static_cast<VkAccessFlagBits2>(std::get<0>(vk_state)),
        static_cast<VkPipelineStageFlagBits2>(std::get<1>(vk_state)),
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_NONE,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED
    };
    pending_buffers.erase(buffer);
    MarkWriteable(buffer, access_write);
    buffer->SetPersistentState(BufferPersistentState{
        .known = state != EBufferState::UNDEFINED,
        .owner_queue = owner_queue,
        .state = state,
        .last_access_kind = state == EBufferState::UNDEFINED ? ERHIResourceLastAccessKind::Unknown :
                                                               LastAccessKind(access_write)
    });
}

void VkTracker::SetTrackedState(
    VulkanTexture* texture,
    ETextureState  state,
    EQueueType     owner_queue,
    bool           access_write,
    uint8          mip_level,
    uint8          mip_count,
    uint8          array_layer,
    uint8          array_count
) {
    if (texture == nullptr) {
        return;
    }

    const uint8 resolved_mip_count =
        mip_count == kRemainingSubresource ? uint8(texture->GetNumMips() - mip_level) : mip_count;
    const uint8 resolved_array_count =
        array_count == kRemainingSubresource ? uint8(texture->GetNumArray() - array_layer) : array_count;
    ValidateSubresourceRange(texture, mip_level, resolved_mip_count, array_layer, resolved_array_count);

    const EPassType pass_type = PassTypeForQueue(owner_queue);
    const auto      vk_state  = access_write ? WriteTexture(texture, state, pass_type) :
                                                ReadTexture(texture, state, pass_type);
    const VkAccessFlagBits2        access = static_cast<VkAccessFlagBits2>(std::get<0>(vk_state));
    const VkImageLayout            layout = std::get<1>(vk_state);
    const VkPipelineStageFlagBits2 stage  = static_cast<VkPipelineStageFlagBits2>(std::get<2>(vk_state));

    for (uint8 mip = 0; mip < resolved_mip_count; ++mip) {
        auto key = MakeTextureStateKey(texture, uint8(mip_level + mip), 1, array_layer, resolved_array_count);
        texture_states[key] = TextureState{
            access,
            layout,
            stage,
            VK_ACCESS_2_NONE,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_NONE,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED
        };
        pending_textures.erase(key);
        MarkWriteable(key, access_write);
    }

    const TexturePersistentState persistent{
        .known = state != ETextureState::UNDEFINED,
        .owner_queue = owner_queue,
        .state = state,
        .last_access_kind = state == ETextureState::UNDEFINED ? ERHIResourceLastAccessKind::Unknown :
                                                                LastAccessKind(access_write)
    };
    if (MatchDebugName(texture->GetName())) {
        LOG_INFO(MOER_TEXT("[SetTracked] {} → state=%d owner=%hs writer=%d"), texture->GetName(), int(state), QueueTypeName(owner_queue), access_write);
    }
    texture->SetPersistentState(persistent);
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
void VkTracker::EmitReleasePlan(
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

void VkTracker::EmitReleasePlan(VulkanBuffer* _buffer, uint _src_queue, uint _dst_queue) {

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

void VkTracker::EmitAcquirePlan(
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

void VkTracker::EmitAcquirePlan(
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

    // Auto-load from PersistentState when the resource is first encountered.
    if (!texture_states.contains(key)) {
        LoadPersistentState(_texture, _mip_level, 1, _array_layer, _array_count);
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

void VkTracker::ResolveBarriers() {

    for (VulkanBuffer* buffer : pending_buffers) {

        //normal buffers with certain ranges and states to flush
        if (auto it = buffer_states.find(buffer); it != buffer_states.end()) {
            auto& state = it->second;
            const bool same_state = state.src_access == state.dst_access && state.src_stage == state.dst_stage;
            const bool write_after_write = IsWriteState(state.src_access) && IsWriteState(state.dst_access);
            const bool queue_transfer = state.src_queue_family != VK_QUEUE_FAMILY_IGNORED &&
                                        state.dst_queue_family != VK_QUEUE_FAMILY_IGNORED &&
                                        state.src_queue_family != state.dst_queue_family;
            if (!queue_transfer && same_state && !write_after_write) {
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
            const bool same_state = state.src_access == state.dst_access && state.src_stage == state.dst_stage &&
                                    state.src_layout == state.dst_layout;
            const bool write_after_write = IsWriteState(state.src_layout, state.src_access) &&
                                           IsWriteState(state.dst_layout, state.dst_access);
            const bool queue_transfer = state.src_queue_family != VK_QUEUE_FAMILY_IGNORED &&
                                        state.dst_queue_family != VK_QUEUE_FAMILY_IGNORED &&
                                        state.src_queue_family != state.dst_queue_family;
            if (!queue_transfer && same_state && !write_after_write) {
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
    flush_buffer_states.clear();
    buffer_overlap_states.clear();
    exported_buffers.clear();
    exported_textures.clear();
}

} // namespace Moer::Render
