#include "VulkanDescriptor.h"

#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanPlatform.h"
#include "VulkanQueue.h"
#include "VulkanRHIResource.h"
#include "VulkanUtil.h"

#include "misc/MacroUtils.h"
#include "rhi/RHIResource.h"

#include <cassert>
#include <stdexcept>

namespace Moer::Render {

static constexpr std::string_view s_ring_desc_buffer_name       = "VkDescriporHeap::RingDescriptorBuffer";
static constexpr uint32           s_offline_descriptor_capacity = 10086;

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

VulkanDescriptorHeap::VulkanDescriptorHeap(VulkanDevice& _device) :
    next_buffer_cache_byte_offset(0),
    next_image_cache_byte_offset(0),
    next_acceleration_structure_cache_byte_offset(0),
    m_device(&_device),
    storage_buffer_descriptor_size(GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)),
    uniform_buffer_descriptor_size(GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)),
    storage_texel_buffer_descriptor_size(
        GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
    ),
    uniform_texel_buffer_descriptor_size(
        GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
    ),
    buffer_cache_slot_stride(
        std::max({
            GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER),
            GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER),
        })
    ),
    sampled_image_descriptor_size(
        _device.GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize
    ),
    storage_image_descriptor_size(
        _device.GetOptionalProperties().descriptor_buffer_properties.storageImageDescriptorSize
    ),
    image_cache_slot_stride(
        std::max(
            _device.GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize,
            _device.GetOptionalProperties().descriptor_buffer_properties.storageImageDescriptorSize
        )
    ),
    sampler_descriptor_size(
        _device.GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize
    ),
    acceleration_structure_descriptor_size(
        _device.GetOptionalProperties().descriptor_buffer_properties.accelerationStructureDescriptorSize
    ),
    image_cache_byte_offset(
        _device.GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize *
        VulkanDevice::bindless_sampler_count
    ) {

    buffer_descriptor_cache.resize(s_offline_descriptor_capacity * buffer_cache_slot_stride);
    sampler_and_image_descriptor_cache.resize(
        image_cache_byte_offset + s_offline_descriptor_capacity * image_cache_slot_stride
    );
    acceleration_structure_descriptor_cache.resize(
        s_offline_descriptor_capacity * acceleration_structure_descriptor_size
    );
    buffer_descriptor_types.resize(s_offline_descriptor_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM);
    image_descriptor_types.resize(s_offline_descriptor_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM);

    VkBuffer           desc_buffer            = VK_NULL_HANDLE;
    VmaAllocation      desc_buffer_allocation = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    // Increase descriptor buffer size to accommodate larger descriptor sets
    // Each frame gets enough space for all possible descriptors
    // Graphics and compute each retain their own in-flight slice budget. A
    // shared three-slice pool can deadlock when one queue holds every slice
    // while waiting on work that the other queue has not yet recorded.
    constexpr uint32 descriptor_queue_count = 2;
    buffer_ci.size  = descriptor_queue_count * s_queue_max_frame_in_flight * 1024 * 1024;
    buffer_ci.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

#if WITH_CUDA
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

    descriptor_buffer = MoerNew(VulkanBuffer)(
        s_ring_desc_buffer_name, buffer_info, *m_device, desc_buffer, desc_buffer_allocation, false, true
    );

    //fill offsets
    lease_slot_byte_offsets.resize(descriptor_queue_count * m_device->command_allocator_slot_count);

    // Use descriptorBufferOffsetAlignment for ring buffer offset alignment
    uint64 alignment =
        m_device->GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment;

    // Calculate per-frame size conservatively to ensure each frame has enough space
    uint64 per_frame_size = (buffer_ci.size / lease_slot_byte_offsets.size() / alignment) * alignment;

    for (uint32_t i = 0; i < lease_slot_byte_offsets.size(); ++i) {
        lease_slot_byte_offsets[i] = Moer::AlignUp(per_frame_size * i, alignment);
    }
    active_leases.resize(lease_slot_byte_offsets.size());
    vmaMapMemory(
        m_device->GetVmaAllocator(), descriptor_buffer->GetAllocation(), (void**)&mapped_descriptor_data
    );

    //fill sampler descs in image_data
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

VulkanDescriptorHeap::~VulkanDescriptorHeap() {
    if (descriptor_buffer) {
        vmaUnmapMemory(m_device->GetVmaAllocator(), descriptor_buffer->GetAllocation());
        MoerDelete(descriptor_buffer);
        descriptor_buffer = nullptr;
    }
}

uint VulkanDescriptorHeap::GetOrCreateBufferDescriptorOffset(
    const BufferView& _in_buffer,
    VkDescriptorType  _type,
    VkFormat          _format
) {
    assert(_in_buffer.GetBuffer() != nullptr && "buffer is nullptr");
    VulkanBuffer* vk_buffer     = ResourceCast(_in_buffer.GetBuffer());
    VkFormat      buffer_format = VulkanEnumTranslator::METoVKFormat(_in_buffer.format);
    _format                     = buffer_format == VK_FORMAT_UNDEFINED ? _format : buffer_format;
    if (_type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
        _type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
        _format = VK_FORMAT_UNDEFINED;
    }

    const VkBufferDescKey key{
        .byte_offset = _in_buffer.byte_offset,
        .byte_size   = _in_buffer.GetByteSize(),
        .format      = _format,
    };

    std::lock_guard<std::mutex> lock(cache_mutex);
    uint                        idx     = 0;
    auto&                       indices = vk_buffer->GetDescriptorIndices(_type);

    auto it = indices.find(key);
    if (it != indices.end()) {
        return it->second * buffer_cache_slot_stride;
    } else {
        VkDescriptorAddressInfoEXT buffer_info{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        buffer_info.address = vk_buffer->DeviceAddress() + _in_buffer.byte_offset;
        buffer_info.range   = _in_buffer.GetByteSize();
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
                LOG_ERROR(
                    "Unsupported buffer descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, _type)
                );
                assert(false && "Unsupported buffer descriptor type");
                return 0;
        }
        if (free_buffer_slots.empty()) {
            idx = next_buffer_cache_byte_offset / buffer_cache_slot_stride;
            if (idx >= buffer_descriptor_types.size()) {
                throw std::runtime_error("offline buffer descriptor cache exhausted");
            }
            next_buffer_cache_byte_offset += buffer_cache_slot_stride;
        } else {
            idx = free_buffer_slots.back();
            free_buffer_slots.pop_back();
            if (idx >= buffer_descriptor_types.size()) {
                throw std::runtime_error("offline buffer descriptor free-list index is invalid");
            }
        }

        indices[key]                 = idx;
        buffer_descriptor_types[idx] = _type;

        vkGetDescriptorEXT(
            m_device->GetDevice(),
            &buffer_desc_info,
            desc_size,
            buffer_descriptor_cache.data() + idx * buffer_cache_slot_stride
        );
    }
    return idx * buffer_cache_slot_stride;
}
void VulkanDescriptorHeap::FreeBufferDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_slot_index < buffer_descriptor_types.size() && "offline buffer descriptor index is invalid");
    buffer_descriptor_types[_slot_index] = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    free_buffer_slots.push_back(_slot_index);
}

uint VulkanDescriptorHeap::GetOrCreateImageDescriptorOffset(
    const TextureView* _in_image,
    VkImageLayout      _layout,
    VkDescriptorType   _descriptor_type
) {
    auto* texture = ResourceCast(_in_image->texture);
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

    const uint8      array_count = _in_image->num_array == 0 ? 1 : _in_image->num_array;
    VkTextureDescKey key{
        _descriptor_type,
        _layout,
        _in_image->mip_level,
        _in_image->num_mips,
        _in_image->array_layer,
        array_count,
    };

    // Image-view creation owns a texture-local mutex. Resolve it before the
    // descriptor heap lock so texture destruction never observes the reverse
    // heap->view / view->heap lock order.
    const VkImageView image_view =
        texture->GetView(_in_image->mip_level, _in_image->num_mips, _in_image->array_layer, array_count);

    std::lock_guard<std::mutex> lock(cache_mutex);
    const auto                  cached = texture->m_descriptor_indices.find(key);
    if (cached != texture->m_descriptor_indices.end()) {
        return cached->second * image_cache_slot_stride + image_cache_byte_offset;
    }
    uint idx = 0;
    {
        VkDescriptorImageInfo  image_info{.imageView = image_view, .imageLayout = _layout};
        VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        desc_info.type = _descriptor_type;
        if (_descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            desc_info.data.pStorageImage = &image_info;
        } else {
            desc_info.data.pSampledImage = &image_info;
        }
        if (free_image_slots.empty()) {
            idx = next_image_cache_byte_offset / image_cache_slot_stride;
            if (idx >= image_descriptor_types.size()) {
                throw std::runtime_error("offline image descriptor cache exhausted");
            }
            next_image_cache_byte_offset += image_cache_slot_stride;
        } else {
            idx = free_image_slots.back();
            free_image_slots.pop_back();
            if (idx >= image_descriptor_types.size()) {
                throw std::runtime_error("offline image descriptor free-list index is invalid");
            }
        }
        texture->m_descriptor_indices.emplace(key, idx);
        image_descriptor_types[idx] = _descriptor_type;
        vkGetDescriptorEXT(
            m_device->GetDevice(),
            &desc_info,
            GetDescriptorSize(_descriptor_type),
            sampler_and_image_descriptor_cache.data() + idx * image_cache_slot_stride +
                image_cache_byte_offset
        );
    }
    return idx * image_cache_slot_stride + image_cache_byte_offset;
}
void VulkanDescriptorHeap::FreeImageDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_slot_index < image_descriptor_types.size() && "offline image descriptor index is invalid");
    image_descriptor_types[_slot_index] = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    free_image_slots.push_back(_slot_index);
}
uint VulkanDescriptorHeap::GetSamplerDescriptorOffset(Sampler _sampler) {
    return m_device->GetSamplerIndex(_sampler) * sampler_descriptor_size;
}

uint VulkanDescriptorHeap::GetOrCreateAccelerationStructureDescriptorOffset(
    VulkanAccelerationStructure* _as
) {
    assert(_as != nullptr && "accel struct is nullptr");
    assert(_as->handle != VK_NULL_HANDLE && "accel struct handle is null");
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (_as->m_descriptor_idx >= 0) {
        return _as->m_descriptor_idx * acceleration_structure_descriptor_size;
    }
    VkAccelerationStructureDeviceAddressInfoKHR address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };
    address_info.accelerationStructure = _as->handle;
    VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    desc_info.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    desc_info.data.accelerationStructure =
        vkGetAccelerationStructureDeviceAddressKHR(m_device->GetDevice(), &address_info);
    uint idx = 0;
    if (free_acceleration_structure_slots.empty()) {
        idx = next_acceleration_structure_cache_byte_offset / acceleration_structure_descriptor_size;
        if (idx >= acceleration_structure_descriptor_cache.size() / acceleration_structure_descriptor_size) {
            throw std::runtime_error("offline acceleration-structure descriptor cache exhausted");
        }
        next_acceleration_structure_cache_byte_offset += acceleration_structure_descriptor_size;
    } else {
        idx = free_acceleration_structure_slots.back();
        free_acceleration_structure_slots.pop_back();
        if (idx >= acceleration_structure_descriptor_cache.size() / acceleration_structure_descriptor_size) {
            throw std::runtime_error("offline acceleration-structure descriptor free-list index is invalid");
        }
    }
    _as->m_descriptor_idx = idx;
    vkGetDescriptorEXT(
        m_device->GetDevice(),
        &desc_info,
        acceleration_structure_descriptor_size,
        acceleration_structure_descriptor_cache.data() + idx * acceleration_structure_descriptor_size
    );

    return idx * acceleration_structure_descriptor_size;
}

void VulkanDescriptorHeap::FreeAccelerationStructureDescriptorSlot(uint _slot_index) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    free_acceleration_structure_slots.push_back(_slot_index);
}

VulkanDescriptorBufferLease VulkanDescriptorHeap::AcquireDescriptorBufferLease(EQueueType _queue_type) {
    assert(
        (_queue_type == EQueueType::Graphics || _queue_type == EQueueType::Compute) &&
        "copy queues do not own descriptor buffer leases"
    );
    const uint32                 queue_index = _queue_type == EQueueType::Graphics ? 0u : 1u;
    const uint32                 slot_count  = m_device->command_allocator_slot_count;
    const uint32                 first_slot  = queue_index * slot_count;
    std::unique_lock<std::mutex> lock(lease_mutex);
    lease_available.wait(lock, [&, this] {
        return std::any_of(
            active_leases.begin() + first_slot,
            active_leases.begin() + first_slot + slot_count,
            [](const auto& _lease) {
                return _lease == nullptr;
            }
        );
    });

    for (uint32 attempt = 0; attempt < slot_count; ++attempt) {
        const uint32 relative_slot = (next_queue_slots[queue_index] + attempt) % slot_count;
        const uint32 slot          = first_slot + relative_slot;
        if (active_leases[slot] != nullptr) {
            continue;
        }
        const uint64 begin  = lease_slot_byte_offsets[slot];
        const uint64 end    = slot + 1 < lease_slot_byte_offsets.size() ? lease_slot_byte_offsets[slot + 1] :
                                                                          descriptor_buffer->GetByteSize();
        auto         state  = std::make_shared<VulkanDescriptorBufferLeaseState>(slot, begin, end);
        active_leases[slot] = state;
        next_queue_slots[queue_index] = (relative_slot + 1) % slot_count;
        return {std::move(state)};
    }

    assert(false && "descriptor lease predicate succeeded without a free slot");
    return {};
}

void VulkanDescriptorHeap::FlushDescriptorWrites(const VulkanDescriptorBufferLease& _lease) {
    assert(_lease.IsValid() && "descriptor buffer lease is invalid");
    const uint64 byte_size =
        _lease.state->next_byte_offset.load(std::memory_order_acquire) - _lease.state->begin_byte_offset;
    if (byte_size == 0) {
        return;
    }
    vmaFlushAllocation(
        m_device->GetVmaAllocator(),
        descriptor_buffer->GetAllocation(),
        _lease.state->begin_byte_offset,
        byte_size
    );
}

void VulkanDescriptorHeap::RecycleDescriptorBufferLease(VulkanDescriptorBufferLease _lease) {
    if (!_lease.IsValid()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(lease_mutex);
        const uint32                slot = _lease.state->slot_index;
        assert(slot < active_leases.size() && "descriptor lease slot is invalid");
        assert(
            active_leases[slot] == _lease.state && "descriptor lease was recycled twice or after slot reuse"
        );
        active_leases[slot].reset();
    }
    // Graphics and compute wait on disjoint slot partitions but share this
    // condition variable. notify_one could wake the wrong partition and leave
    // the queue that actually gained a slot asleep indefinitely.
    lease_available.notify_all();
}

std::optional<uint64> VulkanDescriptorHeap::ReserveDescriptorRange(
    const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease,
    uint64                                                   _size
) {
    assert(_lease != nullptr && "descriptor range reservation has no submission lease");
    if (_lease == nullptr) {
        return std::nullopt;
    }
    uint64 begin = _lease->next_byte_offset.load(std::memory_order_acquire);
    while (true) {
        if (begin > _lease->end_byte_offset || _size > _lease->end_byte_offset - begin) {
            return std::nullopt;
        }
        const uint64 end = begin + _size;
        if (_lease->next_byte_offset.compare_exchange_weak(
                begin, end, std::memory_order_acq_rel, std::memory_order_acquire
            )) {
            return begin;
        }
    }
}

uint64 VulkanDescriptorHeap::GetDescriptorWriteOffset(
    const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease
) const {
    assert(_lease != nullptr && "descriptor offset query has no submission lease");
    return _lease->next_byte_offset.load(std::memory_order_acquire);
}

void VulkanDescriptorHeap::RewindDescriptorWrites(
    const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease,
    uint64                                                   _offset
) {
    assert(_lease != nullptr && "descriptor rewind has no submission lease");
    const uint64 current = _lease->next_byte_offset.load(std::memory_order_acquire);
    assert(
        _offset >= _lease->begin_byte_offset && _offset <= current &&
        "descriptor rewind escaped the submission lease"
    );
    _lease->next_byte_offset.store(_offset, std::memory_order_release);
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

void VulkanDescriptorHeap::WriteUniformDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_source_byte_offset % buffer_cache_slot_stride == 0 && "buffer descriptor offset is misaligned");
    const uint64 idx = _source_byte_offset / buffer_cache_slot_stride;
    assert(idx < buffer_descriptor_types.size() && "buffer descriptor offset is invalid");
    assert(
        buffer_descriptor_types[idx] == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        "uniform descriptor writer received a different descriptor type"
    );
    memcpy(
        mapped_descriptor_data + _destination_byte_offset,
        buffer_descriptor_cache.data() + _source_byte_offset,
        GetDescriptorSize(buffer_descriptor_types[idx])
    );
}

void VulkanDescriptorHeap::WriteStorageDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_source_byte_offset % buffer_cache_slot_stride == 0 && "buffer descriptor offset is misaligned");
    const uint64 idx = _source_byte_offset / buffer_cache_slot_stride;
    assert(idx < buffer_descriptor_types.size() && "buffer descriptor offset is invalid");
    const VkDescriptorType descriptor_type = buffer_descriptor_types[idx];
    assert(
        (descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
         descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
         descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) &&
        "storage descriptor writer received a different descriptor type"
    );
    memcpy(
        mapped_descriptor_data + _destination_byte_offset,
        buffer_descriptor_cache.data() + _source_byte_offset,
        GetDescriptorSize(descriptor_type)
    );
}

void VulkanDescriptorHeap::WriteImageDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    assert(_source_byte_offset >= image_cache_byte_offset && "image descriptor offset precedes image cache");
    const uint64 relative_offset = _source_byte_offset - image_cache_byte_offset;
    assert(relative_offset % image_cache_slot_stride == 0 && "image descriptor offset is misaligned");
    const uint64 idx = relative_offset / image_cache_slot_stride;
    assert(idx < image_descriptor_types.size() && "image descriptor offset is invalid");
    memcpy(
        mapped_descriptor_data + _destination_byte_offset,
        sampler_and_image_descriptor_cache.data() + _source_byte_offset,
        GetDescriptorSize(image_descriptor_types[idx])
    );
}

void VulkanDescriptorHeap::WriteSamplerDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    memcpy(
        mapped_descriptor_data + _destination_byte_offset,
        sampler_and_image_descriptor_cache.data() + _source_byte_offset,
        sampler_descriptor_size
    );
}

void VulkanDescriptorHeap::WriteAccelDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    memcpy(
        mapped_descriptor_data + _destination_byte_offset,
        acceleration_structure_descriptor_cache.data() + _source_byte_offset,
        acceleration_structure_descriptor_size
    );
}
} // namespace Moer::Render
