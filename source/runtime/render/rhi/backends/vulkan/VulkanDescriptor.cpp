#include "VulkanEnumTranslator.h"
#include "VulkanRaytracingResource.h"
#include "VulkanDescriptor.h"

#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"

#include "misc/Alignment.h"
#include "misc/RAII.h"
#include "rhi/RHIResource.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Moer::Render {

uint64 GetBindlessTextureDescriptorOffset(const VulkanDevice& device) {
    if (device.GetDescriptorMode() != VulkanDescriptorMode::Buffer)
        return 0;
    const auto& properties = device.GetOptionalProperties().descriptor_buffer_properties;
    return Moer::AlignUp(
        device.GetGlobalDescriptorHeap().GetBindlessSamplerSetSize(),
        properties.descriptorBufferOffsetAlignment
    );
}

namespace {

static constexpr std::string_view s_descriptor_buffer_name         = "VulkanDescriptorHeap::DescriptorBuffer";
static constexpr uint32           s_descriptor_cache_slot_capacity = 10086;
static constexpr uint64           s_descriptor_buffer_capacity     = 6 * 1024 * 1024;
static constexpr uint64           s_heap_capacity                  = 64 * 1024 * 1024;
static constexpr uint64           s_descriptor_block_size          = 64 * 1024;

uint64 GetDescriptorBlockSize(const VulkanDevice& device);

uint64 GetDescriptorCapacity(const VulkanDevice& device) {
    if (device.GetDescriptorMode() != VulkanDescriptorMode::Heap)
        return s_descriptor_buffer_capacity;
    const auto& p = device.GetOptionalProperties().descriptor_heap_properties;
    if (p.maxResourceHeapSize <= p.minResourceHeapReservedRange)
        throw std::runtime_error("No usable resource heap memory");
    const uint64 block = GetDescriptorBlockSize(device);
    return std::min<uint64>(s_heap_capacity, p.maxResourceHeapSize - p.minResourceHeapReservedRange) / block *
           block;
}

uint64 GetDescriptorBlockSize(const VulkanDevice& _device) {
    const uint64 alignment = std::max<uint64>(
        _device.GetDescriptorMode() == VulkanDescriptorMode::Heap ?
            std::max(
                _device.GetOptionalProperties().descriptor_heap_properties.imageDescriptorAlignment,
                _device.GetOptionalProperties().descriptor_heap_properties.bufferDescriptorAlignment
            ) :
            std::max<uint64>(
                1,
                _device.GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment
            ),
        _device.GetCoreProperties().core_1_0.limits.nonCoherentAtomSize
    );
    return Moer::AlignUp(s_descriptor_block_size, alignment);
}

} // namespace

VulkanDescriptorHeap::DescriptorCache::DescriptorCache(uint _slot_stride) :
    slot_stride(_slot_stride),
    bytes(static_cast<size_t>(_slot_stride) * s_descriptor_cache_slot_capacity) {
    // Releasing a slot, including rollback after a failed native encode, must not allocate.
    if (_slot_stride)
        free_slots.reserve(s_descriptor_cache_slot_capacity);
}

uint VulkanDescriptorHeap::DescriptorCache::AllocateSlot() {
    if (!free_slots.empty()) {
        const uint slot = free_slots.back();
        if (slot >= next_slot) {
            throw std::runtime_error("descriptor free-list index is invalid");
        }
        free_slots.pop_back();
        return slot;
    }
    if (slot_stride == 0 || next_slot >= bytes.size() / slot_stride) {
        throw std::runtime_error("descriptor cache exhausted or descriptor type unavailable");
    }
    return next_slot++;
}

VulkanDescriptorHeap::VulkanDescriptorHeap(VulkanDevice& _device) :
    m_device(&_device),
    buffer_cache(std::max(
        {GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
         GetDescriptorSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
         GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER),
         GetDescriptorSize(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)}
    )),
    buffer_descriptor_types(s_descriptor_cache_slot_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM),
    image_cache(std::max(
        GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
    )),
    image_descriptor_types(s_descriptor_cache_slot_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM),
    acceleration_structure_cache(GetDescriptorSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)),
    sampler_descriptors(VulkanDevice::bindless_sampler_count * GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER)),
    descriptor_allocator(
        GetDescriptorCapacity(_device),
        GetDescriptorBlockSize(_device),
        GetStorageAlignment(),
        std::max<uint64>(GetStorageAlignment(), _device.GetCoreProperties().core_1_0.limits.nonCoherentAtomSize)
    ) {
    if (_device.GetDescriptorMode() == VulkanDescriptorMode::Sets)
        return;
    InitializeSamplerDescriptors();
    if (_device.GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        CreateNativeHeaps();
        MapDescriptorBuffer();
        return;
    }
    InitializeSamplerSetLayout();
    CreateDescriptorBuffer();
    // Map last: no potentially throwing initialization remains after success.
    MapDescriptorBuffer();
}

void VulkanDescriptorHeap::InitializeSamplerSetLayout() {
    VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_SAMPLER, VulkanDevice::bindless_sampler_count, VK_SHADER_STAGE_ALL
    };
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                           VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
    };
    binding_flags.bindingCount  = 1;
    binding_flags.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    info.pNext        = &binding_flags;
    info.bindingCount = 1;
    info.pBindings    = &binding;
    VkDescriptorSetLayout layout;
    VK_CHECK_RESULT(vkCreateDescriptorSetLayout(m_device->GetDevice(), &info, nullptr, &layout));
    auto release = OnScopeExit([&] {
        vkDestroyDescriptorSetLayout(m_device->GetDevice(), layout, nullptr);
    });
    vkGetDescriptorSetLayoutSizeEXT(m_device->GetDevice(), layout, &bindless_sampler_set_size);
    vkGetDescriptorSetLayoutBindingOffsetEXT(
        m_device->GetDevice(), layout, 0, &bindless_sampler_binding_offset
    );
}

void VulkanDescriptorHeap::InitializeSamplerDescriptors() {
    const uint sampler_descriptor_size = GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER);
    for (uint32_t i = 0; i < VulkanDevice::bindless_sampler_count; ++i) {
        const uint source      = i < m_device->ImmutableSamplerCount() ? i : 0;
        auto*      destination = sampler_descriptors.data() + i * sampler_descriptor_size;
        if (m_device->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
            VkHostAddressRangeEXT      range{destination, sampler_descriptor_size};
            const VkSamplerCreateInfo& info = m_device->GetSamplerCreateInfo(source);
            VK_CHECK_RESULT(
                m_device->GetDescriptorHeapFunctions().write_samplers(m_device->GetDevice(), 1, &info, &range)
            );
        } else {
            VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            VkSampler              sampler = m_device->GetImmutableSamplers()[source];
            desc_info.type                 = VK_DESCRIPTOR_TYPE_SAMPLER;
            desc_info.data.pSampler        = &sampler;
            vkGetDescriptorEXT(m_device->GetDevice(), &desc_info, sampler_descriptor_size, destination);
        }
    }
}

uint64 VulkanDescriptorHeap::GetStorageAlignment() const {
    if (m_device->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        const auto& p = m_device->GetOptionalProperties().descriptor_heap_properties;
        return std::max(p.imageDescriptorAlignment, p.bufferDescriptorAlignment);
    }
    return std::max<uint64>(
        1, m_device->GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment
    );
}

void VulkanDescriptorHeap::CreateNativeHeaps() {
    const auto&    p          = m_device->GetOptionalProperties().descriptor_heap_properties;
    const uint32_t families[] = {
        m_device->GetQueueFamilyIndex(EQueueType::Graphics),
        m_device->GetQueueFamilyIndex(EQueueType::Compute)
    };
    auto create = [&](std::string_view name, uint64 bytes, uint64 alignment) {
        return std::make_unique<VulkanBuffer>(
            name,
            BufferInfo(bytes, 1, EBufferUsageFlags::UNORDERED_ACCESS),
            *m_device,
            VulkanBufferAllocationInfo{
                .vk_usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .allocation_flags     = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .needs_device_address = true,
                .queue_family_indices = std::span(families, families[0] == families[1] ? 1 : 2),
                .min_alignment        = alignment,
            },
            false
        );
    };
    const uint64 resource_reserved = Moer::AlignUp(GetDescriptorCapacity(*m_device), GetStorageAlignment());
    const uint64 resource_size     = resource_reserved + p.minResourceHeapReservedRange;
    const uint64 sampler_reserved =
        Moer::AlignUp<uint64>(sampler_descriptors.size(), p.samplerDescriptorAlignment);
    const uint64 sampler_size = sampler_reserved + p.minSamplerHeapReservedRange;
    if (resource_size > p.maxResourceHeapSize || sampler_size > p.maxSamplerHeapSize) {
        throw std::runtime_error("Vulkan descriptor heap capacity exceeds device limits");
    }
    descriptor_buffer               = create("Vulkan resource heap", resource_size, p.resourceHeapAlignment);
    sampler_heap                    = create("Vulkan sampler heap", sampler_size, p.samplerHeapAlignment);
    resource_heap_binding.heapRange = {descriptor_buffer->DeviceAddress(), resource_size};
    resource_heap_binding.reservedRangeOffset = resource_reserved;
    resource_heap_binding.reservedRangeSize   = p.minResourceHeapReservedRange;
    sampler_heap_binding.heapRange            = {sampler_heap->DeviceAddress(), sampler_size};
    sampler_heap_binding.reservedRangeOffset  = sampler_reserved;
    sampler_heap_binding.reservedRangeSize    = p.minSamplerHeapReservedRange;
    void* mapped                              = nullptr;
    VK_CHECK_RESULT(vmaMapMemory(m_device->GetVmaAllocator(), sampler_heap->GetAllocation(), &mapped));
    memcpy(mapped, sampler_descriptors.data(), sampler_descriptors.size());
    const VkResult flushed = vmaFlushAllocation(
        m_device->GetVmaAllocator(), sampler_heap->GetAllocation(), 0, sampler_descriptors.size()
    );
    vmaUnmapMemory(m_device->GetVmaAllocator(), sampler_heap->GetAllocation());
    VK_CHECK_RESULT(flushed);
}

void VulkanDescriptorHeap::CreateDescriptorBuffer() {
    const uint32_t queue_families[] = {
        m_device->GetQueueFamilyIndex(EQueueType::Graphics),
        m_device->GetQueueFamilyIndex(EQueueType::Compute)
    };
    descriptor_buffer = std::make_unique<VulkanBuffer>(
        s_descriptor_buffer_name,
        BufferInfo(s_descriptor_buffer_capacity, 1, EBufferUsageFlags::UNORDERED_ACCESS),
        *m_device,
        VulkanBufferAllocationInfo{
            .vk_usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                        VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .needs_device_address = true,
            .queue_family_indices = std::span(queue_families, queue_families[0] == queue_families[1] ? 1 : 2),
        },
        false
    );
}

void VulkanDescriptorHeap::MapDescriptorBuffer() {
    void*          mapped_data = nullptr;
    const VkResult result =
        vmaMapMemory(m_device->GetVmaAllocator(), descriptor_buffer->GetAllocation(), &mapped_data);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to map descriptor buffer: " + std::to_string(result));
    }
    mapped_descriptor_data = static_cast<uint8*>(mapped_data);
}

VulkanDescriptorHeap::~VulkanDescriptorHeap() {
    if (mapped_descriptor_data) {
        vmaUnmapMemory(m_device->GetVmaAllocator(), descriptor_buffer->GetAllocation());
    }
}

uint VulkanDescriptorHeap::GetDescriptorSize(VkDescriptorType _type) const {
    if (m_device->GetDescriptorMode() == VulkanDescriptorMode::Sets)
        return 0;
    if (m_device->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        const auto& p = m_device->GetOptionalProperties().descriptor_heap_properties;
        switch (_type) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                return static_cast<uint>(p.samplerDescriptorSize);
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                return static_cast<uint>(p.imageDescriptorSize);
            default:
                return static_cast<uint>(p.bufferDescriptorSize);
        }
    }
    const auto& properties = m_device->GetOptionalProperties().descriptor_buffer_properties;
    const bool  robust     = m_device->GetCoreFeatures().GetCore10Features().robustBufferAccess == VK_TRUE;
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return static_cast<uint>(
                robust ? properties.robustStorageBufferDescriptorSize : properties.storageBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return static_cast<uint>(
                robust ? properties.robustUniformBufferDescriptorSize : properties.uniformBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return static_cast<uint>(
                robust ? properties.robustStorageTexelBufferDescriptorSize :
                         properties.storageTexelBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return static_cast<uint>(
                robust ? properties.robustUniformTexelBufferDescriptorSize :
                         properties.uniformTexelBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            return static_cast<uint>(properties.samplerDescriptorSize);
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            return static_cast<uint>(properties.sampledImageDescriptorSize);
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return static_cast<uint>(properties.storageImageDescriptorSize);
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return static_cast<uint>(properties.accelerationStructureDescriptorSize);
        default:
            throw std::invalid_argument("unsupported descriptor type");
    }
}

VkDeviceAddress VulkanDescriptorHeap::GetDescriptorBufferAddress() const {
    return descriptor_buffer ? descriptor_buffer->DeviceAddress() : 0;
}

VulkanCachedDescriptor VulkanDescriptorHeap::GetOrCreateBufferDescriptor(
    const BufferView& _buffer,
    VkDescriptorType  _type,
    VkFormat          _format
) {
    assert(_buffer.GetBuffer() != nullptr && "buffer is nullptr");
    VulkanBuffer* vk_buffer     = ResourceCast(_buffer.GetBuffer());
    VkFormat      buffer_format = VulkanEnumTranslator::METoVKFormat(_buffer.format);
    _format                     = buffer_format == VK_FORMAT_UNDEFINED ? _format : buffer_format;
    if (_type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
        _type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
        _format = VK_FORMAT_UNDEFINED;
    }

    const VulkanBufferDescriptorKey key{
        .byte_offset = _buffer.byte_offset,
        .byte_size   = _buffer.GetByteSize(),
        .format      = _format,
    };

    std::lock_guard<std::mutex> lock(cache_mutex);
    auto&                       cache_slots = vk_buffer->GetDescriptorCacheSlots(_type);

    auto cached = cache_slots.find(key);
    if (cached != cache_slots.end()) {
        return {_type, cached->second};
    }
    VkDescriptorAddressInfoEXT buffer_info{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    buffer_info.address = vk_buffer->DeviceAddress() + _buffer.byte_offset;
    buffer_info.range   = _buffer.GetByteSize();
    VkDescriptorGetInfoEXT buffer_desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    buffer_desc_info.type = _type;
    const uint desc_size  = GetDescriptorSize(_type);
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            buffer_info.format                        = _format;
            buffer_desc_info.data.pStorageTexelBuffer = &buffer_info;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            buffer_desc_info.data.pStorageBuffer = &buffer_info;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            buffer_desc_info.data.pUniformBuffer = &buffer_info;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            buffer_info.format                        = _format;
            buffer_desc_info.data.pUniformTexelBuffer = &buffer_info;
            break;
        default:
            throw std::invalid_argument("unsupported buffer descriptor type");
    }
    const uint slot_index = buffer_cache.AllocateSlot();

    bool published = false;
    auto rollback  = OnScopeExit([&] {
        if (!published)
            buffer_cache.free_slots.push_back(slot_index);
    });

    void* destination = buffer_cache.bytes.data() + slot_index * buffer_cache.slot_stride;
    if (m_device->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        VkResourceDescriptorInfoEXT info{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
        info.type = _type;
        VkDeviceAddressRangeEXT        address{buffer_info.address, buffer_info.range};
        VkTexelBufferDescriptorInfoEXT texel{VK_STRUCTURE_TYPE_TEXEL_BUFFER_DESCRIPTOR_INFO_EXT};
        texel.format       = _format;
        texel.addressRange = address;
        if (_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            _type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
            info.data.pTexelBuffer = &texel;
        else
            info.data.pAddressRange = &address;
        VkHostAddressRangeEXT range{destination, desc_size};
        VK_CHECK_RESULT(
            m_device->GetDescriptorHeapFunctions().write_resources(m_device->GetDevice(), 1, &info, &range)
        );
    } else {
        vkGetDescriptorEXT(m_device->GetDevice(), &buffer_desc_info, desc_size, destination);
    }
    cache_slots.emplace(key, slot_index);
    buffer_descriptor_types[slot_index] = _type;
    published                           = true;
    return {_type, slot_index};
}

VulkanCachedDescriptor VulkanDescriptorHeap::GetOrCreateImageDescriptor(
    const TextureView* _image,
    VkImageLayout      _layout,
    VkDescriptorType   _descriptor_type
) {
    auto* texture = ResourceCast(_image->texture);
    assert(texture != nullptr && "texture is nullptr");
    if (_descriptor_type == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
        _descriptor_type = _layout == VK_IMAGE_LAYOUT_GENERAL ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                                                                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    }
    assert(
        (_descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
         _descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) &&
        "unsupported image descriptor type"
    );

    const uint8                array_count = _image->num_array == 0 ? 1 : _image->num_array;
    VulkanTextureDescriptorKey key{
        _descriptor_type,
        _layout,
        _image->mip_level,
        _image->num_mips,
        _image->array_layer,
        array_count,
    };

    // Image-view creation owns a texture-local mutex. Resolve it before the
    // descriptor heap lock so texture destruction never observes the reverse
    // heap->view / view->heap lock order.
    const VkImageView image_view =
        texture->GetView(_image->mip_level, _image->num_mips, _image->array_layer, array_count);

    std::lock_guard<std::mutex> lock(cache_mutex);
    const auto                  cached = texture->m_descriptor_cache_slots.find(key);
    if (cached != texture->m_descriptor_cache_slots.end()) {
        return {_descriptor_type, cached->second};
    }
    VkDescriptorImageInfo  image_info{.imageView = image_view, .imageLayout = _layout};
    VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    desc_info.type = _descriptor_type;
    if (_descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
        desc_info.data.pStorageImage = &image_info;
    } else {
        desc_info.data.pSampledImage = &image_info;
    }
    const uint slot_index = image_cache.AllocateSlot();
    bool       published   = false;
    auto       rollback    = OnScopeExit([&] {
        if (!published)
            image_cache.free_slots.push_back(slot_index);
    });
    void*      destination = image_cache.bytes.data() + slot_index * image_cache.slot_stride;
    if (m_device->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image            = texture->GetHandle();
        view.viewType         = texture->GetDimension() == ETextureDimension::TEX_CUBE && array_count == 1 ?
                                    VK_IMAGE_VIEW_TYPE_2D :
                                    VulkanEnumTranslator::METoVKImageViewType(texture->GetDimension());
        view.format           = VulkanEnumTranslator::METoVKFormat(texture->GetFormat());
        view.subresourceRange = {
            VulkanEnumTranslator::METoVKImageAspectFlags(texture->GetAspectFlags()),
            _image->mip_level,
            _image->num_mips,
            _image->array_layer,
            array_count
        };
        VkImageDescriptorInfoEXT image{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
        image.pView  = &view;
        image.layout = _layout;
        VkResourceDescriptorInfoEXT info{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
        info.type        = _descriptor_type;
        info.data.pImage = &image;
        VkHostAddressRangeEXT range{destination, GetDescriptorSize(_descriptor_type)};
        VK_CHECK_RESULT(
            m_device->GetDescriptorHeapFunctions().write_resources(m_device->GetDevice(), 1, &info, &range)
        );
    } else {
        vkGetDescriptorEXT(
            m_device->GetDevice(), &desc_info, GetDescriptorSize(_descriptor_type), destination
        );
    }
    texture->m_descriptor_cache_slots.emplace(key, slot_index);
    image_descriptor_types[slot_index] = _descriptor_type;
    published                          = true;
    return {_descriptor_type, slot_index};
}

VulkanCachedDescriptor VulkanDescriptorHeap::GetOrCreateAccelerationStructureDescriptor(
    VulkanAccelerationStructure* _acceleration_structure
) {
    assert(_acceleration_structure != nullptr && "accel struct is nullptr");
    assert(_acceleration_structure->handle != VK_NULL_HANDLE && "accel struct handle is null");
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (_acceleration_structure->m_descriptor_cache_slot >= 0) {
        return {
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            static_cast<uint>(_acceleration_structure->m_descriptor_cache_slot)
        };
    }
    VkAccelerationStructureDeviceAddressInfoKHR address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };
    address_info.accelerationStructure = _acceleration_structure->handle;
    VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    desc_info.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    desc_info.data.accelerationStructure =
        vkGetAccelerationStructureDeviceAddressKHR(m_device->GetDevice(), &address_info);
    const uint slot_index                            = acceleration_structure_cache.AllocateSlot();
    bool       published                             = false;
    auto       rollback                              = OnScopeExit([&] {
        if (!published)
            acceleration_structure_cache.free_slots.push_back(slot_index);
    });
    void*      destination =
        acceleration_structure_cache.bytes.data() + slot_index * acceleration_structure_cache.slot_stride;
    if (m_device->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        VkDeviceAddressRangeEXT     address{desc_info.data.accelerationStructure, 0};
        VkResourceDescriptorInfoEXT info{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
        info.type               = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        info.data.pAddressRange = &address;
        VkHostAddressRangeEXT range{destination, acceleration_structure_cache.slot_stride};
        VK_CHECK_RESULT(
            m_device->GetDescriptorHeapFunctions().write_resources(m_device->GetDevice(), 1, &info, &range)
        );
    } else {
        vkGetDescriptorEXT(
            m_device->GetDevice(), &desc_info, acceleration_structure_cache.slot_stride, destination
        );
    }

    _acceleration_structure->m_descriptor_cache_slot = slot_index;
    published                                        = true;
    return {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, slot_index};
}

VulkanCachedDescriptor VulkanDescriptorHeap::GetSamplerDescriptor(Sampler _sampler) const {
    return {VK_DESCRIPTOR_TYPE_SAMPLER, m_device->GetSamplerIndex(_sampler)};
}

void VulkanDescriptorHeap::FreeBufferDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_slot_index < buffer_descriptor_types.size() && "buffer descriptor index is invalid");
    buffer_descriptor_types[_slot_index] = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    buffer_cache.free_slots.push_back(_slot_index);
}

void VulkanDescriptorHeap::FreeImageDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_slot_index < image_descriptor_types.size() && "image descriptor index is invalid");
    image_descriptor_types[_slot_index] = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    image_cache.free_slots.push_back(_slot_index);
}

void VulkanDescriptorHeap::FreeAccelerationStructureDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    acceleration_structure_cache.free_slots.push_back(_slot_index);
}

void VulkanDescriptorHeap::CopyDescriptorToMemory(
    VulkanCachedDescriptor _descriptor,
    std::span<byte>        _destination
) {
    std::lock_guard lock(cache_mutex);
    const auto      bytes = GetCachedDescriptor(_descriptor);
    if (_destination.size() < bytes.size()) {
        throw std::out_of_range("descriptor copy destination is too small");
    }
    memcpy(_destination.data(), bytes.data(), bytes.size());
}

void VulkanDescriptorHeap::CopyDescriptorToBuffer(
    VulkanCachedDescriptor _descriptor,
    uint64                 _destination_byte_offset
) {
    std::lock_guard lock(cache_mutex);
    const auto      bytes    = GetCachedDescriptor(_descriptor);
    const uint64    capacity = descriptor_buffer->GetByteSize();
    if (_destination_byte_offset > capacity || bytes.size() > capacity - _destination_byte_offset) {
        throw std::out_of_range("descriptor copy exceeds GPU descriptor buffer capacity");
    }
    memcpy(mapped_descriptor_data + _destination_byte_offset, bytes.data(), bytes.size());
}

VulkanDescriptorBufferLease VulkanDescriptorHeap::CreateDescriptorBufferLease() {
    return descriptor_allocator.CreateLease();
}

void VulkanDescriptorHeap::FlushDescriptorWrites(const VulkanDescriptorBufferLease& _lease) {
    assert(_lease != nullptr && "descriptor buffer lease is invalid");
    // Recorders have joined. Never flush across gaps owned by another batch:
    // a non-coherent flush counts as a write to every touched memory atom.
    for (const auto& range : descriptor_allocator.GetWrittenRanges(*_lease)) {
        const VkResult result = vmaFlushAllocation(
            m_device->GetVmaAllocator(),
            descriptor_buffer->GetAllocation(),
            range.byte_offset,
            range.byte_size
        );
        if (result != VK_SUCCESS) {
            throw std::runtime_error("failed to flush descriptor writes: " + std::to_string(result));
        }
    }
}

void VulkanDescriptorHeap::RecycleDescriptorBufferLease(VulkanDescriptorBufferLease _lease) {
    if (_lease != nullptr) {
        descriptor_allocator.Release(*_lease);
    }
}

std::optional<uint64>
VulkanDescriptorHeap::ReserveDescriptorRange(const VulkanDescriptorBufferLease& _lease, uint64 _byte_size) {
    assert(_lease != nullptr && "descriptor range reservation has no submission lease");
    if (!_lease || m_device->IsFaulted()) {
        return std::nullopt;
    }
    return descriptor_allocator.ReserveRange(*_lease, _byte_size);
}

uint64 VulkanDescriptorHeap::GetUsedDescriptorBytes(const VulkanDescriptorBufferLease& _lease) const {
    assert(_lease != nullptr && "descriptor usage query has no submission lease");
    return descriptor_allocator.GetUsage(*_lease).used_bytes;
}

std::optional<std::vector<VulkanDescriptorBufferAllocator::Recording>>
VulkanDescriptorHeap::PrepareDescriptorRecordings(
    const VulkanDescriptorBufferLease& _lease,
    std::span<const std::vector<uint64_t>> _requests
) {
    if (!_lease || m_device->IsFaulted()) {
        return std::nullopt;
    }
    return descriptor_allocator.PrepareRecordings(*_lease, _requests);
}

std::span<const byte> VulkanDescriptorHeap::GetCachedDescriptor(VulkanCachedDescriptor _descriptor) const {
    const uint descriptor_size = GetDescriptorSize(_descriptor.type);
    if (_descriptor.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
        if (descriptor_size == 0 || _descriptor.slot_index >= sampler_descriptors.size() / descriptor_size) {
            throw std::out_of_range("sampler descriptor slot is invalid");
        }
        return {sampler_descriptors.data() + _descriptor.slot_index * descriptor_size, descriptor_size};
    }

    const DescriptorCache*            cache = nullptr;
    std::span<const VkDescriptorType> types;
    switch (_descriptor.type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            cache = &buffer_cache;
            types = buffer_descriptor_types;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            cache = &image_cache;
            types = image_descriptor_types;
            break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            cache = &acceleration_structure_cache;
            break;
        default:
            throw std::invalid_argument("unsupported cached descriptor type");
    }
    if (_descriptor.slot_index >= cache->next_slot || descriptor_size == 0 ||
        descriptor_size > cache->slot_stride) {
        throw std::out_of_range("descriptor cache slot is invalid");
    }
    if (!types.empty() && types[_descriptor.slot_index] != _descriptor.type) {
        throw std::invalid_argument("descriptor cache slot is empty or has a different type");
    }
    return {cache->bytes.data() + _descriptor.slot_index * cache->slot_stride, descriptor_size};
}

} // namespace Moer::Render
