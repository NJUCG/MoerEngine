#include "VulkanDescriptor.h"
#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanRHIResource.h"
#include "VulkanUtil.h"
#include <volk.h>

#include "misc/MacroUtils.h"
#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"

#include <cassert>

const float default_pool_size[VK_DESCRIPTOR_TYPE_RANGE_SIZE] = {
    4096, // VK_DESCRIPTOR_TYPE_SAMPLER
    4096, // VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
    4096, // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
    //1 / 8.0,// VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
    //1 / 2.0,// VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
    //1 / 8.0,// VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
    //1 / 4.0,// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    //1 / 8.0,// VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
    //4,      // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
    //1 / 8.0,// VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
    //1 / 8.0 // VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
};
namespace Moer::Render {

static constexpr std::string_view s_ring_desc_buffer_name = "VkDescriporHeap::RingDescriptorBuffer";

VulkanDescriptorSetsLayout::VulkanDescriptorSetsLayout(
    VulkanDevice*                                        _device,
    const Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings
) :
    VulkanDeviceObject(_device) {
    m_layouts.resize(_descriptor_bindings.size(), VK_NULL_HANDLE);
    for (uint32_t set_idx = 0; set_idx < _descriptor_bindings.size(); ++set_idx) {
        for (const auto& binding : _descriptor_bindings[set_idx]) {
            m_descriptor_type_count[binding.descriptorType] += binding.descriptorCount;
        }
        VkDescriptorSetLayoutCreateInfo set_layout_ci{};
        set_layout_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout_ci.bindingCount = _descriptor_bindings[set_idx].size();
        set_layout_ci.pBindings    = _descriptor_bindings[set_idx].data();
        VK_CHECK_RESULT(
            vkCreateDescriptorSetLayout(m_device->GetDevice(), &set_layout_ci, nullptr, &m_layouts[set_idx])
        );
    }
}

VulkanDescriptorSetsLayout::~VulkanDescriptorSetsLayout() {
    for (const auto& layout : m_layouts) {
        vkDestroyDescriptorSetLayout(m_device->GetDevice(), layout, nullptr);
    }
}

enum class EBindlessSizeType : uint8 {
    Buffer,
    Sampler,
    Image,
    Num
};
enum class EBindlessSetType : uint8 {
    Buffer,
    SamplerAndImage,
    Num
};

VkImageLayout DecideImageLayout(VkDescriptorType _type) {
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return VK_IMAGE_LAYOUT_GENERAL;
        default:
            LOG_ERROR("Unsupported image descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, _type));
            return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

VulkanDescriptorHeap::VulkanDescriptorHeap(VulkanDevice& _device) :
    m_device(&_device),
    storage_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize
    ),
    uniform_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.uniformBufferDescriptorSize
    ),
    storage_texel_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.storageTexelBufferDescriptorSize
    ),
    uniform_texel_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.uniformTexelBufferDescriptorSize
    ),
    buffer_desc_stride(
        std::max(
            _device.GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize,
            _device.GetOptionalProperties().descriptor_buffer_properties.uniformBufferDescriptorSize
        )
    ),
    image_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize
    ),
    texture_desc_offset(
        _device.GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize *
        VulkanDevice::bindless_sampler_cnt
    ),
    accel_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.accelerationStructureDescriptorSize
    ),
    image_offset(0),
    buffer_offset(0) {

    buffer_desc_data.resize(10086 * buffer_desc_stride);
    image_desc_data.resize(10086 * image_desc_stride);
    accel_desc_data.resize(10086 * accel_desc_stride);

    VkBuffer           desc_buffer            = VK_NULL_HANDLE;
    VmaAllocation      desc_buffer_allocation = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_ci.size  = s_queue_max_frame_in_flight * 256 * 16 * 100;
    buffer_ci.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

#if CUDA_PASS_IN_RASTER
    buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK_RESULT(vmaCreateBuffer(
        _device.GetVmaAllocator(), &buffer_ci, &alloc_ci, &desc_buffer, &desc_buffer_allocation, nullptr
    ));

    BufferInfo buffer_info{};
    buffer_info.size   = buffer_ci.size;
    buffer_info.stride = 1;
    buffer_info.usage  = EBufferUsageFlags::UNORDERED_ACCESS;

    ring_desc_buffer = MoerNew(VulkanBuffer)(
        s_ring_desc_buffer_name, buffer_info, *m_device, desc_buffer, desc_buffer_allocation, false, true
    );

    //fill offsets
    ring_buffer_offsets.resize(m_device->cmd_alloc_limits);

    uint64 alignment =
        m_device->GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment;

    for (uint32_t i = 0; i < m_device->cmd_alloc_limits; ++i) {
        ring_buffer_offsets[i] = Moer::AlignUp(buffer_ci.size / m_device->cmd_alloc_limits * i, alignment);
    }
    current_offset = 0;
    vmaMapMemory(m_device->GetVmaAllocator(), ring_desc_buffer->GetAllocation(), (void**)&map_ptr);

    //fill sampler descs in image_data
    sample_desc_stride = _device.GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize;
    for (uint32_t i = 0; i < VulkanDevice::bindless_sampler_cnt; ++i) {
        VkDescriptorImageInfo  sampler_info{.sampler = VK_NULL_HANDLE};
        VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        if (i >= m_device->ImmutableSamplerCount()) {
            break;
        }
        VkSampler sampler       = m_device->GetImmutableSamplers()[i];
        desc_info.data.pSampler = &sampler;
        desc_info.type          = VK_DESCRIPTOR_TYPE_SAMPLER;
        vkGetDescriptorEXT(
            m_device->GetDevice(),
            &desc_info,
            sample_desc_stride,
            image_desc_data.data() + i * sample_desc_stride
        );
    }
}

VulkanDescriptorHeap::~VulkanDescriptorHeap() {
    if (ring_desc_buffer) {
        vmaUnmapMemory(m_device->GetVmaAllocator(), ring_desc_buffer->GetAllocation());
        MoerDelete(ring_desc_buffer);
        ring_desc_buffer = nullptr;
    }
    if (buffer_desc_buffer) {
        MoerDelete(buffer_desc_buffer);
        buffer_desc_buffer = nullptr;
    }
    if (image_desc_buffer) {
        MoerDelete(image_desc_buffer);
        image_desc_buffer = nullptr;
    }
}

uint VulkanDescriptorHeap::GetBufferDescIdx(
    const BufferView& _in_buffer,
    VkDescriptorType  _type,
    VkFormat          _format
) {
    assert(_in_buffer.GetBuffer() != nullptr && "buffer is nullptr");
    uint idx = 0;

    VulkanBuffer* vk_buffer = ResourceCast(_in_buffer.GetBuffer());
    auto&         indices   = vk_buffer->GetDescriptorIndices(_type);

    auto it = indices.find(_in_buffer.byte_offset);
    if (it != indices.end()) {
        return it->second * buffer_desc_stride;
    } else {
        VkDescriptorAddressInfoEXT buffer_info{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        buffer_info.address = vk_buffer->DeviceAddress() + _in_buffer.byte_offset;
        buffer_info.range   = _in_buffer.GetByteSize();
        VkDescriptorGetInfoEXT buffer_desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        buffer_desc_info.type  = _type;
        uint     desc_size     = 0;
        VkFormat buffer_format = VulkanEnumTranslator::METoVKFormat(_in_buffer.format);
        _format                = buffer_format == VK_FORMAT_UNDEFINED ? _format : buffer_format;
        switch (_type) {
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                buffer_info.format                        = _format;
                buffer_desc_info.data.pStorageTexelBuffer = &buffer_info;
                desc_size                                 = storage_texel_desc_stride;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                buffer_desc_info.data.pStorageBuffer = &buffer_info;
                desc_size                            = storage_desc_stride;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                buffer_desc_info.data.pUniformBuffer = &buffer_info;
                desc_size                            = uniform_desc_stride;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                buffer_info.format                        = _format;
                buffer_desc_info.data.pUniformTexelBuffer = &buffer_info;
                desc_size                                 = uniform_texel_desc_stride;
                break;
            default:
                LOG_ERROR(
                    "Unsupported buffer descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, _type)
                );
                assert(false && "Unsupported buffer descriptor type");
                return 0;
        }
        assert(
            m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize <=
            sizeof(BufferCpuDescHandle)
        );

        std::lock_guard<std::mutex> lock(m_mutex);
        if (buffer_free_list.empty()) {
            idx = buffer_offset / buffer_desc_stride;
            buffer_offset += buffer_desc_stride;
        } else {
            idx = buffer_free_list.back();
            buffer_free_list.pop_back();
        }

        indices[_in_buffer.byte_offset] = idx;

        vkGetDescriptorEXT(
            m_device->GetDevice(),
            &buffer_desc_info,
            desc_size,
            buffer_desc_data.data() + idx * buffer_desc_stride
        );
    }
    return idx * buffer_desc_stride;
}
void VulkanDescriptorHeap::FreeBufferDescIdx(uint _idx) {
    std::lock_guard<std::mutex> lock(m_mutex);
    buffer_free_list.push_back(_idx);
}

uint VulkanDescriptorHeap::GetImageDescIdx(const TextureView* _in_image, VkImageLayout _layout) {
    auto* texture = ResourceCast(_in_image->texture);
    assert(texture != nullptr && "texture is nullptr");
    VkTextureDescKey key{_layout, _in_image->mip_level, _in_image->num_mips};
    auto             res = texture->m_descriptor_indices.try_emplace(key, -1);
    if (!res.second) {
        return res.first->second * image_desc_stride + texture_desc_offset;
    }
    auto& idx = res.first->second;
    {
        VkDescriptorImageInfo image_info{
            .imageView = texture->GetView(_in_image->mip_level, _in_image->num_mips), .imageLayout = _layout
        };
        VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        desc_info.type               = _layout == VK_IMAGE_LAYOUT_GENERAL ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                                                                            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        desc_info.data.pSampledImage = &image_info;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (image_free_list.empty()) {
            idx = image_offset / image_desc_stride;
            image_offset += image_desc_stride;
        } else {
            idx = image_free_list.back();
            image_free_list.pop_back();
        }
        vkGetDescriptorEXT(
            m_device->GetDevice(),
            &desc_info,
            image_desc_stride,
            image_desc_data.data() + idx * image_desc_stride + texture_desc_offset
        );
    }
    return idx * image_desc_stride + texture_desc_offset;
}
void VulkanDescriptorHeap::FreeImageDescIdx(uint _idx) {
    std::lock_guard<std::mutex> lock(m_mutex);
    image_free_list.push_back(_idx);
}
uint VulkanDescriptorHeap::GetSamplerDescIdx(Sampler _sampler) {
    return m_device->GetSamplerIdx(_sampler) * sample_desc_stride;
}

uint VulkanDescriptorHeap::GetAccelDescIdx(VulkanAccelerationStructure* _as) {
    assert(_as != nullptr && "accel struct is nullptr");
    if (_as->m_descriptor_idx >= 0) {
        return _as->m_descriptor_idx * accel_desc_stride;
    }
    VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    desc_info.type                       = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    desc_info.data.accelerationStructure = _as->underlying_buffer->DeviceAddress();
    uint                        idx      = 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (accel_free_list.empty()) {
        idx = accel_offset / accel_desc_stride;
        accel_offset += accel_desc_stride;
    } else {
        idx = accel_free_list.back();
        accel_free_list.pop_back();
    }
    _as->m_descriptor_idx = idx;
    vkGetDescriptorEXT(
        m_device->GetDevice(), &desc_info, accel_desc_stride, accel_desc_data.data() + idx * accel_desc_stride
    );

    return idx * accel_desc_stride;
}

uint VulkanDescriptorHeap::FreeAccelDescIdx(uint _idx) {
    std::lock_guard<std::mutex> lock(m_mutex);
    accel_free_list.push_back(_idx);
    return 0;
}

void VulkanDescriptorHeap::BeginPushDescriptors(uint _frame_idx) {
    _frame_idx     = _frame_idx % m_device->cmd_alloc_limits;
    current_offset = ring_buffer_offsets[_frame_idx];
}
void VulkanDescriptorHeap::EndPushDescriptors(uint _frame_idx) {
    _frame_idx         = _frame_idx % m_device->cmd_alloc_limits;
    uint64 base_offset = ring_buffer_offsets[_frame_idx];
    vmaFlushAllocation(
        m_device->GetVmaAllocator(),
        ring_desc_buffer->GetAllocation(),
        base_offset,
        current_offset - base_offset
    );
}

// void VulkanDescriptorHeap::PushBufferDesc(uint64 _src_offset, uint64 _set_offset) {
//     std::lock_guard<std::mutex> lock(m_mutex);
//     memcpy(map_ptr + current_offset + _set_offset, buffer_desc_data.data() + _src_offset, buffer_desc_stride);
// }

void VulkanDescriptorHeap::PushUniformDesc(uint64 _src_offset, uint64 _set_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    memcpy(
        map_ptr + current_offset + _set_offset, buffer_desc_data.data() + _src_offset, uniform_desc_stride
    );
}

void VulkanDescriptorHeap::PushStorageDesc(uint64 _src_offset, uint64 _set_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    memcpy(
        map_ptr + current_offset + _set_offset, buffer_desc_data.data() + _src_offset, storage_desc_stride
    );
}

void VulkanDescriptorHeap::PushImageDesc(uint64 _src_offset, uint64 _set_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    memcpy(map_ptr + current_offset + _set_offset, image_desc_data.data() + _src_offset, image_desc_stride);
}

void VulkanDescriptorHeap::PushSamplerDesc(uint64 _src_offset, uint64 _set_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    memcpy(map_ptr + current_offset + _set_offset, image_desc_data.data() + _src_offset, image_desc_stride);
}

void VulkanDescriptorHeap::PushAccelDesc(uint64 _src_offset, uint64 _set_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    memcpy(map_ptr + current_offset + _set_offset, accel_desc_data.data() + _src_offset, accel_desc_stride);
}

void VulkanDescriptorHeap::IncrementOffset(uint64 _size) {
    current_offset += _size;
}
} // namespace Moer::Render