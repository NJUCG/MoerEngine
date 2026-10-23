#include "VulkanEnumTranslator.h"
#include "VulkanRaytracingResource.h"
#include "VulkanDescriptor.h"

#include "VulkanDevice.h"
#include "VulkanMemoryAllocator.h"
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
    const auto& properties = device.GetOptionalProperties().descriptor_buffer_properties;
    const uint64 sampler_bytes =
        static_cast<uint64>(properties.samplerDescriptorSize) * VulkanDevice::bindless_sampler_count;
    return Moer::AlignUp(sampler_bytes, properties.descriptorBufferOffsetAlignment);
}

namespace {

static constexpr std::string_view s_descriptor_buffer_name         = "VulkanDescriptorHeap::DescriptorBuffer";
static constexpr uint32           s_descriptor_cache_slot_capacity = 10086;
static constexpr uint64           s_descriptor_buffer_capacity     = 6 * 1024 * 1024;
static constexpr uint64           s_descriptor_block_size          = 64 * 1024;

uint64 GetDescriptorBlockSize(const VulkanDevice& _device) {
    const uint64 alignment = std::max<uint64>(
        _device.GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment,
        _device.GetCoreProperties().core_1_0.limits.nonCoherentAtomSize
    );
    return Moer::AlignUp(s_descriptor_block_size, alignment);
}

} // namespace

VulkanDescriptorHeap::DescriptorCache::DescriptorCache(uint _slot_stride) :
    slot_stride(_slot_stride),
    bytes(static_cast<size_t>(_slot_stride) * s_descriptor_cache_slot_capacity) {}

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
    buffer_cache(
        std::max(
            {GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
             GetDescriptorSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
             GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER),
             GetDescriptorSize(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)}
        )
    ),
    buffer_descriptor_types(s_descriptor_cache_slot_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM),
    image_cache(
        std::max(
            GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        )
    ),
    image_descriptor_types(s_descriptor_cache_slot_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM),
    acceleration_structure_cache(GetDescriptorSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)),
    sampler_descriptors(VulkanDevice::bindless_sampler_count * GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER)),
    descriptor_allocator(
        s_descriptor_buffer_capacity,
        GetDescriptorBlockSize(_device),
        _device.GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment
    ) {
    InitializeSamplerDescriptors();
    CreateDescriptorBuffer();
    // Map last: no potentially throwing initialization remains after success.
    MapDescriptorBuffer();
}

void VulkanDescriptorHeap::InitializeSamplerDescriptors() {
    const uint sampler_descriptor_size = GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER);
    for (uint32_t i = 0; i < VulkanDevice::bindless_sampler_count; ++i) {
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
            sampler_descriptor_size,
            sampler_descriptors.data() + i * sampler_descriptor_size
        );
    }
}

void VulkanDescriptorHeap::CreateDescriptorBuffer() {
    VkBuffer           desc_buffer            = VK_NULL_HANDLE;
    VmaAllocation      desc_buffer_allocation = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_ci.size  = s_descriptor_buffer_capacity;
    buffer_ci.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    const uint32_t queue_families[] = {
        m_device->GetQueueFamilyIndex(EQueueType::Graphics),
        m_device->GetQueueFamilyIndex(EQueueType::Compute)
    };
    if (queue_families[0] != queue_families[1]) {
        buffer_ci.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        buffer_ci.queueFamilyIndexCount = 2;
        buffer_ci.pQueueFamilyIndices   = queue_families;
    }

#if WITH_CUDA
    const auto external_memory_info = MakeExternalMemoryBufferCreateInfo();
    buffer_ci.pNext = &external_memory_info;
#endif

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.flags        = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    alloc_ci.usage        = VMA_MEMORY_USAGE_AUTO;
    const VkResult result = vmaCreateBuffer(
        m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &desc_buffer, &desc_buffer_allocation, nullptr
    );
    if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to create descriptor buffer: " + std::to_string(result));
    }
    // Release the native allocation if C++ wrapper construction fails.
    auto release_unowned_buffer = OnScopeExit([&] {
        if (desc_buffer_allocation != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_device->GetVmaAllocator(), desc_buffer, desc_buffer_allocation);
        }
    });

    BufferInfo buffer_info{};
    buffer_info.size   = buffer_ci.size;
    buffer_info.stride = 1;
    buffer_info.usage  = EBufferUsageFlags::UNORDERED_ACCESS;

    descriptor_buffer = std::make_unique<VulkanBuffer>(
        s_descriptor_buffer_name, buffer_info, *m_device, desc_buffer, desc_buffer_allocation, false, true
    );
    desc_buffer_allocation = VK_NULL_HANDLE;
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
    return descriptor_buffer->DeviceAddress();
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

    cache_slots[key]                    = slot_index;
    buffer_descriptor_types[slot_index] = _type;

    vkGetDescriptorEXT(
        m_device->GetDevice(),
        &buffer_desc_info,
        desc_size,
        buffer_cache.bytes.data() + slot_index * buffer_cache.slot_stride
    );
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
    texture->m_descriptor_cache_slots.emplace(key, slot_index);
    image_descriptor_types[slot_index] = _descriptor_type;
    vkGetDescriptorEXT(
        m_device->GetDevice(),
        &desc_info,
        GetDescriptorSize(_descriptor_type),
        image_cache.bytes.data() + slot_index * image_cache.slot_stride
    );
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
    _acceleration_structure->m_descriptor_cache_slot = slot_index;
    vkGetDescriptorEXT(
        m_device->GetDevice(),
        &desc_info,
        acceleration_structure_cache.slot_stride,
        acceleration_structure_cache.bytes.data() + slot_index * acceleration_structure_cache.slot_stride
    );

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

VulkanDescriptorAllocationCheckpoint
VulkanDescriptorHeap::CreateDescriptorCheckpoint(const VulkanDescriptorBufferLease& _lease) const {
    assert(_lease != nullptr && "descriptor checkpoint has no submission lease");
    return descriptor_allocator.CreateCheckpoint(*_lease);
}

void VulkanDescriptorHeap::RewindDescriptorWrites(
    const VulkanDescriptorBufferLease&          _lease,
    const VulkanDescriptorAllocationCheckpoint& _checkpoint
) {
    assert(_lease != nullptr && "descriptor rewind has no submission lease");
    descriptor_allocator.Rewind(*_lease, _checkpoint);
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
