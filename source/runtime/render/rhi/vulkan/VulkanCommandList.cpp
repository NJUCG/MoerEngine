//
// Created by 74535 on 2023/10/17.
//

#include "VulkanResourceTracker.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include "VulkanCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanPlatform.h"
#include "VulkanRHIResource.h"
#include <string_view>

#include "VulkanAllocator.h"
#include "Core.h"
#include "shader/ShaderPipeline.h"
#include "vulkan/vulkan_core.h"

#include <cstdint>
#include <stdint.h>
#include <string>
#include <variant>
#include <vector>
namespace Moer::Render {

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

    auto transfer_state_any_or = [&](ETextureStateFlags    _tmp_state,
                                     VkAccessFlags2        _access,
                                     VkPipelineStageFlags2 _stage,
                                     VkImageLayout         _layout) {
        if (EnumHasAnyFlag(_state, _tmp_state)) {
            src_access_flags |= _access;
            src_stage = _stage;
            layout    = _layout;
        }
    };
    src_stage        = VK_PIPELINE_STAGE_2_NONE;
    src_access_flags = VK_ACCESS_2_NONE;
    transfer_state_any_or(
        TS_UNORDERED_READ,
        VK_ACCESS_2_SHADER_READ_BIT,
        GetPipelineStageFromPassType(_pass),
        VK_IMAGE_LAYOUT_GENERAL
    );
    transfer_state_any_or(
        TS_UNORDERED_WRITE,
        VK_ACCESS_2_SHADER_WRITE_BIT,
        GetPipelineStageFromPassType(_pass),
        VK_IMAGE_LAYOUT_GENERAL
    );
    transfer_state_any_or(
        TS_COLOR_ATTACHMENT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        uint32_t(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    );

    transfer_state_any_or(
        TS_RESOLVE_ATTACHMENT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        uint32_t(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    );
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
} // namespace Moer::Render
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
//RHICreateGfxPso<Texture, Texture, Buffer>? no RHICreateGfxPso<TPipelineLayout>(auto&& _init_info);
//     VulkanShaderResourceState state(_flags);
//     switch (state.resource_type) {
//         case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SRV:
//             return true;
//         default: return false;
//     }
// }

VulkanCmdList::VulkanCmdList(VulkanCmdAllocator* _alloc, VulkanDevice& _device) :
    allocator(_alloc),
    device(_device) {
    VkCommandBufferAllocateInfo command_buffer_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext              = nullptr,
        .commandPool        = allocator->GetHandle(),
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VK_CHECK_RESULT(vkAllocateCommandBuffers(device.GetDevice(), &command_buffer_info, &command_buffer));
    _device.SetResourceName(
        (uint64)command_buffer,
        VK_OBJECT_TYPE_COMMAND_BUFFER,
        std::format("CommandBuffer_{}", allocator->GetQueueName())
    );
}

VulkanCmdList::~VulkanCmdList() {
    vkFreeCommandBuffers(device.GetDevice(), allocator->GetHandle(), 1, &command_buffer);
}
void VulkanCmdList::Begin() {
    VkCommandBufferBeginInfo begin_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .pInheritanceInfo = nullptr
    };
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
    uint64        _dst_offset
) {

    VkBufferCopy copy_region = {.srcOffset = _src_offset, .dstOffset = _dst_offset, .size = _size};
    vkCmdCopyBuffer(command_buffer, _src->GetHandle(), _dst->GetHandle(), 1, &copy_region);
}
void VulkanCmdList::CopyBufferToTexture(
    VulkanBuffer*  _src,
    VulkanTexture* _dst,
    uint64         _size,
    uint64         _src_offset,
    uint3          _dst_offset,
    uint3          _extent,
    uint32         _mip_level,
    uint32         _array_layer
) {
    VkImageAspectFlags aspect =
        _src->GetResourceType() == RRT_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkBufferImageCopy copy_region = {
        .bufferOffset      = _src_offset,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {.aspectMask = aspect, .mipLevel = _mip_level, .baseArrayLayer = _array_layer, .layerCount = 1},
        .imageOffset =
            {static_cast<int32_t>(_dst_offset.x),
             static_cast<int32_t>(_dst_offset.y),
             static_cast<int32_t>(_dst_offset.z)},
        .imageExtent = {
            static_cast<uint32_t>(_extent.x),
            static_cast<uint32_t>(_extent.y),
            static_cast<uint32_t>(_extent.z)
        }
    };

    // _dst->GetExtent() 是整个texture的尺寸
    // _extent 是这次mipmap的尺寸
    const uint64 expected_size_exact    = GetSizeFromImageFormat(_dst->GetFormat(), _extent);
    const uint64 legacy_texel_count_size = uint64(_extent.x) * uint64(_extent.y) * uint64(_extent.z);

    if (_size != expected_size_exact) {
        const uint3 texture_extent = _dst->GetExtent();
        LOG_ERROR(
            "[CopyBufferToTexture] Size mismatch. Texture='{}' Handle={:#x} Format={} Mip={} ArrayLayer={} "
            "TextureExtent=({}, {}, {}) CopyOffset=({}, {}, {}) CopyExtent=({}, {}, {}) "
            "IncomingSize={} ExactExpected={} LegacyTexelCount={} DstMipByteSize={} "
            "DeltaToExact={} DeltaToLegacy={} LegacyMatch={}",
            _dst->GetName(),
            (uint64)_dst->GetHandle(),
            uint32(_dst->GetFormat()),
            _mip_level,
            _array_layer,
            texture_extent.x,
            texture_extent.y,
            texture_extent.z,
            _dst_offset.x,
            _dst_offset.y,
            _dst_offset.z,
            _extent.x,
            _extent.y,
            _extent.z,
            _size,
            expected_size_exact,
            legacy_texel_count_size,
            _dst->GetMipByteSize(_mip_level),
            int64_t(_size) - int64_t(expected_size_exact),
            int64_t(_size) - int64_t(legacy_texel_count_size),
            _size == legacy_texel_count_size ? "true" : "false"
        );
    }

    assert(_size == expected_size_exact && "Copy size does not match texture extent");

    vkCmdCopyBufferToImage(
        command_buffer,
        _src->GetHandle(),
        _dst->GetHandle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy_region
    );
}

void VulkanCmdList::CopyTextureToBuffer(
    VulkanTexture* _src,
    VulkanBuffer*  _dst,
    uint64         _size,
    uint3          _src_offset,
    uint64         _dst_offset,
    uint3          _extent,
    uint32         _mip_level
) {
    VkImageAspectFlags aspect =
        _src->GetResourceType() == RRT_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkBufferImageCopy copy_region = {
        .bufferOffset      = _dst_offset,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {.aspectMask = aspect, .mipLevel = _mip_level, .baseArrayLayer = 0, .layerCount = 1},
        .imageOffset =
            {static_cast<int32_t>(_src_offset.x),
             static_cast<int32_t>(_src_offset.y),
             static_cast<int32_t>(_src_offset.z)},
        .imageExtent = {
            static_cast<uint32_t>(_extent.x),
            static_cast<uint32_t>(_extent.y),
            static_cast<uint32_t>(_extent.z)
        }
    };

    vkCmdCopyImageToBuffer(
        command_buffer,
        _src->GetHandle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        _dst->GetHandle(),
        1,
        &copy_region
    );
}

void VulkanCmdList::CopyData(const BufferView& _dst, const void* _data, uint64 _size) {
    auto*        buffer    = static_cast<VulkanBuffer*>(_dst.buffer);
    VmaAllocator allocator = device.GetVmaAllocator();

    // void* p_data;
    // VK_CHECK_RESULT(vmaMapMemory(allocator, buffer->GetAllocation(), &p_data));
    // std::memcpy((byte*)p_data + _dst.GetByteOffset(), _data, _size);
    // vmaUnmapMemory(allocator, buffer->GetAllocation());
    // vmaFlushAllocation(allocator, buffer->GetAllocation(), _dst.GetByteOffset(), _size);
    VK_CHECK_RESULT(
        vmaCopyMemoryToAllocation(allocator, _data, buffer->GetAllocation(), _dst.GetByteOffset(), _size)
    );
}

void VulkanCmdList::CopyData(void* _dst, const BufferView& _src, uint64 _size) {
    auto*        buffer    = static_cast<VulkanBuffer*>(_src.buffer);
    VmaAllocator allocator = device.GetVmaAllocator();

    // void* p_data;
    // VK_CHECK_RESULT(vmaMapMemory(allocator, buffer->GetAllocation(), &p_data));
    // std::memcpy((byte*)_dst, (byte*)p_data + _src.GetByteOffset(), _size);
    // vmaUnmapMemory(allocator, buffer->GetAllocation());
    // vmaFlushAllocation(allocator, buffer->GetAllocation(), _src.GetByteOffset(), _size);
    VK_CHECK_RESULT(
        vmaCopyAllocationToMemory(allocator, buffer->GetAllocation(), _src.GetByteOffset(), _dst, _size)
    );
}

void* VulkanCmdList::MapBuffer(const BufferView& _buffer) {
    auto*        buffer    = static_cast<VulkanBuffer*>(_buffer.buffer);
    VmaAllocator allocator = device.GetVmaAllocator();

    void* p_data;
    VK_CHECK_RESULT(vmaMapMemory(allocator, buffer->GetAllocation(), &p_data));
    return (byte*)p_data + _buffer.GetByteOffset();
}

void VulkanCmdList::UnmapBuffer(const BufferView& _buffer) {
    auto*        buffer    = static_cast<VulkanBuffer*>(_buffer.buffer);
    VmaAllocator allocator = device.GetVmaAllocator();

    vmaUnmapMemory(allocator, buffer->GetAllocation());
    vmaFlushAllocation(allocator, buffer->GetAllocation(), _buffer.GetByteOffset(), _buffer.GetByteSize());
}

void VulkanCmdList::DrawIndexedInstanced(
    uint32_t _index_cnt,
    uint32_t _instance_cnt,
    uint32_t _first_index,
    uint32_t _vertex_offset,
    uint32_t _first_instance
) {
    vkCmdDrawIndexed(
        command_buffer, _index_cnt, _instance_cnt, _first_index, _vertex_offset, _first_instance
    );
}

void VulkanCmdList::DrawInstanced(
    uint32_t _vertex_cnt,
    uint32_t _instance_cnt,
    uint32_t _first_vertex,
    uint32_t _first_instance
) {
    vkCmdDraw(command_buffer, _vertex_cnt, _instance_cnt, _first_vertex, _first_instance);
}

void VulkanCmdList::DrawIndexedIndirectCnt(
    VulkanBuffer* _commands,
    uint64        _commands_offset,
    VulkanBuffer* _count,
    uint64        _count_offset,
    uint32_t      _max_cnt,
    uint32_t      _stride
) {
    vkCmdDrawIndexedIndirectCount(
        command_buffer,
        _commands->GetHandle(),
        _commands_offset,
        _count->GetHandle(),
        _count_offset,
        _max_cnt,
        _stride
    );
}

void VulkanCmdList::DrawIndirectCnt(
    VulkanBuffer* _commands,
    uint64        _commands_offset,
    VulkanBuffer* _count,
    uint64        _count_offset,
    uint32_t      _max_cnt,
    uint32_t      _stride
) {
    vkCmdDrawIndirectCount(
        command_buffer,
        _commands->GetHandle(),
        _commands_offset,
        _count->GetHandle(),
        _count_offset,
        _max_cnt,
        _stride
    );
}

void VulkanCmdList::DrawIndexedIndirect(
    VulkanBuffer* _buffer,
    uint64        _offset,
    uint32_t      _draw_cnt,
    uint32_t      _stride
) {
    vkCmdDrawIndexedIndirect(command_buffer, _buffer->GetHandle(), _offset, _draw_cnt, _stride);
}

void VulkanCmdList::DrawIndirect(
    VulkanBuffer* _buffer,
    uint64        _offset,
    uint32_t      _draw_cnt,
    uint32_t      _stride
) {
    vkCmdDrawIndirect(command_buffer, _buffer->GetHandle(), _offset, _draw_cnt, _stride);
}

void VulkanCmdList::DispatchMesh(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
    vkCmdDrawMeshTasksEXT(command_buffer, _group_count_x, _group_count_y, _group_count_z);
}

void VulkanCmdList::DispatchMeshIndirect(
    VulkanBuffer* _buffer,
    uint64        _offset,
    uint32_t      _draw_cnt,
    uint32_t      _stride
) {
    vkCmdDrawMeshTasksIndirectEXT(command_buffer, _buffer->GetHandle(), _offset, _draw_cnt, _stride);
}

void VulkanCmdList::DispatchMeshIndirectCount(
    VulkanBuffer* _commands,
    uint64        _commands_offset,
    VulkanBuffer* _count,
    uint64        _count_offset,
    uint32_t      _max_cnt,
    uint32_t      _stride
) {
    vkCmdDrawMeshTasksIndirectCountEXT(
        command_buffer,
        _commands->GetHandle(),
        _commands_offset,
        _count->GetHandle(),
        _count_offset,
        _max_cnt,
        _stride
    );
}

void VulkanCmdList::CopyTexture(
    VulkanTexture* _src,
    VulkanTexture* _dst,
    uint3          _extent,
    uint3          _src_offset,
    uint3          _dst_offset,
    uint32         _src_mip_level,
    uint32         _dst_mip_level
) {
    VkImageCopy copy_region = {
        .srcSubresource =
            {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
             .mipLevel       = _src_mip_level,
             .baseArrayLayer = 0,
             .layerCount     = 1},
        .srcOffset =
            {static_cast<int32_t>(_src_offset.x),
             static_cast<int32_t>(_src_offset.y),
             static_cast<int32_t>(_src_offset.z)},
        .dstSubresource =
            {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
             .mipLevel       = _dst_mip_level,
             .baseArrayLayer = 0,
             .layerCount     = 1},
        .dstOffset =
            {static_cast<int32_t>(_dst_offset.x),
             static_cast<int32_t>(_dst_offset.y),
             static_cast<int32_t>(_dst_offset.z)},
        .extent = {
            static_cast<uint32_t>(_extent.x),
            static_cast<uint32_t>(_extent.y),
            static_cast<uint32_t>(_extent.z)
        }
    };

    vkCmdCopyImage(
        command_buffer,
        _src->GetHandle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        _dst->GetHandle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy_region
    );
}

void VulkanCmdList::BeginRendering(VkRenderingInfo&& _info) {
    vkCmdBeginRendering(command_buffer, &_info);
}

void VulkanCmdList::EndRendering() {
    vkCmdEndRendering(command_buffer);
}

void VulkanCmdList::SetVertexBuffers(
    uint                _first_binding,
    uint                _binding_cnt,
    std::span<VkBuffer> _buffers,
    std::span<uint64>   _offsets
) {
    vkCmdBindVertexBuffers(command_buffer, _first_binding, _binding_cnt, _buffers.data(), _offsets.data());
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

void VulkanCmdList::ClearBufferUInt(VulkanBuffer* _buffer, uint64 _offset, uint64 _size, uint32 _data) {
    vkCmdFillBuffer(command_buffer, _buffer->GetHandle(), _offset, _size, _data);
}

void VulkanCmdList::ClearTexture(
    VulkanTexture*                 _texture,
    const VkClearColorValue&       _color,
    const VkImageSubresourceRange& _range
) {
    vkCmdClearColorImage(
        command_buffer, _texture->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &_color, 1, &_range
    );
}

void VulkanCmdList::ResetQueryPool(VkNativeQueryPool& _query_pool, uint32 _first_query, uint32 _query_cnt) {
    vkCmdResetQueryPool(command_buffer, _query_pool.GetHandle(), _first_query, _query_cnt);
}

void VulkanCmdList::BeginQuery(VkNativeQueryPool& _query_pool, uint32 _query, VkQueryControlFlags _flags) {
    vkCmdBeginQuery(command_buffer, _query_pool.GetHandle(), _query, _flags);
}

void VulkanCmdList::EndQuery(VkNativeQueryPool& _query_pool, uint32 _query) {
    vkCmdEndQuery(command_buffer, _query_pool.GetHandle(), _query);
}

void VulkanCmdList::WriteTimeStamp(
    VkNativeQueryPool&       _query_pool,
    uint32                   _query_idx,
    VkPipelineStageFlagBits2 _stage
) {
    vkCmdWriteTimestamp2(command_buffer, _stage, _query_pool.GetHandle(), _query_idx);
}

void VulkanCmdList::Dispatch(uint _group_count_x, uint _group_count_y, uint _group_count_z) {
    vkCmdDispatch(command_buffer, _group_count_x, _group_count_y, _group_count_z);
}

void VulkanCmdList::DispatchIndirect(VulkanBuffer* _buffer, uint64 _offset) {
    vkCmdDispatchIndirect(command_buffer, _buffer->GetHandle(), _offset);
}

void VulkanCmdList::UploadDescriptors(PipelineHandle& _pso_handle) {}

void VulkanCmdList::UploadPushConstants(PipelineHandle& _pso_handle, std::span<const uint> _data) {
    auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(_pso_handle.handle);
    // auto  binding_info               = _pso_handle.binding_infos[_pso_handle.constant_idx];
    // auto [offset, size, stage_flags] = DecodeReflectPushConstant(binding_info);

    // vkCmdPushConstants(command_buffer, vk_pso->GetPipelineLayout(), stage_flags, offset, size, _data.data());
}

VkImageLayout GetSamplerImageLayout(const TextureView& _view) {
    return uint(_view.GetTexture()->GetAspectFlags() & ETextureAspectFlags::DEPTH_SLICE) ?
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanCmdList::BindDescriptors(PipelineHandle& _pso_handle, const ArrayArguments& _args) {
    auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(_pso_handle.handle);

    assert(vk_pso && vk_pso->bind_template != nullptr && "Pipeline state has no bind template!");
    VulkanPipelineParamBinder& bind_template        = *vk_pso->bind_template;
    VulkanDescriptorHeap&      descriptor_heap      = device.GetGlobalDescriptorHeap();
    auto&                      set_binders          = bind_template.set_binders;
    Array<byte>                push_data;
    push_data.resize(bind_template.push_data_size);
    if (!push_data.empty()) {
        memset(push_data.data(), 0, push_data.size());
    }

    auto write_push_offset = [&](uint32_t _push_offset, uint64 _value) {
        assert(_push_offset + sizeof(uint32_t) <= push_data.size());
        const uint32_t encoded_value = uint32_t(_value);
        memcpy(push_data.data() + _push_offset, &encoded_value, sizeof(encoded_value));
    };

    for (auto& [set, binder] : set_binders) {
        std::visit(
            Overload{
                [&](const VulkanBindlessSetArray& _binder) {
                    const auto* bindless_arg = std::get_if<BindlessArrayRef>(&_args[_binder.param_idx]);
                    if (bindless_arg == nullptr) {
                        LOG_ERROR(
                            "Bindless binder expected BindlessArrayRef at arg {}, but variant index was {}.",
                            _binder.param_idx,
                            _args[_binder.param_idx].index()
                        );
                        assert(false && "bindless binder arg type mismatch");
                    }
                    BindlessArrayRef     array = *bindless_arg;
                    VulkanBindlessArray* bindless_array = static_cast<VulkanBindlessArray*>(array.Get());
                    const auto cache = descriptor_heap.CacheBindlessArray(*bindless_array);
                    write_push_offset(_binder.push_data_offset, cache.array_offset);
                },
                [&](const VulkanDescriptorSetBinderImpl& _binder) {
                    uint64 set_base_offset = 0;
                    if (_binder.size > 0) {
                        set_base_offset = descriptor_heap.AllocateOnlineDescriptorRange(_binder.size);
                        write_push_offset(_binder.resource_push_data_offset, set_base_offset);
                    }

                    for (uint i = 0; i < _binder.writers.size(); ++i) {
                        auto& writer = _binder.writers[i];
                        if (writer.descriptorCount < 1)
                            continue;
                        const VulkanDescriptorInfo& set_info = _binder.bind_infos[i];
                        if (!(_pso_handle.valid_bits & (1 << set_info.param_idx)))
                            continue;

                        VkFormat format =
                            g_platform_pixel_formats[VulkanShaderResourceState(
                                                         _pso_handle.binding_infos[set_info.param_idx]
                                                             .state_flags
                                                     )
                                                         .format]
                                .format;
                        const auto& binding_info = _binder.binding_infos[i];
                        switch (writer.descriptorType) {
                            case VK_DESCRIPTOR_TYPE_SAMPLER: {
                                assert(writer.descriptorCount == 1 && "Sampler arrays are not supported on descriptor heap path");
                                uint64 src_handle = descriptor_heap.GetSamplerDescIdx(
                                    std::get<Sampler>(_args[set_info.param_idx])
                                );
                                write_push_offset(
                                    binding_info.push_data_offset,
                                    descriptor_heap.GetBindlessSamplerDescriptorOffset(
                                        uint32(src_handle / descriptor_heap.GetOfflineSamplerDescriptorStride())
                                    )
                                );
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {

                                if (writer.descriptorCount > 1) {
                                    std::span<TextureView> textures =
                                        std::get<std::span<TextureView>>(_args[set_info.param_idx]);
                                    uint desc_size = std::min(writer.descriptorCount, uint(textures.size()));
                                    for (uint j = 0; j < desc_size; ++j) {
                                        VkImageLayout layout = GetSamplerImageLayout(textures[j]);
                                        uint64        src_handle =
                                            descriptor_heap.GetImageDescIdx(&textures[j], layout);
                                        descriptor_heap.PushImageDesc(
                                            src_handle,
                                            set_base_offset + binding_info.offset + j * binding_info.descriptor_stride
                                        );
                                    }
                                    break;
                                }
                                VkImageLayout layout =
                                    GetSamplerImageLayout(std::get<TextureView>(_args[set_info.param_idx]));

                                uint64 src_handle = descriptor_heap.GetImageDescIdx(
                                    &std::get<TextureView>(_args[set_info.param_idx]), layout
                                );
                                descriptor_heap.PushImageDesc(
                                    src_handle, set_base_offset + binding_info.offset
                                );
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
                                if (writer.descriptorCount > 1) {
                                    std::span<TextureView> textures =
                                        std::get<std::span<TextureView>>(_args[set_info.param_idx]);
                                    uint desc_size = std::min(writer.descriptorCount, uint(textures.size()));
                                    for (uint j = 0; j < desc_size; ++j) {
                                        uint64 src_handle = descriptor_heap.GetImageDescIdx(
                                            &textures[j], VK_IMAGE_LAYOUT_GENERAL
                                        );
                                        descriptor_heap.PushImageDesc(
                                            src_handle,
                                            set_base_offset + binding_info.offset + j * binding_info.descriptor_stride
                                        );
                                    }
                                    break;
                                }
                                uint64 src_handle = descriptor_heap.GetImageDescIdx(
                                    &std::get<TextureView>(_args[set_info.param_idx]), VK_IMAGE_LAYOUT_GENERAL
                                );
                                descriptor_heap.PushImageDesc(
                                    src_handle, set_base_offset + binding_info.offset
                                );
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
                                uint64 src_handle = descriptor_heap.GetBufferDescIdx(
                                    std::get<BufferView>(_args[set_info.param_idx]),
                                    writer.descriptorType,
                                    format
                                );
                                descriptor_heap.PushUniformDesc(
                                    src_handle, set_base_offset + binding_info.offset
                                );
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
                                if (writer.descriptorCount > 1) {
                                    std::span<BufferView> buffers =
                                        std::get<std::span<BufferView>>(_args[set_info.param_idx]);
                                    for (uint j = 0; j < writer.descriptorCount; ++j) {
                                        uint64 src_handle = descriptor_heap.GetBufferDescIdx(
                                            buffers[j], writer.descriptorType, format
                                        );
                                        descriptor_heap.PushStorageDesc(
                                            src_handle,
                                            set_base_offset + binding_info.offset + j * binding_info.descriptor_stride
                                        );
                                    }
                                    break;
                                }
                                uint64 src_handle = descriptor_heap.GetBufferDescIdx(
                                    std::get<BufferView>(_args[set_info.param_idx]),
                                    writer.descriptorType,
                                    format
                                );
                                descriptor_heap.PushStorageDesc(
                                    src_handle, set_base_offset + binding_info.offset
                                );
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                                VulkanAccelerationStructure* as = ResourceCast(
                                    std::get<RaytracingTlasRef>(_args[set_info.param_idx]).Get()
                                );
                                uint64 src_handle = descriptor_heap.GetAccelDescIdx(as);
                                descriptor_heap.PushAccelDesc(
                                    src_handle, set_base_offset + binding_info.offset
                                );
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_MAX_ENUM: {
                                //empty
                                break;
                            }
                            default: {
                                assert(false && "Unsupported descriptor type!");
                            }
                        }
                    }
                }
            },
            binder
        );
    }

    if (bind_template.needs_resource_heap) {
        const VkBindHeapInfoEXT resource_heap_info = descriptor_heap.GetResourceHeapBindInfo();
        device.CmdBindResourceHeap(command_buffer, &resource_heap_info);
    }

    if (bind_template.needs_sampler_heap) {
        const VkBindHeapInfoEXT sampler_heap_info = descriptor_heap.GetSamplerHeapBindInfo();
        device.CmdBindSamplerHeap(command_buffer, &sampler_heap_info);
    }

    if (bind_template.push_constants_info.size > 0) {
        const uint32_t constants_offset = bind_template.push_constants_info.offset;
        const uint32_t constants_size = bind_template.push_constants_info.size;
        const uint32_t available_size = uint32_t(_args.constants.size() * sizeof(uint));
        assert(constants_size <= available_size && "Push constant byte size exceeds recorded constant payload");
        if (push_data.size() < constants_offset + constants_size) {
            const size_t old_size = push_data.size();
            push_data.resize(constants_offset + constants_size);
            memset(push_data.data() + old_size, 0, push_data.size() - old_size);
        }
        memcpy(push_data.data() + constants_offset, _args.constants.data(), constants_size);
    }

    if (!push_data.empty()) {
        VkPushDataInfoEXT push_info{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
        push_info.offset = 0;
        push_info.data = {
            .address = push_data.data(),
            .size = push_data.size()
        };
        device.CmdPushData(command_buffer, &push_info);
    }
}

void VulkanCmdList::SetPso(const PipelineHandle& _pso_handle) {
    MOER_ASSERT(_pso_handle.IsValid(), "VulkanCmdList::SetPso received an invalid pipeline handle");
    auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(_pso_handle.handle);
    MOER_ASSERT(vk_pso != nullptr, "VulkanCmdList::SetPso resolved a null pipeline state");
    vkCmdBindPipeline(command_buffer, vk_pso->GetPipelineBindPoint(), vk_pso->GetHandle());
}

void VulkanCmdList::BuildAccelerationStructures(
    const Array<VkAccelerationStructureBuildGeometryInfoKHR>& _build_infos,
    const Array<VkAccelerationStructureBuildRangeInfoKHR*>&   _build_ranges
) {
    vkCmdBuildAccelerationStructuresKHR(
        command_buffer, _build_infos.size(), _build_infos.data(), _build_ranges.data()
    );
}

void VulkanCmdList::BeginLabel(std::string_view _label, float4 _color) {
    VkDebugUtilsLabelEXT label = {
        .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pNext      = nullptr,
        .pLabelName = _label.data(),
        .color      = {_color.x, _color.y, _color.z, _color.w}
    };
    vkCmdBeginDebugUtilsLabelEXT(command_buffer, &label);
}

void VulkanCmdList::EndLabel() {
    vkCmdEndDebugUtilsLabelEXT(command_buffer);
}

void VulkanCmdList::InsertLabel(std::string_view _label, float4 _color) {
    VkDebugUtilsLabelEXT label = {
        .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pNext      = nullptr,
        .pLabelName = _label.data(),
        .color      = {_color.x, _color.y, _color.z, _color.w}
    };
    vkCmdInsertDebugUtilsLabelEXT(command_buffer, &label);
}

} // namespace Moer::Render
