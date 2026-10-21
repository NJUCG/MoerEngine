#include "VulkanCommandList.h"

#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include "VulkanCommandQueries.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanFault.h"
#include "VulkanRHIResource.h"
#include <string_view>

#include "shader/ShaderPipeline.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <format>
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
    const uint32_t family_index = m_device->GetQueueFamilyIndex(_queue_type);

    VkCommandPoolCreateInfo pool_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .queueFamilyIndex = family_index
    };

    const VkResult result = vkCreateCommandPool(_device->GetDevice(), &pool_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
        _device->TryLatchFirstFault(
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::CommandPoolCreate,
                .queue_type = queue_type,
            },
            result
        );
        throw std::runtime_error(
            "Vulkan command-pool creation failed: " + std::string(VulkanResultName(result))
        );
    }
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

VulkanCommandList::VulkanCommandList(
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
    const VkResult result =
        vkAllocateCommandBuffers(device.GetDevice(), &command_buffer_info, &command_buffer);
    if (result != VK_SUCCESS) {
        device.TryLatchFirstFault(
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::CommandBufferAllocate,
                .queue_type = command_pool->GetQueueType(),
            },
            result
        );
        throw std::runtime_error(
            "Vulkan command-buffer allocation failed: " + std::string(VulkanResultName(result))
        );
    }
    try {
        device.SetResourceName(
            (uint64)command_buffer,
            VK_OBJECT_TYPE_COMMAND_BUFFER,
            std::format(
                "{}CommandBuffer_{}",
                level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? "Primary" : "Secondary",
                command_pool->GetQueueName()
            )
        );
    } catch (...) {
        // A failed constructor has no destructor to release the native buffer.
        vkFreeCommandBuffers(device.GetDevice(), command_pool->GetHandle(), 1, &command_buffer);
        throw;
    }
}

VulkanCommandList::~VulkanCommandList() {
    vkFreeCommandBuffers(device.GetDevice(), command_pool->GetHandle(), 1, &command_buffer);
}

// Recording lifecycle.
VkResult VulkanCommandList::Begin() {
    assert(level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    VkCommandBufferBeginInfo begin_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .pInheritanceInfo = nullptr
    };
    return vkBeginCommandBuffer(command_buffer, &begin_info);
}

VkResult VulkanCommandList::BeginSecondary(
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

VkResult VulkanCommandList::End() {
    return vkEndCommandBuffer(command_buffer);
}

void VulkanCommandList::ExecuteSecondaryCommands(std::span<const VkCommandBuffer> _secondary_buffers) {
    assert(level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    if (_secondary_buffers.empty()) {
        return;
    }
    vkCmdExecuteCommands(
        command_buffer, static_cast<uint32_t>(_secondary_buffers.size()), _secondary_buffers.data()
    );
}

// Rendering scope.
void VulkanCommandList::BeginRendering(const VkRenderingInfo& _info) {
    vkCmdBeginRendering(command_buffer, &_info);
}

void VulkanCommandList::EndRendering() {
    vkCmdEndRendering(command_buffer);
}

// Pipeline and shader arguments.
void VulkanCommandList::BindPipeline(const PipelineHandle& _pipeline_handle) {
    auto* pipeline = static_cast<VulkanPipelineState*>(_pipeline_handle.GetState());
    vkCmdBindPipeline(command_buffer, pipeline->GetPipelineBindPoint(), pipeline->GetHandle());
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

void VulkanCommandList::BindShaderArguments(
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
void VulkanCommandList::SetVertexBuffers(
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

void VulkanCommandList::SetIndexBuffer(VulkanBuffer* _buffer, uint64 _offset, VkIndexType _index_type) {
    vkCmdBindIndexBuffer(command_buffer, _buffer->GetHandle(), _offset, _index_type);
}

void VulkanCommandList::SetScissor(const VkRect2D& _scissor) {
    vkCmdSetScissor(command_buffer, 0, 1, &_scissor);
}

void VulkanCommandList::SetViewport(const VkViewport& _viewport) {

    vkCmdSetViewport(command_buffer, 0, 1, &_viewport);
}

// Draw and mesh commands.
void VulkanCommandList::DrawInstanced(
    uint32_t _vertex_count,
    uint32_t _instance_count,
    uint32_t _first_vertex,
    uint32_t _first_instance
) {
    vkCmdDraw(command_buffer, _vertex_count, _instance_count, _first_vertex, _first_instance);
}

void VulkanCommandList::DrawIndexedInstanced(
    uint32_t _index_count,
    uint32_t _instance_count,
    uint32_t _first_index,
    uint32_t _vertex_offset,
    uint32_t _first_instance
) {
    vkCmdDrawIndexed(
        command_buffer, _index_count, _instance_count, _first_index, _vertex_offset, _first_instance
    );
}

void VulkanCommandList::DrawIndirect(
    VulkanBuffer* _buffer,
    uint64        _offset,
    uint32_t      _draw_count,
    uint32_t      _stride
) {
    vkCmdDrawIndirect(command_buffer, _buffer->GetHandle(), _offset, _draw_count, _stride);
}

void VulkanCommandList::DrawIndirectCount(
    VulkanBuffer* _commands,
    uint64        _commands_offset,
    VulkanBuffer* _count_buffer,
    uint64        _count_buffer_offset,
    uint32_t      _max_draw_count,
    uint32_t      _stride
) {
    vkCmdDrawIndirectCount(
        command_buffer,
        _commands->GetHandle(),
        _commands_offset,
        _count_buffer->GetHandle(),
        _count_buffer_offset,
        _max_draw_count,
        _stride
    );
}

void VulkanCommandList::DrawIndexedIndirect(
    VulkanBuffer* _buffer,
    uint64        _offset,
    uint32_t      _draw_count,
    uint32_t      _stride
) {
    vkCmdDrawIndexedIndirect(command_buffer, _buffer->GetHandle(), _offset, _draw_count, _stride);
}

void VulkanCommandList::DrawIndexedIndirectCount(
    VulkanBuffer* _commands,
    uint64        _commands_offset,
    VulkanBuffer* _count_buffer,
    uint64        _count_buffer_offset,
    uint32_t      _max_draw_count,
    uint32_t      _stride
) {
    vkCmdDrawIndexedIndirectCount(
        command_buffer,
        _commands->GetHandle(),
        _commands_offset,
        _count_buffer->GetHandle(),
        _count_buffer_offset,
        _max_draw_count,
        _stride
    );
}

void VulkanCommandList::DispatchMesh(
    uint32_t _group_count_x,
    uint32_t _group_count_y,
    uint32_t _group_count_z
) {
    vkCmdDrawMeshTasksEXT(command_buffer, _group_count_x, _group_count_y, _group_count_z);
}

void VulkanCommandList::DispatchMeshIndirect(
    VulkanBuffer* _buffer,
    uint64        _offset,
    uint32_t      _draw_count,
    uint32_t      _stride
) {
    vkCmdDrawMeshTasksIndirectEXT(command_buffer, _buffer->GetHandle(), _offset, _draw_count, _stride);
}

void VulkanCommandList::DispatchMeshIndirectCount(
    VulkanBuffer* _commands,
    uint64        _commands_offset,
    VulkanBuffer* _count_buffer,
    uint64        _count_buffer_offset,
    uint32_t      _max_draw_count,
    uint32_t      _stride
) {
    vkCmdDrawMeshTasksIndirectCountEXT(
        command_buffer,
        _commands->GetHandle(),
        _commands_offset,
        _count_buffer->GetHandle(),
        _count_buffer_offset,
        _max_draw_count,
        _stride
    );
}

// Compute commands.
void VulkanCommandList::Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) {
    vkCmdDispatch(command_buffer, _group_count_x, _group_count_y, _group_count_z);
}

void VulkanCommandList::DispatchIndirect(VulkanBuffer* _buffer, uint64 _offset) {
    vkCmdDispatchIndirect(command_buffer, _buffer->GetHandle(), _offset);
}

// Transfer and clear commands.
void VulkanCommandList::CopyBuffer(
    VulkanBuffer* _src,
    VulkanBuffer* _dst,
    uint64        _byte_size,
    uint64        _src_offset,
    uint64        _dst_offset
) {

    VkBufferCopy copy_region = {.srcOffset = _src_offset, .dstOffset = _dst_offset, .size = _byte_size};
    vkCmdCopyBuffer(command_buffer, _src->GetHandle(), _dst->GetHandle(), 1, &copy_region);
}

namespace {

VkImageAspectFlags GetCopyAspect(const VulkanTexture& _texture) {
    switch (GetPixelFormatInfo(_texture.GetFormat()).aspect) {
        case EPixelFormatAspect::Color:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        case EPixelFormatAspect::Depth:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case EPixelFormatAspect::Stencil:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            // The current copy interface cannot select a plane of a combined
            // depth/stencil or multi-plane format. Do not silently choose one.
            throw std::invalid_argument("Texture copy requires a supported single-aspect format");
    }
}

VkBufferImageCopy MakeBufferImageCopy(
    const VulkanBuffer&  _buffer,
    const VulkanTexture& _texture,
    uint64               _byte_size,
    uint64               _buffer_offset,
    uint3                _texture_offset,
    uint3                _extent,
    uint32               _mip_level,
    uint32               _array_layer
) {
    const VkImageAspectFlags aspect = GetCopyAspect(_texture);
    if (_mip_level >= _texture.GetNumMips() || _array_layer >= _texture.GetNumArray()) {
        throw std::out_of_range("Texture copy subresource is out of range");
    }
    if (_texture.GetNumSamples() != 1 || _extent.x == 0 || _extent.y == 0 || _extent.z == 0) {
        throw std::invalid_argument("Buffer/image copy requires a nonempty single-sampled region");
    }
    const uint64 expected_byte_size = GetSizeFromImageFormat(_texture.GetFormat(), _extent);
    if (_byte_size != expected_byte_size) {
        throw std::invalid_argument(std::format(
            "Texture copy size mismatch: texture='{}', mip={}, layer={}, bytes={}, expected={}",
            _texture.GetName(),
            _mip_level,
            _array_layer,
            _byte_size,
            expected_byte_size
        ));
    }
    if (_buffer_offset > _buffer.GetByteSize() || _byte_size > _buffer.GetByteSize() - _buffer_offset) {
        throw std::out_of_range("Texture copy exceeds the buffer range");
    }
    return VkBufferImageCopy{
        .bufferOffset      = _buffer_offset,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {.aspectMask = aspect, .mipLevel = _mip_level, .baseArrayLayer = _array_layer, .layerCount = 1},
        .imageOffset =
            {static_cast<int32_t>(_texture_offset.x),
             static_cast<int32_t>(_texture_offset.y),
             static_cast<int32_t>(_texture_offset.z)},
        .imageExtent = {_extent.x, _extent.y, _extent.z}
    };
}

} // namespace

void VulkanCommandList::CopyBufferToTexture(
    VulkanBuffer*  _src,
    VulkanTexture* _dst,
    uint64         _byte_size,
    uint64         _src_offset,
    uint3          _dst_offset,
    uint3          _extent,
    uint32         _mip_level,
    uint32         _array_layer
) {
    const VkBufferImageCopy copy_region = MakeBufferImageCopy(
        *_src, *_dst, _byte_size, _src_offset, _dst_offset, _extent, _mip_level, _array_layer
    );
    vkCmdCopyBufferToImage(
        command_buffer,
        _src->GetHandle(),
        _dst->GetHandle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy_region
    );
}

void VulkanCommandList::CopyTextureToBuffer(
    VulkanTexture* _src,
    VulkanBuffer*  _dst,
    uint64         _byte_size,
    uint3          _src_offset,
    uint64         _dst_offset,
    uint3          _extent,
    uint32         _mip_level,
    uint32         _array_layer
) {
    const VkBufferImageCopy copy_region = MakeBufferImageCopy(
        *_dst, *_src, _byte_size, _dst_offset, _src_offset, _extent, _mip_level, _array_layer
    );
    vkCmdCopyImageToBuffer(
        command_buffer,
        _src->GetHandle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        _dst->GetHandle(),
        1,
        &copy_region
    );
}

void VulkanCommandList::CopyTexture(
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
    if ((_src_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && _src_layout != VK_IMAGE_LAYOUT_GENERAL) ||
        (_dst_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && _dst_layout != VK_IMAGE_LAYOUT_GENERAL)) {
        throw std::invalid_argument("vkCmdCopyImage requires GENERAL or transfer-optimal layouts");
    }
    const VkImageAspectFlags aspect = GetCopyAspect(*_src);
    if (aspect != GetCopyAspect(*_dst)) {
        throw std::invalid_argument("Image copy requires matching source and destination aspects");
    }
    VkImageCopy copy_region = {
        .srcSubresource =
            {.aspectMask = aspect, .mipLevel = _src_mip_level, .baseArrayLayer = 0, .layerCount = 1},
        .srcOffset =
            {static_cast<int32_t>(_src_offset.x),
             static_cast<int32_t>(_src_offset.y),
             static_cast<int32_t>(_src_offset.z)},
        .dstSubresource =
            {.aspectMask = aspect, .mipLevel = _dst_mip_level, .baseArrayLayer = 0, .layerCount = 1},
        .dstOffset =
            {static_cast<int32_t>(_dst_offset.x),
             static_cast<int32_t>(_dst_offset.y),
             static_cast<int32_t>(_dst_offset.z)},
        .extent =
            {static_cast<uint32_t>(_extent.x),
             static_cast<uint32_t>(_extent.y),
             static_cast<uint32_t>(_extent.z)}
    };

    vkCmdCopyImage(
        command_buffer, _src->GetHandle(), _src_layout, _dst->GetHandle(), _dst_layout, 1, &copy_region
    );
}

void VulkanCommandList::ClearBufferUInt(
    VulkanBuffer* _buffer,
    uint64        _offset,
    uint64        _byte_size,
    uint32        _data
) {
    vkCmdFillBuffer(command_buffer, _buffer->GetHandle(), _offset, _byte_size, _data);
}

void VulkanCommandList::ClearColorTexture(
    VulkanTexture*                 _texture,
    const VkClearColorValue&       _color,
    const VkImageSubresourceRange& _range
) {
    vkCmdClearColorImage(
        command_buffer, _texture->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &_color, 1, &_range
    );
}

// Acceleration structures.
void VulkanCommandList::BuildAccelerationStructures(
    const Array<VkAccelerationStructureBuildGeometryInfoKHR>& _build_infos,
    const Array<VkAccelerationStructureBuildRangeInfoKHR*>&   _build_ranges
) {
    vkCmdBuildAccelerationStructuresKHR(
        command_buffer, _build_infos.size(), _build_infos.data(), _build_ranges.data()
    );
}

// Queries.
void VulkanCommandList::ResetQueryPool(
    VkNativeQueryPool& _query_pool,
    uint32             _first_query,
    uint32             _query_count
) {
    vkCmdResetQueryPool(command_buffer, _query_pool.GetHandle(), _first_query, _query_count);
}

void VulkanCommandList::BeginQuery(
    VkNativeQueryPool&  _query_pool,
    uint32              _query,
    VkQueryControlFlags _flags
) {
    vkCmdBeginQuery(command_buffer, _query_pool.GetHandle(), _query, _flags);
}

void VulkanCommandList::EndQuery(VkNativeQueryPool& _query_pool, uint32 _query) {
    vkCmdEndQuery(command_buffer, _query_pool.GetHandle(), _query);
}

void VulkanCommandList::WriteTimestamp(
    VkNativeQueryPool&       _query_pool,
    uint32                   _query,
    VkPipelineStageFlagBits2 _stage
) {
    vkCmdWriteTimestamp2(command_buffer, _stage, _query_pool.GetHandle(), _query);
}

// Debug labels.
void VulkanCommandList::BeginLabel(std::string_view _label, float4 _color) {
    const std::string    label_name(_label);
    VkDebugUtilsLabelEXT label = {
        .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pNext      = nullptr,
        .pLabelName = label_name.c_str(),
        .color      = {_color.x, _color.y, _color.z, _color.w}
    };
    vkCmdBeginDebugUtilsLabelEXT(command_buffer, &label);
}

void VulkanCommandList::EndLabel() {
    vkCmdEndDebugUtilsLabelEXT(command_buffer);
}

void VulkanCommandList::InsertLabel(std::string_view _label, float4 _color) {
    const std::string    label_name(_label);
    VkDebugUtilsLabelEXT label = {
        .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pNext      = nullptr,
        .pLabelName = label_name.c_str(),
        .color      = {_color.x, _color.y, _color.z, _color.w}
    };
    vkCmdInsertDebugUtilsLabelEXT(command_buffer, &label);
}

} // namespace Moer::Render
