//
// Created by 74535 on 2023/10/17.
//

#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include "VulkanCommand.h"
#include "VulkanCommandQueries.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanPlatform.h"
#include "VulkanRHIResource.h"
#include <string_view>

#include "shader/ShaderPipeline.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
namespace Moer::Render {

VulkanCommandPool::VulkanCommandPool(VulkanDevice* _device, EQueueType _queue_type) :
    VulkanDeviceObject(_device),
    queue_type(_queue_type) {
    // Use the selected queue-role mapping, including vendor overrides, so the
    // command pool belongs to the same family as the actual submission queue.
    uint32_t family_index = m_device->GetQueueFamilyIndex(_queue_type);

    VkCommandPoolCreateInfo pool_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .queueFamilyIndex = family_index
    };

    VK_CHECK_RESULT(vkCreateCommandPool(_device->GetDevice(), &pool_info, nullptr, &command_pool));
}

VulkanCommandPool::~VulkanCommandPool() {
    if (command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device->GetDevice(), command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
    }
}

std::string_view VulkanCommandPool::GetQueueName() const {
    switch (queue_type) {
        case EQueueType::Graphics:
            return "Graphics";
        case EQueueType::Compute:
            return "Compute";
        case EQueueType::Copy:
            return "Copy";
        default:
            return "Unknown";
    }
}

VulkanCmdList::VulkanCmdList(
    VulkanCommandPool*   _command_pool,
    VulkanDevice&        _device,
    VkCommandBufferLevel _level
) :
    command_pool(_command_pool),
    device(_device),
    level(_level) {
    VkCommandBufferAllocateInfo command_buffer_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext              = nullptr,
        .commandPool        = command_pool->GetHandle(),
        .level              = level,
        .commandBufferCount = 1
    };
    VK_CHECK_RESULT(vkAllocateCommandBuffers(device.GetDevice(), &command_buffer_info, &command_buffer));
    _device.SetResourceName(
        (uint64)command_buffer,
        VK_OBJECT_TYPE_COMMAND_BUFFER,
        std::format(
            "{}CommandBuffer_{}",
            level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? "Primary" : "Secondary",
            command_pool->GetQueueName()
        )
    );
}

VulkanCmdList::~VulkanCmdList() {
    vkFreeCommandBuffers(device.GetDevice(), command_pool->GetHandle(), 1, &command_buffer);
}

// Recording lifecycle.
VkResult VulkanCmdList::Begin() {
    assert(level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    VkCommandBufferBeginInfo begin_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .pInheritanceInfo = nullptr
    };
    return vkBeginCommandBuffer(command_buffer, &begin_info);
}

VkResult VulkanCmdList::BeginSecondary(
    const VkCommandBufferInheritanceInfo& _inheritance,
    VkCommandBufferUsageFlags             _flags
) {
    assert(level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    VkCommandBufferBeginInfo begin_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = _flags,
        .pInheritanceInfo = &_inheritance
    };
    return vkBeginCommandBuffer(command_buffer, &begin_info);
}

VkResult VulkanCmdList::End() {
    return vkEndCommandBuffer(command_buffer);
}

void VulkanCmdList::ExecuteSecondaryCommands(
    std::span<const VkCommandBuffer> _secondary_buffers
) {
    assert(level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    if (_secondary_buffers.empty()) {
        return;
    }
    vkCmdExecuteCommands(
        command_buffer,
        static_cast<uint32_t>(_secondary_buffers.size()),
        _secondary_buffers.data()
    );
}

// Rendering scope.
void VulkanCmdList::BeginRendering(const VkRenderingInfo& _info) {
    vkCmdBeginRendering(command_buffer, &_info);
}

void VulkanCmdList::EndRendering() {
    vkCmdEndRendering(command_buffer);
}

// Pipeline and shader arguments.
void VulkanCmdList::BindPipeline(const PipelineHandle& _pso_handle) {
    auto* vk_pso = static_cast<VulkanPipelineState*>(_pso_handle.GetState());
    vkCmdBindPipeline(command_buffer, vk_pso->GetPipelineBindPoint(), vk_pso->GetHandle());
}

namespace {

VkImageLayout GetSampledImageLayout(const TextureView& _view) {
    return uint(_view.GetTexture()->GetAspectFlags() & ETextureAspectFlags::DEPTH_SLICE) ?
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

uint64 GetResourceDescriptorAllocationSize(const VulkanPipelineBindingTemplate& _binding_template) {
    uint64 allocation_size = 0;
    for (const auto& [set_index, set_binding] : _binding_template.set_bindings) {
        if (const auto* resource_bindings =
                std::get_if<VulkanResourceSetBindings>(&set_binding.resource_bindings)) {
            allocation_size += resource_bindings->allocation_size;
        }
    }
    return allocation_size;
}

template<typename View, typename WriteDescriptor>
void WriteViewDescriptors(
    const TArg&       _argument,
    uint              _descriptor_count,
    uint              _descriptor_size,
    uint64            _binding_byte_offset,
    std::string_view  _resource_kind,
    WriteDescriptor&& _write_descriptor
) {
    if (_descriptor_count == 1) {
        _write_descriptor(std::get<View>(_argument), _binding_byte_offset);
        return;
    }
    const auto& views = std::get<Array<View>>(_argument);
    if (views.empty()) {
        throw std::runtime_error(std::string(_resource_kind) + " descriptor array is empty");
    }
    if (views.size() > _descriptor_count) {
        throw std::runtime_error(
            std::string(_resource_kind) + " descriptor array exceeds its declared capacity"
        );
    }
    // Ordinary sets do not enable partial binding. Fill unused capacity with
    // the last provided view so every declared descriptor remains initialized.
    for (uint element_index = 0; element_index < _descriptor_count; ++element_index) {
        _write_descriptor(
            views[std::min<size_t>(element_index, views.size() - 1)],
            _binding_byte_offset + element_index * _descriptor_size
        );
    }
}

void WriteResourceBindingDescriptors(
    VulkanDescriptorHeap&        _heap,
    const VulkanResourceBinding& _binding,
    const TArg&                  _argument,
    VkFormat                     _buffer_format,
    uint64                       _binding_byte_offset
) {
    const VkDescriptorType descriptor_type = _binding.descriptor_type;
    switch (descriptor_type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: {
            if (_binding.descriptor_count != 1) {
                throw std::runtime_error("sampler descriptor arrays are not supported by ArrayArguments");
            }
            _heap.CopySamplerDescriptorToBuffer(
                _heap.GetSamplerDescriptorOffset(std::get<Sampler>(_argument)), _binding_byte_offset
            );
            break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
            const bool is_storage = descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            WriteViewDescriptors<TextureView>(
                _argument,
                _binding.descriptor_count,
                _heap.GetDescriptorSize(descriptor_type),
                _binding_byte_offset,
                is_storage ? "storage-image" : "sampled-image",
                [&](const TextureView& texture, uint64 destination_byte_offset) {
                    const VkImageLayout image_layout =
                        is_storage ? VK_IMAGE_LAYOUT_GENERAL : GetSampledImageLayout(texture);
                    const uint64 source_byte_offset =
                        _heap.GetOrCreateImageDescriptorOffset(&texture, image_layout, descriptor_type);
                    _heap.CopyImageDescriptorToBuffer(source_byte_offset, destination_byte_offset);
                }
            );
            break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
            WriteViewDescriptors<BufferView>(
                _argument,
                _binding.descriptor_count,
                _heap.GetDescriptorSize(descriptor_type),
                _binding_byte_offset,
                "buffer",
                [&](const BufferView& buffer, uint64 destination_byte_offset) {
                    const uint64 source_byte_offset =
                        _heap.GetOrCreateBufferDescriptorOffset(buffer, descriptor_type, _buffer_format);
                    _heap.CopyBufferDescriptorToBuffer(source_byte_offset, destination_byte_offset);
                }
            );
            break;
        }
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
            if (_binding.descriptor_count != 1) {
                throw std::runtime_error(
                    "acceleration-structure descriptor arrays are not supported by ArrayArguments"
                );
            }
            VulkanAccelerationStructure* acceleration_structure =
                ResourceCast(std::get<RaytracingTlasRef>(_argument).Get());
            _heap.CopyAccelerationStructureDescriptorToBuffer(
                _heap.GetOrCreateAccelerationStructureDescriptorOffset(acceleration_structure),
                _binding_byte_offset
            );
            break;
        }
        case VK_DESCRIPTOR_TYPE_MAX_ENUM:
            break;
        default:
            throw std::runtime_error("unsupported descriptor type");
    }
}

void WriteResourceSetDescriptors(
    VulkanDescriptorHeap&            _heap,
    const VulkanPipelineState&       _pipeline,
    const VulkanResourceSetBindings& _resource_bindings,
    const ArrayArguments&            _arguments,
    uint64                           _set_byte_offset
) {
    const uint64 active_argument_bits = _pipeline.GetActiveArgumentBits();
    for (const auto& binding : _resource_bindings.bindings) {
        const uint argument_index = binding.argument_index;
        if (binding.descriptor_count == 0 || argument_index >= 64 ||
            !(active_argument_bits & (uint64(1) << argument_index))) {
            continue;
        }
        const VkFormat buffer_format =
            g_platform_pixel_formats[_pipeline.GetArgumentInfo(argument_index).texel_format].format;
        WriteResourceBindingDescriptors(
            _heap, binding, _arguments[argument_index], buffer_format, _set_byte_offset + binding.byte_offset
        );
    }
}

Array<DescriptorSetBufferBinding> ResolveDescriptorSetBindings(
    VulkanDescriptorHeap&                    _heap,
    const VulkanPipelineState&               _pipeline,
    const ArrayArguments&                    _arguments,
    Array<VkDescriptorBufferBindingInfoEXT>& _descriptor_buffers,
    uint64                                   _next_set_byte_offset
) {
    const auto&                       set_bindings = _pipeline.binding_template->set_bindings;
    Array<DescriptorSetBufferBinding> resolved_set_bindings;
    resolved_set_bindings.reserve(set_bindings.size());
    for (const auto& [set_index, set_binding] : set_bindings) {
        auto&  descriptor_buffer = _descriptor_buffers[set_binding.buffer_index];
        uint64 set_byte_offset   = set_binding.initial_byte_offset;
        std::visit(
            Overload{
                [&]<VkDescriptorType DescriptorType>(
                    const VulkanBindlessSetBindings<DescriptorType>& resource_bindings
                ) {
                    BindlessArrayRef array =
                        std::get<BindlessArrayRef>(_arguments[resource_bindings.argument_index]);
                    VulkanBindlessArray* bindless_array = static_cast<VulkanBindlessArray*>(array.Get());
                    if constexpr (DescriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                        descriptor_buffer.address = bindless_array->bindless_buffer_descs->DeviceAddress();
                    } else {
                        descriptor_buffer.address = bindless_array->bindless_texture_descs->DeviceAddress();
                    }
                },
                [&](const VulkanResourceSetBindings& resource_bindings) {
                    set_byte_offset = _next_set_byte_offset;
                    _next_set_byte_offset += resource_bindings.allocation_size;
                    WriteResourceSetDescriptors(
                        _heap, _pipeline, resource_bindings, _arguments, set_byte_offset
                    );
                }
            },
            set_binding.resource_bindings
        );
        resolved_set_bindings.emplace_back(set_index, set_binding.buffer_index, set_byte_offset);
    }
    return resolved_set_bindings;
}

void BindDescriptorBufferSets(
    VkCommandBuffer                                   _command_buffer,
    VkPipelineBindPoint                               _bind_point,
    VkPipelineLayout                                  _pipeline_layout,
    std::span<const VkDescriptorBufferBindingInfoEXT> _descriptor_buffers,
    std::span<const DescriptorSetBufferBinding>       _set_bindings
) {
    if (!_descriptor_buffers.empty()) {
        vkCmdBindDescriptorBuffersEXT(
            _command_buffer, _descriptor_buffers.size(), _descriptor_buffers.data()
        );
    }
    for (const auto& set_binding : _set_bindings) {
        vkCmdSetDescriptorBufferOffsetsEXT(
            _command_buffer,
            _bind_point,
            _pipeline_layout,
            set_binding.set_index,
            1,
            &set_binding.buffer_index,
            &set_binding.offset
        );
    }
}

void PushShaderConstants(
    VkCommandBuffer               _command_buffer,
    const VkPushConstantsInfoKHR& _push_constants_info,
    std::span<const uint>         _constants
) {
    if (_push_constants_info.size == 0) {
        return;
    }
    if (_constants.size_bytes() < _push_constants_info.size) {
        throw std::invalid_argument("Push constant upload exceeds the source data buffer");
    }
    vkCmdPushConstants(
        _command_buffer,
        _push_constants_info.layout,
        _push_constants_info.stageFlags,
        _push_constants_info.offset,
        _push_constants_info.size,
        _constants.data()
    );
}

} // namespace

void VulkanCmdList::BindShaderArguments(
    const PipelineHandle& _pipeline_handle,
    const ArrayArguments& _arguments
) {
    auto* pipeline = static_cast<VulkanPipelineState*>(_pipeline_handle.GetState());
    assert(pipeline && pipeline->binding_template != nullptr && "Pipeline state has no bind template!");
    const VulkanPipelineBindingTemplate& binding_template = *pipeline->binding_template;
    VulkanDescriptorHeap&                descriptor_heap  = device.GetGlobalDescriptorHeap();

    const std::optional<uint64> allocation_offset = descriptor_heap.ReserveDescriptorRange(
        descriptor_buffer_lease, GetResourceDescriptorAllocationSize(binding_template)
    );
    if (!allocation_offset.has_value()) {
        throw std::runtime_error("descriptor submission lease exhausted");
    }

    // Pipeline metadata is shared and immutable. Only resolved addresses and
    // set offsets belong to this recording; the binding map need not be copied.
    auto       descriptor_buffers = binding_template.descriptor_buffers;
    const auto set_bindings       = ResolveDescriptorSetBindings(
        descriptor_heap, *pipeline, _arguments, descriptor_buffers, *allocation_offset
    );
    BindDescriptorBufferSets(
        command_buffer,
        pipeline->GetPipelineBindPoint(),
        pipeline->GetPipelineLayout(),
        descriptor_buffers,
        set_bindings
    );
    PushShaderConstants(command_buffer, binding_template.push_constants_info, _arguments.constants);
}

// Vertex input and dynamic state.
void VulkanCmdList::SetVertexBuffers(
    uint32_t                      _first_binding,
    std::span<const VkBuffer>     _buffers,
    std::span<const VkDeviceSize> _offsets
) {
    if (_buffers.size() != _offsets.size()) {
        throw std::invalid_argument("Vertex buffer and offset counts must match");
    }
    if (_buffers.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::out_of_range("Vertex buffer count exceeds uint32_t");
    }
    vkCmdBindVertexBuffers(
        command_buffer,
        _first_binding,
        static_cast<uint32_t>(_buffers.size()),
        _buffers.data(),
        _offsets.data()
    );
}

void VulkanCmdList::SetIndexBuffer(VulkanBuffer* _buffer, uint64 _offset, VkIndexType _index_type) {
    vkCmdBindIndexBuffer(command_buffer, _buffer->GetHandle(), _offset, _index_type);
}

void VulkanCmdList::SetScissor(const VkRect2D& _scissor) {
    vkCmdSetScissor(command_buffer, 0, 1, &_scissor);
}

void VulkanCmdList::SetViewport(const VkViewport& _view_port) {

    vkCmdSetViewport(command_buffer, 0, 1, &_view_port);
}

// Draw and mesh commands.
void VulkanCmdList::DrawInstanced(
    uint32_t _vertex_cnt,
    uint32_t _instance_cnt,
    uint32_t _first_vertex,
    uint32_t _first_instance
) {
    vkCmdDraw(command_buffer, _vertex_cnt, _instance_cnt, _first_vertex, _first_instance);
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

void VulkanCmdList::DrawIndirect(
    VulkanBuffer* _buffer,
    uint64        _offset,
    uint32_t      _draw_cnt,
    uint32_t      _stride
) {
    vkCmdDrawIndirect(command_buffer, _buffer->GetHandle(), _offset, _draw_cnt, _stride);
}

void VulkanCmdList::DrawIndirectCount(
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

void VulkanCmdList::DrawIndexedIndirectCount(
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

// Compute commands.
void VulkanCmdList::Dispatch(uint _group_count_x, uint _group_count_y, uint _group_count_z) {
    vkCmdDispatch(command_buffer, _group_count_x, _group_count_y, _group_count_z);
}

void VulkanCmdList::DispatchIndirect(VulkanBuffer* _buffer, uint64 _offset) {
    vkCmdDispatchIndirect(command_buffer, _buffer->GetHandle(), _offset);
}

// Transfer and clear commands.
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
    uint32         _mip_level,
    uint32         _array_layer
) {
    VkImageAspectFlags aspect =
        _src->GetResourceType() == RRT_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkBufferImageCopy copy_region = {
        .bufferOffset      = _dst_offset,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {.aspectMask = aspect,
             .mipLevel = _mip_level,
             .baseArrayLayer = _array_layer,
             .layerCount = 1},
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

void VulkanCmdList::CopyTexture(
    VulkanTexture* _src,
    VulkanTexture* _dst,
    uint3          _extent,
    uint3          _src_offset,
    uint3          _dst_offset,
    uint32         _src_mip_level,
    uint32         _dst_mip_level,
    VkImageLayout  _src_layout,
    VkImageLayout  _dst_layout
) {
    if ((_src_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
         _src_layout != VK_IMAGE_LAYOUT_GENERAL) ||
        (_dst_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         _dst_layout != VK_IMAGE_LAYOUT_GENERAL)) {
        throw std::invalid_argument(
            "vkCmdCopyImage requires GENERAL or transfer-optimal layouts"
        );
    }
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
        _src_layout,
        _dst->GetHandle(),
        _dst_layout,
        1,
        &copy_region
    );
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

// Acceleration structures.
void VulkanCmdList::BuildAccelerationStructures(
    const Array<VkAccelerationStructureBuildGeometryInfoKHR>& _build_infos,
    const Array<VkAccelerationStructureBuildRangeInfoKHR*>&   _build_ranges
) {
    vkCmdBuildAccelerationStructuresKHR(
        command_buffer, _build_infos.size(), _build_infos.data(), _build_ranges.data()
    );
}

// Queries.
void VulkanCmdList::ResetQueryPool(VkNativeQueryPool& _query_pool, uint32 _first_query, uint32 _query_cnt) {
    vkCmdResetQueryPool(command_buffer, _query_pool.GetHandle(), _first_query, _query_cnt);
}

void VulkanCmdList::BeginQuery(
    VkNativeQueryPool&  _query_pool,
    uint32              _query,
    VkQueryControlFlags _flags
) {
    vkCmdBeginQuery(
        command_buffer, _query_pool.GetHandle(), _query, _flags
    );
}

void VulkanCmdList::EndQuery(
    VkNativeQueryPool& _query_pool,
    uint32             _query
) {
    vkCmdEndQuery(command_buffer, _query_pool.GetHandle(), _query);
}

void VulkanCmdList::WriteTimestamp(
    VkNativeQueryPool&       _query_pool,
    uint32                   _query_idx,
    VkPipelineStageFlagBits2 _stage
) {
    vkCmdWriteTimestamp2(command_buffer, _stage, _query_pool.GetHandle(), _query_idx);
}

// Debug labels.
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
