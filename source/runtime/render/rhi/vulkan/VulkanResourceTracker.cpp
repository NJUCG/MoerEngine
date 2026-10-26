#include "VulkanEnumTranslator.h"
#include "VulkanResourceTracker.h"
#include "VulkanCommandList.h"

#include "VulkanRHIResource.h"
#include "log/LogSystem.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"

#include <cassert>
#include <stdexcept>

namespace Moer::Render {

namespace {
bool IsWriteState(VkImageLayout _layout, VkAccessFlags2 _access) {
    bool read_layout = _layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                       _layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (read_layout)
        return false;

    bool read_access  = _access & VK_ACCESS_2_SHADER_READ_BIT ||
                        _access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT ||
                        _access & VK_ACCESS_2_TRANSFER_READ_BIT;
    bool write_access = _access & VK_ACCESS_2_SHADER_WRITE_BIT ||
                        _access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT ||
                        _access & VK_ACCESS_2_MEMORY_WRITE_BIT || _access & VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (read_access && !write_access)
        return false;
    return true;
}

bool IsWriteState(VkAccessFlags2 _access) {
    return _access & VK_ACCESS_2_SHADER_WRITE_BIT || _access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT ||
           _access & VK_ACCESS_2_MEMORY_WRITE_BIT || _access & VK_ACCESS_2_TRANSFER_WRITE_BIT;
}

VkImageLayout GetInitialTextureLayout(VulkanTexture* _texture, EQueueType _queue) {
    if (!_texture->b_has_preferred_state) {
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (_texture->b_present) {
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return _texture->GetQueuePreferredLayout(_queue);
}

} // namespace

VulkanResourceTracker::VulkanResourceTracker(EQueueType _queue) : queue_type(_queue) {
    assert(
        (_queue == EQueueType::Graphics || _queue == EQueueType::Compute || _queue == EQueueType::Copy) &&
        "Invalid queue type for Vulkan resource tracker"
    );
}

void VulkanResourceTracker::SetWriteState(const TextureSubresourceKeyT<VulkanTexture>& _key, bool _is_write) {
    if (_is_write) {
        write_state_textures.insert(_key);
    } else {
        write_state_textures.erase(_key);
    }
}

void VulkanResourceTracker::SetWriteState(VulkanBuffer* _buffer, bool _is_write) {
    if (_is_write) {
        write_state_buffers.insert(_buffer);
    } else {
        write_state_buffers.erase(_buffer);
    }
}

void VulkanResourceTracker::RecordState(
    VulkanBuffer*         _buffer,
    VkAccessFlags2        _access,
    VkPipelineStageFlags2 _stage,
    uint32_t              _src_queue_family,
    uint32_t              _dst_queue_family
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
        throw std::logic_error("mixed explicit partial buffer barrier and inferred buffer state");
    }
    SetWriteState(_buffer, IsWriteState(_access));
    pending_buffers.insert(_buffer);

    if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
        auto& state = it->second;
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

void VulkanResourceTracker::AdoptExternalState(
    VulkanBuffer*         _buffer,
    VkAccessFlags2        _access,
    VkPipelineStageFlags2 _stage
) {
    if (auto it = buffer_states.find(_buffer); it != buffer_states.end()) {
        auto& state      = it->second;
        state.src_access = _access;
        state.src_stage  = _stage;
        state.dst_access = VK_ACCESS_2_NONE;
        state.dst_stage  = VK_PIPELINE_STAGE_2_NONE;
    }
}

void VulkanResourceTracker::AdoptExternalState(
    VulkanTexture*        _texture,
    VkAccessFlags2        _access,
    VkImageLayout         _layout,
    VkPipelineStageFlags2 _stage
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

void VulkanResourceTracker::RecordState(
    VulkanBuffer*                  _buffer,
    const VulkanBufferAccessState& _state,
    uint32_t                       _src_queue_family,
    uint32_t                       _dst_queue_family
) {
    RecordState(_buffer, _state.access, _state.stages, _src_queue_family, _dst_queue_family);
}

void VulkanResourceTracker::RecordInternalBufferRange(
    const BufferView&     _view,
    VkAccessFlags2        _access,
    VkPipelineStageFlags2 _stage,
    VkAccessFlags2        _src_access,
    VkPipelineStageFlags2 _src_stage
) {
    VulkanBuffer* buffer = ResourceCast(_view.buffer);

    auto [range_iter, inserted] = pending_internal_buffer_ranges.try_emplace(
        buffer, _view.GetByteOffset(), _view.GetByteSize() + _view.GetByteOffset()
    );
    if (inserted) {
        auto& state = internal_buffer_states[buffer];
        state.dst_access |= _access;
        state.dst_stage |= _stage;
        state.src_access = _src_access;
        state.src_stage  = _src_stage;
        pending_buffers.insert(buffer);
    } else {
        auto& range = range_iter->second;
        range.begin = Min(range.begin, _view.GetByteOffset());
        range.end   = Max(range.end, _view.GetByteSize() + _view.GetByteOffset());
    }
}

void VulkanResourceTracker::RecordState(
    VulkanTexture*                  _texture,
    const VulkanTextureAccessState& _state,
    uint32_t                        _src_queue_family,
    uint32_t                        _dst_queue_family
) {
    RecordState(
        _texture,
        _state.access,
        _state.layout,
        _state.stages,
        0,
        _texture->GetNumMips(),
        0,
        _texture->GetNumArray(),
        _src_queue_family,
        _dst_queue_family
    );
}

void VulkanResourceTracker::QueueTransferReleaseResource(
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
            state.src_layout = GetInitialTextureLayout(_texture, queue_type);
            texture_states.emplace(key, state);
        }
    }
    exported_textures.insert(_texture);
}

void VulkanResourceTracker::QueueTransferReleaseResource(
    VulkanBuffer* _buffer,
    uint          _src_queue,
    uint          _dst_queue
) {

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

void VulkanResourceTracker::QueueTransferAcquireResource(
    VulkanBuffer*         _buffer,
    uint                  _src_queue,
    uint                  _dst_queue,
    VkAccessFlags2        _dst_access,
    VkPipelineStageFlags2 _dst_stage
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

void VulkanResourceTracker::QueueTransferAcquireResource(
    VulkanTexture*        _texture,
    uint                  _src_queue,
    uint                  _dst_queue,
    VkImageLayout         _src_layout,
    VkImageLayout         _dst_layout,
    VkAccessFlags2        _dst_access,
    VkPipelineStageFlags2 _dst_stage
) {
    uint8 num_mips   = _texture->GetNumMips();
    uint8 num_arrays = _texture->GetNumArray();

    // 下列判断主要针对AMD GPU
    // - AMD GPU没有TransferQueue，RHI会把TransferQueue的命令当做GraphicsQueue来执行
    // - 因此，此处会受到RHI发出的GraphicsQueue->GraphicsQueue的指令
    // - 我们要对这种情况做出处理
    if (_src_queue == _dst_queue) {
        for (uint8 mip = 0; mip < num_mips; ++mip) {
            auto key            = MakeTextureStateKey(_texture, mip, 1, 0, num_arrays);
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

TextureSubresourceKeyT<VulkanTexture> VulkanResourceTracker::MakeTextureStateKey(
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

void VulkanResourceTracker::MarkBlasWritten(uint64 _geometry_handle) {
    written_blas_handles.insert(_geometry_handle);
}

void VulkanResourceTracker::RecordState(
    VulkanTexture*        _texture,
    VkAccessFlags2        _access,
    VkImageLayout         _layout,
    VkPipelineStageFlags2 _stage,
    uint8_t               _mip_level,
    uint8_t               _mip_count,
    uint8_t               _array_layer,
    uint8_t               _array_count,
    uint32_t              _src_queue_family,
    uint32_t              _dst_queue_family
) {
    if (explicit_partial_aspect_textures.contains(_texture)) {
        LOG_CRITICAL(
            "A partial-aspect explicit texture barrier cannot be followed by "
            "backend-inferred state tracking in the same submit"
        );
        throw std::logic_error("mixed explicit partial-aspect barrier and inferred texture state");
    }
    uint8 resolved_mip_count =
        _mip_count == kRemainingSubresource ? uint8(_texture->GetNumMips() - _mip_level) : _mip_count;
    uint8 resolved_array_count =
        _array_count == kRemainingSubresource ? uint8(_texture->GetNumArray() - _array_layer) : _array_count;
    const TextureAspectSubresourceRangeT<VulkanTexture> requested_range{
        .subresource =
            MakeTextureStateKey(_texture, _mip_level, resolved_mip_count, _array_layer, resolved_array_count),
        .aspects = VulkanEnumTranslator::METoVKImageAspectFlags(_texture->GetAspectFlags()),
    };
    for (const auto& released_range : explicit_released_texture_ranges) {
        if (TextureAspectSubresourceRangesOverlap(released_range, requested_range)) {
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
    if (resolved_mip_count > 1 || (has_explicit_texture_state && resolved_array_count > 1)) {
        for (uint8_t i = 0; i < resolved_mip_count; ++i) {
            const uint8 layer_count = has_explicit_texture_state ? resolved_array_count : 1;
            for (uint8 layer = 0; layer < layer_count; ++layer) {
                RecordState(
                    _texture,
                    _access,
                    _layout,
                    _stage,
                    _mip_level + i,
                    1,
                    has_explicit_texture_state ? static_cast<uint8>(_array_layer + layer) : _array_layer,
                    has_explicit_texture_state ? 1 : resolved_array_count,
                    _src_queue_family,
                    _dst_queue_family
                );
            }
        }
        return;
    }

    auto key = MakeTextureStateKey(_texture, _mip_level, 1, _array_layer, _array_count);
    for (const auto& explicit_range : explicit_texture_ranges) {
        if (!TextureSubresourceRangesOverlap(explicit_range, key) || explicit_range == key) {
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
        throw std::logic_error("mixed explicit and inferred texture ranges have incompatible shapes");
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
        state.src_layout = GetInitialTextureLayout(_texture, queue_type);
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
    SetWriteState(key, is_write);
}

void VulkanResourceTracker::EmitExplicitBarrier(
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

    buffer_barriers.emplace_back(
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

    if (_offset == 0 && _byte_size == _buffer->GetByteSize()) {
        buffer_states[_buffer] = BufferState{
            .src_access       = static_cast<VkAccessFlags2>(_dst_access),
            .src_stage        = static_cast<VkPipelineStageFlags2>(_dst_stage),
            .dst_access       = VK_ACCESS_2_NONE,
            .dst_stage        = VK_PIPELINE_STAGE_2_NONE,
            .src_queue_family = VK_QUEUE_FAMILY_IGNORED,
            .dst_queue_family = VK_QUEUE_FAMILY_IGNORED,
        };
        SetWriteState(_buffer, IsWriteState(_dst_access));
    } else {
        explicit_partial_buffers.insert(_buffer);
    }
}

void VulkanResourceTracker::EmitExplicitBarrier(
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
            MakeTextureStateKey(_texture, _mip_level, resolved_mip_count, _array_layer, resolved_array_count),
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

    texture_barriers.emplace_back(
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
                _texture, static_cast<uint8>(_mip_level + mip), 1, static_cast<uint8>(_array_layer + layer), 1
            );
            texture_states[key] = TextureState{
                .src_access       = static_cast<VkAccessFlags2>(_dst_access),
                .src_layout       = _dst_layout,
                .src_stage        = static_cast<VkPipelineStageFlags2>(_dst_stage),
                .dst_access       = VK_ACCESS_2_NONE,
                .dst_layout       = VK_IMAGE_LAYOUT_UNDEFINED,
                .dst_stage        = VK_PIPELINE_STAGE_2_NONE,
                .src_queue_family = VK_QUEUE_FAMILY_IGNORED,
                .dst_queue_family = VK_QUEUE_FAMILY_IGNORED,
            };
            explicit_texture_ranges.insert(key);
            SetWriteState(key, IsWriteState(_dst_layout, _dst_access));
        }
    }
    if (IsWriteState(_dst_layout, _dst_access)) {
        // Keep the declared aggregate view as a conservative bindless lookup
        // key while the authoritative state table remains mip/layer atomic.
        // HandleBindless performs overlap-based current-write exclusion.
        write_state_textures.insert(
            MakeTextureStateKey(_texture, _mip_level, resolved_mip_count, _array_layer, resolved_array_count)
        );
    }
}

void VulkanResourceTracker::ResolveBarriers() {

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
        if (auto it = pending_internal_buffer_ranges.find(buffer);
            it != pending_internal_buffer_ranges.end()) {
            auto state = internal_buffer_states[buffer];
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
            barrier.offset                  = it->second.begin;
            barrier.size                    = it->second.end - it->second.begin;
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
    pending_internal_buffer_ranges.clear();
}

void VulkanResolvedBarrierBatch::Record(VulkanCommandList& _cmd_list) const {
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

VulkanResolvedBarrierBatch VulkanResourceTracker::TakeResolvedBarriers() {
    VulkanResolvedBarrierBatch batch{
        .buffer_barriers  = std::move(buffer_barriers),
        .texture_barriers = std::move(texture_barriers),
        .memory_barriers  = std::move(memory_barriers),
    };
    buffer_barriers.clear();
    texture_barriers.clear();
    memory_barriers.clear();
    return batch;
}

void VulkanResourceTracker::DispatchBarriers(VulkanCommandList& _cmdlist) {
    TakeResolvedBarriers().Record(_cmdlist);
}

void VulkanResourceTracker::RestorePreferredStates() {
    InitializeUntrackedTextureLayouts();
    RestoreTrackedTextureLayouts();
    AppendBufferRestoreBarrier();
}

void VulkanResourceTracker::InitializeUntrackedTextureLayouts() {
    Set<VulkanTexture*> first_restore_textures;
    for (const auto& [key, state] : texture_states) {
        (void)state;
        VulkanTexture* texture = key.texture;
        if (exported_textures.find(texture) != exported_textures.end()) {
            continue;
        }
        if (texture->b_present) {
            // Presentation images keep their externally managed layout
            // contract and must not receive UNDEFINED complement barriers.
            texture->b_has_preferred_state = true;
            continue;
        }
        if (!texture->b_has_preferred_state) {
            first_restore_textures.insert(texture);
        }
    }

    // b_has_preferred_state is a whole-image promise used by the next
    // submission. When the first submit touches only one mip/layer, restore
    // every untouched cell from UNDEFINED as well before publishing that
    // promise. The ranges are complementary to texture_states, so they never
    // overlap the ordinary current-layout -> preferred-layout barriers below.
    for (VulkanTexture* texture : first_restore_textures) {
        const VkImageLayout      layout = texture->GetQueuePreferredLayout(queue_type);
        const VkImageAspectFlags aspects =
            VulkanEnumTranslator::METoVKImageAspectFlags(texture->GetAspectFlags());
        const uint32 mip_count   = texture->GetNumMips();
        const uint32 array_count = texture->GetNumArray();
        const auto   is_tracked  = [&](uint32 mip, uint32 layer) {
            for (const auto& [key, state] : texture_states) {
                (void)state;
                if (key.texture != texture) {
                    continue;
                }
                const uint32 mip_end =
                    static_cast<uint32>(key.mip_level) + static_cast<uint32>(key.mip_count);
                const uint32 layer_end =
                    static_cast<uint32>(key.array_layer) + static_cast<uint32>(key.array_count);
                if (mip >= key.mip_level && mip < mip_end && layer >= key.array_layer && layer < layer_end) {
                    return true;
                }
            }
            return false;
        };

        for (uint32 mip = 0; mip < mip_count; ++mip) {
            uint32 layer = 0;
            while (layer < array_count) {
                if (is_tracked(mip, layer)) {
                    ++layer;
                    continue;
                }
                const uint32 first_layer = layer;
                do {
                    ++layer;
                } while (layer < array_count && !is_tracked(mip, layer));

                texture_barriers.emplace_back(
                    VkImageMemoryBarrier2{
                        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                        .pNext               = nullptr,
                        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
                        .srcAccessMask       = VK_ACCESS_2_NONE,
                        .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        .dstAccessMask       = VK_ACCESS_2_NONE,
                        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout           = layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image               = texture->GetHandle(),
                        .subresourceRange    = VkImageSubresourceRange{
                            .aspectMask     = aspects,
                            .baseMipLevel   = mip,
                            .levelCount     = 1,
                            .baseArrayLayer = first_layer,
                            .layerCount     = layer - first_layer,
                        },
                    }
                );
            }
        }
        texture->b_has_preferred_state = true;
    }
}

void VulkanResourceTracker::RestoreTrackedTextureLayouts() {
    for (auto& [key, state] : texture_states) {
        VulkanTexture* texture = key.texture;
        if (exported_textures.find(texture) != exported_textures.end())
            continue;
        VkImageLayout layout = texture->GetQueuePreferredLayout(queue_type);
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
}

void VulkanResourceTracker::AppendBufferRestoreBarrier() {
    VkAccessFlags2        src_buffer_access = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 src_buffer_stages = VK_PIPELINE_STAGE_2_NONE;
    for (auto& [buffer, state] : buffer_states) {
        if (exported_buffers.find(buffer) != exported_buffers.end())
            continue;
        src_buffer_access |= state.src_access;
        src_buffer_stages |= state.src_stage;
    }

    for (auto& [buffer, state] : internal_buffer_states) {
        src_buffer_access |= state.dst_access;
        src_buffer_stages |= state.dst_stage;
    }
    if (!buffer_states.empty() || !internal_buffer_states.empty()) {
        //memory barrier
        memory_barriers.emplace_back();
        VkMemoryBarrier2& barrier = memory_barriers.back();
        barrier.sType             = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.pNext             = nullptr;
        barrier.srcAccessMask     = src_buffer_access;
        barrier.dstAccessMask     = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.srcStageMask      = src_buffer_stages;
        barrier.dstStageMask      = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
}

void VulkanResourceTracker::Reset() {
    buffer_barriers.clear();
    texture_barriers.clear();
    memory_barriers.clear();
    buffer_states.clear();
    texture_states.clear();
    write_state_textures.clear();
    write_state_buffers.clear();
    written_blas_handles.clear();
    internal_buffer_states.clear();
    pending_internal_buffer_ranges.clear();
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
