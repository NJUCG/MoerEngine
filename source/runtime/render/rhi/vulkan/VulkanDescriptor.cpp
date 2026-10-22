#include "VulkanDescriptor.h"

#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanPlatform.h"
#include "VulkanRHIResource.h"
#include "VulkanUtil.h"

#include "misc/MacroUtils.h"
#include "misc/RAII.h"
#include "rhi/RHIResource.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Moer::Render {
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

static uint GetBufferDescriptorSize(const VulkanDevice& _device, VkDescriptorType _type) {
    const auto& properties = _device.GetOptionalProperties().descriptor_buffer_properties;
    const bool  robust_buffer_access =
        _device.GetCoreFeatures().GetCore10Features().robustBufferAccess == VK_TRUE;
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return static_cast<uint>(
                robust_buffer_access ? properties.robustStorageBufferDescriptorSize :
                                       properties.storageBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return static_cast<uint>(
                robust_buffer_access ? properties.robustUniformBufferDescriptorSize :
                                       properties.uniformBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return static_cast<uint>(
                robust_buffer_access ? properties.robustStorageTexelBufferDescriptorSize :
                                       properties.storageTexelBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return static_cast<uint>(
                robust_buffer_access ? properties.robustUniformTexelBufferDescriptorSize :
                                       properties.uniformTexelBufferDescriptorSize
            );
        default:
            throw std::runtime_error("unsupported buffer descriptor type");
    }
}

uint AllocateDescriptorCacheSlot(
    Array<uint>&     _free_slots,
    uint64&          _next_byte_offset,
    uint             _slot_stride,
    size_t           _capacity,
    std::string_view _resource_kind
) {
    if (!_free_slots.empty()) {
        const uint slot_index = _free_slots.back();
        _free_slots.pop_back();
        if (slot_index >= _capacity) {
            throw std::runtime_error(std::string(_resource_kind) + " descriptor free-list index is invalid");
        }
        return slot_index;
    }
    const uint slot_index = _next_byte_offset / _slot_stride;
    if (slot_index >= _capacity) {
        throw std::runtime_error(std::string(_resource_kind) + " descriptor cache exhausted");
    }
    _next_byte_offset += _slot_stride;
    return slot_index;
}

void CopyDescriptorBytes(std::span<const byte> _descriptor, std::span<byte> _destination) {
    if (_destination.size() < _descriptor.size()) {
        throw std::out_of_range("descriptor copy destination is too small");
    }
    memcpy(_destination.data(), _descriptor.data(), _descriptor.size());
}

} // namespace

VulkanDescriptorHeap::VulkanDescriptorHeap(VulkanDevice& _device) :
    m_device(&_device),
    storage_buffer_descriptor_size(GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)),
    uniform_buffer_descriptor_size(GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)),
    storage_texel_buffer_descriptor_size(
        GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
    ),
    uniform_texel_buffer_descriptor_size(
        GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
    ),
    sampled_image_descriptor_size(
        _device.GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize
    ),
    storage_image_descriptor_size(
        _device.GetOptionalProperties().descriptor_buffer_properties.storageImageDescriptorSize
    ),
    sampler_descriptor_size(
        _device.GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize
    ),
    acceleration_structure_descriptor_size(
        _device.GetOptionalProperties().descriptor_buffer_properties.accelerationStructureDescriptorSize
    ),
    buffer_cache_slot_stride(
        std::max(
            {storage_buffer_descriptor_size,
             uniform_buffer_descriptor_size,
             storage_texel_buffer_descriptor_size,
             uniform_texel_buffer_descriptor_size}
        )
    ),
    image_cache_slot_stride(std::max(sampled_image_descriptor_size, storage_image_descriptor_size)),
    image_cache_base_byte_offset(sampler_descriptor_size * VulkanDevice::bindless_sampler_count),
    descriptor_allocator(
        s_descriptor_buffer_capacity,
        GetDescriptorBlockSize(_device),
        _device.GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment
    ) {

    InitializeDescriptorCaches();
    InitializeSamplerDescriptors();
    CreateDescriptorBuffer();
    // Map last: no potentially throwing initialization remains after success.
    MapDescriptorBuffer();
}

void VulkanDescriptorHeap::InitializeDescriptorCaches() {
    buffer_descriptor_cache.resize(s_descriptor_cache_slot_capacity * buffer_cache_slot_stride);
    sampler_and_image_descriptor_cache.resize(
        image_cache_base_byte_offset + s_descriptor_cache_slot_capacity * image_cache_slot_stride
    );
    acceleration_structure_descriptor_cache.resize(
        s_descriptor_cache_slot_capacity * acceleration_structure_descriptor_size
    );
    buffer_descriptor_types.resize(s_descriptor_cache_slot_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM);
    image_descriptor_types.resize(s_descriptor_cache_slot_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM);
}

void VulkanDescriptorHeap::InitializeSamplerDescriptors() {
    // Store immutable sampler descriptors before the image cache.
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
            sampler_and_image_descriptor_cache.data() + i * sampler_descriptor_size
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
    buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
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
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            return sampler_descriptor_size;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            return sampled_image_descriptor_size;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return storage_image_descriptor_size;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return uniform_texel_buffer_descriptor_size;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return storage_texel_buffer_descriptor_size;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return uniform_buffer_descriptor_size;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return storage_buffer_descriptor_size;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return acceleration_structure_descriptor_size;
        default:
            LOG_ERROR("Unsupported descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, _type));
            assert(false && "unsupported descriptor type");
            return 0;
    }
}

VkDeviceAddress VulkanDescriptorHeap::GetDescriptorBufferAddress() const {
    return descriptor_buffer->DeviceAddress();
}

uint VulkanDescriptorHeap::GetOrCreateBufferDescriptorOffset(
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
    uint                        slot_index  = 0;
    auto&                       cache_slots = vk_buffer->GetDescriptorCacheSlots(_type);

    auto cached = cache_slots.find(key);
    if (cached != cache_slots.end()) {
        return cached->second * buffer_cache_slot_stride;
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
            LOG_ERROR("Unsupported buffer descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, _type));
            assert(false && "Unsupported buffer descriptor type");
            return 0;
    }
    slot_index = AllocateDescriptorCacheSlot(
        free_buffer_slots,
        next_buffer_cache_byte_offset,
        buffer_cache_slot_stride,
        buffer_descriptor_types.size(),
        "buffer"
    );

    cache_slots[key]                    = slot_index;
    buffer_descriptor_types[slot_index] = _type;

    vkGetDescriptorEXT(
        m_device->GetDevice(),
        &buffer_desc_info,
        desc_size,
        buffer_descriptor_cache.data() + slot_index * buffer_cache_slot_stride
    );
    return slot_index * buffer_cache_slot_stride;
}

uint VulkanDescriptorHeap::GetOrCreateImageDescriptorOffset(
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
        return cached->second * image_cache_slot_stride + image_cache_base_byte_offset;
    }
    uint                   slot_index = 0;
    VkDescriptorImageInfo  image_info{.imageView = image_view, .imageLayout = _layout};
    VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    desc_info.type = _descriptor_type;
    if (_descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
        desc_info.data.pStorageImage = &image_info;
    } else {
        desc_info.data.pSampledImage = &image_info;
    }
    slot_index = AllocateDescriptorCacheSlot(
        free_image_slots,
        next_image_cache_byte_offset,
        image_cache_slot_stride,
        image_descriptor_types.size(),
        "image"
    );
    texture->m_descriptor_cache_slots.emplace(key, slot_index);
    image_descriptor_types[slot_index] = _descriptor_type;
    vkGetDescriptorEXT(
        m_device->GetDevice(),
        &desc_info,
        GetDescriptorSize(_descriptor_type),
        sampler_and_image_descriptor_cache.data() + slot_index * image_cache_slot_stride +
            image_cache_base_byte_offset
    );
    return slot_index * image_cache_slot_stride + image_cache_base_byte_offset;
}

uint VulkanDescriptorHeap::GetOrCreateAccelerationStructureDescriptorOffset(
    VulkanAccelerationStructure* _acceleration_structure
) {
    assert(_acceleration_structure != nullptr && "accel struct is nullptr");
    assert(_acceleration_structure->handle != VK_NULL_HANDLE && "accel struct handle is null");
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (_acceleration_structure->m_descriptor_cache_slot >= 0) {
        return _acceleration_structure->m_descriptor_cache_slot * acceleration_structure_descriptor_size;
    }
    VkAccelerationStructureDeviceAddressInfoKHR address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };
    address_info.accelerationStructure = _acceleration_structure->handle;
    VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    desc_info.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    desc_info.data.accelerationStructure =
        vkGetAccelerationStructureDeviceAddressKHR(m_device->GetDevice(), &address_info);
    uint slot_index = 0;
    slot_index      = AllocateDescriptorCacheSlot(
        free_acceleration_structure_slots,
        next_acceleration_structure_cache_byte_offset,
        acceleration_structure_descriptor_size,
        acceleration_structure_descriptor_cache.size() / acceleration_structure_descriptor_size,
        "acceleration-structure"
    );
    _acceleration_structure->m_descriptor_cache_slot = slot_index;
    vkGetDescriptorEXT(
        m_device->GetDevice(),
        &desc_info,
        acceleration_structure_descriptor_size,
        acceleration_structure_descriptor_cache.data() + slot_index * acceleration_structure_descriptor_size
    );

    return slot_index * acceleration_structure_descriptor_size;
}

uint VulkanDescriptorHeap::GetSamplerDescriptorOffset(Sampler _sampler) const {
    return m_device->GetSamplerIndex(_sampler) * sampler_descriptor_size;
}

void VulkanDescriptorHeap::FreeBufferDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_slot_index < buffer_descriptor_types.size() && "buffer descriptor index is invalid");
    buffer_descriptor_types[_slot_index] = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    free_buffer_slots.push_back(_slot_index);
}

void VulkanDescriptorHeap::FreeImageDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_slot_index < image_descriptor_types.size() && "image descriptor index is invalid");
    image_descriptor_types[_slot_index] = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    free_image_slots.push_back(_slot_index);
}

void VulkanDescriptorHeap::FreeAccelerationStructureDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    free_acceleration_structure_slots.push_back(_slot_index);
}

void VulkanDescriptorHeap::CopyBufferDescriptorToMemory(
    uint64          _source_byte_offset,
    std::span<byte> _destination
) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    CopyDescriptorBytes(GetCachedBufferDescriptor(_source_byte_offset), _destination);
}

void VulkanDescriptorHeap::CopyImageDescriptorToMemory(
    uint64          _source_byte_offset,
    std::span<byte> _destination
) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    CopyDescriptorBytes(GetCachedImageDescriptor(_source_byte_offset), _destination);
}

void VulkanDescriptorHeap::CopyBufferDescriptorToBuffer(
    uint64 _source_byte_offset,
    uint64 _destination_byte_offset
) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    CopyDescriptorToBuffer(GetCachedBufferDescriptor(_source_byte_offset), _destination_byte_offset);
}

void VulkanDescriptorHeap::CopyImageDescriptorToBuffer(
    uint64 _source_byte_offset,
    uint64 _destination_byte_offset
) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    CopyDescriptorToBuffer(GetCachedImageDescriptor(_source_byte_offset), _destination_byte_offset);
}

void VulkanDescriptorHeap::CopySamplerDescriptorToBuffer(
    uint64 _source_byte_offset,
    uint64 _destination_byte_offset
) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (sampler_descriptor_size == 0 || _source_byte_offset % sampler_descriptor_size != 0 ||
        _source_byte_offset >= image_cache_base_byte_offset) {
        throw std::out_of_range("sampler descriptor cache offset is invalid");
    }
    CopyDescriptorToBuffer(
        {sampler_and_image_descriptor_cache.data() + _source_byte_offset, sampler_descriptor_size},
        _destination_byte_offset
    );
}

void VulkanDescriptorHeap::CopyAccelerationStructureDescriptorToBuffer(
    uint64 _source_byte_offset,
    uint64 _destination_byte_offset
) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (acceleration_structure_descriptor_size == 0 ||
        _source_byte_offset % acceleration_structure_descriptor_size != 0 ||
        _source_byte_offset >= next_acceleration_structure_cache_byte_offset) {
        throw std::out_of_range("acceleration-structure descriptor cache offset is invalid");
    }
    CopyDescriptorToBuffer(
        {acceleration_structure_descriptor_cache.data() + _source_byte_offset,
         acceleration_structure_descriptor_size},
        _destination_byte_offset
    );
}

VulkanDescriptorBufferLease VulkanDescriptorHeap::CreateDescriptorBufferLease() {
    return {descriptor_allocator.CreateLease()};
}

void VulkanDescriptorHeap::FlushDescriptorWrites(const VulkanDescriptorBufferLease& _lease) {
    assert(_lease.IsValid() && "descriptor buffer lease is invalid");
    // Recorders have joined. Never flush across gaps owned by another batch:
    // a non-coherent flush counts as a write to every touched memory atom.
    for (const auto& range : descriptor_allocator.GetWrittenRanges(*_lease.state)) {
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
    if (_lease.IsValid()) {
        descriptor_allocator.Release(*_lease.state);
    }
}

std::optional<uint64> VulkanDescriptorHeap::ReserveDescriptorRange(
    const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease,
    uint64                                                   _byte_size
) {
    assert(_lease != nullptr && "descriptor range reservation has no submission lease");
    if (!_lease || m_device->IsFaulted()) {
        return std::nullopt;
    }
    return descriptor_allocator.ReserveRange(*_lease, _byte_size);
}

uint64 VulkanDescriptorHeap::GetUsedDescriptorBytes(
    const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease
) const {
    assert(_lease != nullptr && "descriptor usage query has no submission lease");
    return descriptor_allocator.GetUsage(*_lease).used_bytes;
}

VulkanDescriptorAllocationCheckpoint VulkanDescriptorHeap::CreateDescriptorCheckpoint(
    const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease
) const {
    assert(_lease != nullptr && "descriptor checkpoint has no submission lease");
    return descriptor_allocator.CreateCheckpoint(*_lease);
}

void VulkanDescriptorHeap::RewindDescriptorWrites(
    const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease,
    const VulkanDescriptorAllocationCheckpoint&              _checkpoint
) {
    assert(_lease != nullptr && "descriptor rewind has no submission lease");
    descriptor_allocator.Rewind(*_lease, _checkpoint);
}

std::span<const byte> VulkanDescriptorHeap::GetCachedBufferDescriptor(uint64 _byte_offset) const {
    if (buffer_cache_slot_stride == 0 || _byte_offset % buffer_cache_slot_stride != 0 ||
        _byte_offset / buffer_cache_slot_stride >= buffer_descriptor_types.size()) {
        throw std::out_of_range("buffer descriptor cache offset is invalid");
    }
    const VkDescriptorType type = buffer_descriptor_types[_byte_offset / buffer_cache_slot_stride];
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return {buffer_descriptor_cache.data() + _byte_offset, GetDescriptorSize(type)};
        default:
            throw std::invalid_argument("buffer descriptor cache slot is not populated");
    }
}

std::span<const byte> VulkanDescriptorHeap::GetCachedImageDescriptor(uint64 _byte_offset) const {
    if (_byte_offset < image_cache_base_byte_offset || image_cache_slot_stride == 0) {
        throw std::out_of_range("image descriptor cache offset is invalid");
    }
    const uint64 relative_offset = _byte_offset - image_cache_base_byte_offset;
    if (relative_offset % image_cache_slot_stride != 0 ||
        relative_offset / image_cache_slot_stride >= image_descriptor_types.size()) {
        throw std::out_of_range("image descriptor cache offset is invalid");
    }
    const VkDescriptorType type = image_descriptor_types[relative_offset / image_cache_slot_stride];
    if (type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
        throw std::invalid_argument("image descriptor cache slot is not populated");
    }
    return {sampler_and_image_descriptor_cache.data() + _byte_offset, GetDescriptorSize(type)};
}

void VulkanDescriptorHeap::CopyDescriptorToBuffer(std::span<const byte> _descriptor, uint64 _byte_offset) {
    const uint64 capacity = descriptor_buffer->GetByteSize();
    if (_byte_offset > capacity || _descriptor.size() > capacity - _byte_offset) {
        throw std::out_of_range("descriptor copy exceeds GPU descriptor buffer capacity");
    }
    memcpy(mapped_descriptor_data + _byte_offset, _descriptor.data(), _descriptor.size());
}

} // namespace Moer::Render
