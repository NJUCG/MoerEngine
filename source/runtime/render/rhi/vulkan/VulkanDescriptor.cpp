#include "VulkanDescriptor.h"

#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanPlatform.h"
#include "VulkanRHIResource.h"
#include "VulkanUtil.h"

#include "misc/MacroUtils.h"
#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"

#include <cassert>
#include <stdexcept>

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
static constexpr uint32           s_offline_descriptor_capacity = 10086;

static uint GetBufferDescriptorSize(const VulkanDevice& _device, VkDescriptorType _type) {
    const auto& properties = _device.GetOptionalProperties().descriptor_buffer_properties;
    const bool robust_buffer_access =
        _device.GetCoreFeatures().core_1_0.robustBufferAccess == VK_TRUE;
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return static_cast<uint>(
                robust_buffer_access ? properties.robustStorageBufferDescriptorSize
                                     : properties.storageBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return static_cast<uint>(
                robust_buffer_access ? properties.robustUniformBufferDescriptorSize
                                     : properties.uniformBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return static_cast<uint>(
                robust_buffer_access ? properties.robustStorageTexelBufferDescriptorSize
                                     : properties.storageTexelBufferDescriptorSize
            );
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return static_cast<uint>(
                robust_buffer_access ? properties.robustUniformTexelBufferDescriptorSize
                                     : properties.uniformTexelBufferDescriptorSize
            );
        default:
            throw std::runtime_error("unsupported buffer descriptor type");
    }
}

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
    buffer_offset(0),
    image_offset(0),
    accel_offset(0),
    m_device(&_device),
    storage_desc_stride(GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)),
    uniform_desc_stride(GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)),
    storage_texel_desc_stride(
        GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
    ),
    uniform_texel_desc_stride(
        GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
    ),
    buffer_desc_stride(
        std::max({
            GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER),
            GetBufferDescriptorSize(_device, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER),
        })
    ),
    sampled_image_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize
    ),
    storage_image_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.storageImageDescriptorSize
    ),
    image_desc_stride(
        std::max(
            _device.GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize,
            _device.GetOptionalProperties().descriptor_buffer_properties.storageImageDescriptorSize
        )
    ),
    sample_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize
    ),
    accel_desc_stride(
        _device.GetOptionalProperties().descriptor_buffer_properties.accelerationStructureDescriptorSize
    ),
    texture_desc_offset(
        _device.GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize *
        VulkanDevice::bindless_sampler_cnt
    ) {

    buffer_desc_data.resize(s_offline_descriptor_capacity * buffer_desc_stride);
    image_desc_data.resize(
        texture_desc_offset + s_offline_descriptor_capacity * image_desc_stride
    );
    accel_desc_data.resize(s_offline_descriptor_capacity * accel_desc_stride);
    buffer_desc_types.resize(s_offline_descriptor_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM);
    image_desc_types.resize(s_offline_descriptor_capacity, VK_DESCRIPTOR_TYPE_MAX_ENUM);

    VkBuffer           desc_buffer            = VK_NULL_HANDLE;
    VmaAllocation      desc_buffer_allocation = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    // Increase descriptor buffer size to accommodate larger descriptor sets
    // Each frame gets enough space for all possible descriptors
    // Graphics and compute each retain their own in-flight slice budget. A
    // shared three-slice pool can deadlock when one queue holds every slice
    // while waiting on work that the other queue has not yet recorded.
    constexpr uint32 descriptor_queue_count = 2;
    buffer_ci.size = descriptor_queue_count * s_queue_max_frame_in_flight * 1024 * 1024;
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

    ring_desc_buffer = MoerNew(VulkanBuffer)(
        s_ring_desc_buffer_name, buffer_info, *m_device, desc_buffer, desc_buffer_allocation, false, true
    );

    //fill offsets
    ring_buffer_offsets.resize(descriptor_queue_count * m_device->cmd_alloc_limits);

    // Use descriptorBufferOffsetAlignment for ring buffer offset alignment
    uint64 alignment =
        m_device->GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment;

    // Calculate per-frame size conservatively to ensure each frame has enough space
    uint64 per_frame_size = (buffer_ci.size / ring_buffer_offsets.size() / alignment) * alignment;

    for (uint32_t i = 0; i < ring_buffer_offsets.size(); ++i) {
        ring_buffer_offsets[i] = Moer::AlignUp(per_frame_size * i, alignment);
    }
    active_push_leases.resize(ring_buffer_offsets.size());
    vmaMapMemory(m_device->GetVmaAllocator(), ring_desc_buffer->GetAllocation(), (void**)&map_ptr);

    //fill sampler descs in image_data
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
    VulkanBuffer* vk_buffer = ResourceCast(_in_buffer.GetBuffer());
    VkFormat      buffer_format = VulkanEnumTranslator::METoVKFormat(_in_buffer.format);
    _format = buffer_format == VK_FORMAT_UNDEFINED ? _format : buffer_format;
    if (_type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
        _type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
        _format = VK_FORMAT_UNDEFINED;
    }

    const VkBufferDescKey key{
        .byte_offset = _in_buffer.byte_offset,
        .byte_size   = _in_buffer.GetByteSize(),
        .format      = _format,
    };

    std::lock_guard<std::mutex> lock(m_mutex);
    uint          idx     = 0;
    auto&         indices = vk_buffer->GetDescriptorIndices(_type);

    auto it = indices.find(key);
    if (it != indices.end()) {
        return it->second * buffer_desc_stride;
    } else {
        VkDescriptorAddressInfoEXT buffer_info{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        buffer_info.address = vk_buffer->DeviceAddress() + _in_buffer.byte_offset;
        buffer_info.range   = _in_buffer.GetByteSize();
        VkDescriptorGetInfoEXT buffer_desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        buffer_desc_info.type  = _type;
        const uint desc_size = GetDescriptorSize(_type);
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
        if (buffer_free_list.empty()) {
            idx = buffer_offset / buffer_desc_stride;
            if (idx >= buffer_desc_types.size()) {
                throw std::runtime_error("offline buffer descriptor cache exhausted");
            }
            buffer_offset += buffer_desc_stride;
        } else {
            idx = buffer_free_list.back();
            buffer_free_list.pop_back();
            if (idx >= buffer_desc_types.size()) {
                throw std::runtime_error("offline buffer descriptor free-list index is invalid");
            }
        }

        indices[key]          = idx;
        buffer_desc_types[idx] = _type;

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
    assert(_idx < buffer_desc_types.size() && "offline buffer descriptor index is invalid");
    buffer_desc_types[_idx] = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    buffer_free_list.push_back(_idx);
}

uint VulkanDescriptorHeap::GetImageDescIdx(
    const TextureView* _in_image,
    VkImageLayout      _layout,
    VkDescriptorType   _descriptor_type
) {
    auto* texture = ResourceCast(_in_image->texture);
    assert(texture != nullptr && "texture is nullptr");
    if (_descriptor_type == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
        _descriptor_type = _layout == VK_IMAGE_LAYOUT_GENERAL ?
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    }
    assert(
        (_descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
         _descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) &&
        "unsupported image descriptor type"
    );

    const uint8 array_count = _in_image->num_array == 0 ? 1 : _in_image->num_array;
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
    const VkImageView image_view = texture->GetView(
        _in_image->mip_level,
        _in_image->num_mips,
        _in_image->array_layer,
        array_count
    );

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto cached = texture->m_descriptor_indices.find(key);
    if (cached != texture->m_descriptor_indices.end()) {
        return cached->second * image_desc_stride + texture_desc_offset;
    }
    uint idx = 0;
    {
        VkDescriptorImageInfo image_info{
            .imageView = image_view, .imageLayout = _layout
        };
        VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        desc_info.type = _descriptor_type;
        if (_descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            desc_info.data.pStorageImage = &image_info;
        } else {
            desc_info.data.pSampledImage = &image_info;
        }
        if (image_free_list.empty()) {
            idx = image_offset / image_desc_stride;
            if (idx >= image_desc_types.size()) {
                throw std::runtime_error("offline image descriptor cache exhausted");
            }
            image_offset += image_desc_stride;
        } else {
            idx = image_free_list.back();
            image_free_list.pop_back();
            if (idx >= image_desc_types.size()) {
                throw std::runtime_error("offline image descriptor free-list index is invalid");
            }
        }
        texture->m_descriptor_indices.emplace(key, idx);
        image_desc_types[idx] = _descriptor_type;
        vkGetDescriptorEXT(
            m_device->GetDevice(),
            &desc_info,
            GetDescriptorSize(_descriptor_type),
            image_desc_data.data() + idx * image_desc_stride + texture_desc_offset
        );
    }
    return idx * image_desc_stride + texture_desc_offset;
}
void VulkanDescriptorHeap::FreeImageDescIdx(uint _idx) {
    std::lock_guard<std::mutex> lock(m_mutex);
    assert(_idx < image_desc_types.size() && "offline image descriptor index is invalid");
    image_desc_types[_idx] = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    image_free_list.push_back(_idx);
}
uint VulkanDescriptorHeap::GetSamplerDescIdx(Sampler _sampler) {
    return m_device->GetSamplerIdx(_sampler) * sample_desc_stride;
}

uint VulkanDescriptorHeap::GetAccelDescIdx(VulkanAccelerationStructure* _as) {
    assert(_as != nullptr && "accel struct is nullptr");
    assert(_as->handle != VK_NULL_HANDLE && "accel struct handle is null");
    std::lock_guard<std::mutex> lock(m_mutex);
    if (_as->m_descriptor_idx >= 0) {
        return _as->m_descriptor_idx * accel_desc_stride;
    }
    VkAccelerationStructureDeviceAddressInfoKHR address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };
    address_info.accelerationStructure = _as->handle;
    VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    desc_info.type                       = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    desc_info.data.accelerationStructure =
        vkGetAccelerationStructureDeviceAddressKHR(m_device->GetDevice(), &address_info);
    uint                        idx      = 0;
    if (accel_free_list.empty()) {
        idx = accel_offset / accel_desc_stride;
        if (idx >= accel_desc_data.size() / accel_desc_stride) {
            throw std::runtime_error("offline acceleration-structure descriptor cache exhausted");
        }
        accel_offset += accel_desc_stride;
    } else {
        idx = accel_free_list.back();
        accel_free_list.pop_back();
        if (idx >= accel_desc_data.size() / accel_desc_stride) {
            throw std::runtime_error(
                "offline acceleration-structure descriptor free-list index is invalid"
            );
        }
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

VulkanDescriptorPushLease VulkanDescriptorHeap::BeginPushDescriptors(EQueueType _queue_type) {
    assert(
        (_queue_type == EQueueType::Graphics || _queue_type == EQueueType::Compute) &&
        "copy queues do not own descriptor push leases"
    );
    const uint32 queue_index = _queue_type == EQueueType::Graphics ? 0u : 1u;
    const uint32 slot_count  = m_device->cmd_alloc_limits;
    const uint32 first_slot  = queue_index * slot_count;
    std::unique_lock<std::mutex> lock(push_range_mutex);
    push_range_cv.wait(lock, [&, this] {
        return std::any_of(
            active_push_leases.begin() + first_slot,
            active_push_leases.begin() + first_slot + slot_count,
            [](const auto& _lease) { return _lease == nullptr; }
        );
    });

    for (uint32 attempt = 0; attempt < slot_count; ++attempt) {
        const uint32 relative_slot = (next_push_slots[queue_index] + attempt) % slot_count;
        const uint32 slot = first_slot + relative_slot;
        if (active_push_leases[slot] != nullptr) {
            continue;
        }
        const uint64 begin = ring_buffer_offsets[slot];
        const uint64 end = slot + 1 < ring_buffer_offsets.size()
                               ? ring_buffer_offsets[slot + 1]
                               : ring_desc_buffer->GetByteSize();
        auto state = std::make_shared<VulkanDescriptorPushLeaseState>(slot, begin, end);
        active_push_leases[slot] = state;
        next_push_slots[queue_index] = (relative_slot + 1) % slot_count;
        return {std::move(state)};
    }

    assert(false && "descriptor lease predicate succeeded without a free slot");
    return {};
}

void VulkanDescriptorHeap::EndPushDescriptors(const VulkanDescriptorPushLease& _lease) {
    assert(_lease.IsValid() && "descriptor push lease is invalid");
    const uint64 byte_size =
        _lease.state->next.load(std::memory_order_acquire) - _lease.state->begin;
    if (byte_size == 0) {
        return;
    }
    vmaFlushAllocation(
        m_device->GetVmaAllocator(),
        ring_desc_buffer->GetAllocation(),
        _lease.state->begin,
        byte_size
    );
}

void VulkanDescriptorHeap::RecyclePushDescriptors(VulkanDescriptorPushLease _lease) {
    if (!_lease.IsValid()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(push_range_mutex);
        const uint32 slot = _lease.state->slot;
        assert(slot < active_push_leases.size() && "descriptor lease slot is invalid");
        assert(
            active_push_leases[slot] == _lease.state &&
            "descriptor lease was recycled twice or after slot reuse"
        );
        active_push_leases[slot].reset();
    }
    // Graphics and compute wait on disjoint slot partitions but share this
    // condition variable. notify_one could wake the wrong partition and leave
    // the queue that actually gained a slot asleep indefinitely.
    push_range_cv.notify_all();
}

std::optional<uint64> VulkanDescriptorHeap::ReservePushDescriptorRange(
    const std::shared_ptr<VulkanDescriptorPushLeaseState>& _lease,
    uint64                                                  _size
) {
    assert(_lease != nullptr && "descriptor range reservation has no submission lease");
    if (_lease == nullptr) {
        return std::nullopt;
    }
    uint64 begin = _lease->next.load(std::memory_order_acquire);
    while (true) {
        if (begin > _lease->end || _size > _lease->end - begin) {
            return std::nullopt;
        }
        const uint64 end = begin + _size;
        if (_lease->next.compare_exchange_weak(
                begin, end, std::memory_order_acq_rel, std::memory_order_acquire
            )) {
            return begin;
        }
    }
}

uint64 VulkanDescriptorHeap::CurrentPushDescriptorOffset(
    const std::shared_ptr<VulkanDescriptorPushLeaseState>& _lease
) const {
    assert(_lease != nullptr && "descriptor offset query has no submission lease");
    return _lease->next.load(std::memory_order_acquire);
}

void VulkanDescriptorHeap::RewindPushDescriptors(
    const std::shared_ptr<VulkanDescriptorPushLeaseState>& _lease,
    uint64                                                  _offset
) {
    assert(_lease != nullptr && "descriptor rewind has no submission lease");
    const uint64 current = _lease->next.load(std::memory_order_acquire);
    assert(
        _offset >= _lease->begin && _offset <= current &&
        "descriptor rewind escaped the submission lease"
    );
    _lease->next.store(_offset, std::memory_order_release);
}

uint VulkanDescriptorHeap::GetDescriptorSize(VkDescriptorType _type) const {
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: return sample_desc_stride;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return sampled_image_desc_stride;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return storage_image_desc_stride;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return uniform_texel_desc_stride;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return storage_texel_desc_stride;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return uniform_desc_stride;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return storage_desc_stride;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return accel_desc_stride;
        default:
            LOG_ERROR("Unsupported descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, _type));
            assert(false && "unsupported descriptor type");
            return 0;
    }
}

void VulkanDescriptorHeap::WriteUniformDesc(uint64 _src_offset, uint64 _dst_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    assert(_src_offset % buffer_desc_stride == 0 && "buffer descriptor offset is misaligned");
    const uint64 idx = _src_offset / buffer_desc_stride;
    assert(idx < buffer_desc_types.size() && "buffer descriptor offset is invalid");
    assert(
        buffer_desc_types[idx] == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        "uniform descriptor writer received a different descriptor type"
    );
    memcpy(
        map_ptr + _dst_offset,
        buffer_desc_data.data() + _src_offset,
        GetDescriptorSize(buffer_desc_types[idx])
    );
}

void VulkanDescriptorHeap::WriteStorageDesc(uint64 _src_offset, uint64 _dst_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    assert(_src_offset % buffer_desc_stride == 0 && "buffer descriptor offset is misaligned");
    const uint64 idx = _src_offset / buffer_desc_stride;
    assert(idx < buffer_desc_types.size() && "buffer descriptor offset is invalid");
    const VkDescriptorType descriptor_type = buffer_desc_types[idx];
    assert(
        (descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
         descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
         descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) &&
        "storage descriptor writer received a different descriptor type"
    );
    memcpy(
        map_ptr + _dst_offset,
        buffer_desc_data.data() + _src_offset,
        GetDescriptorSize(descriptor_type)
    );
}

void VulkanDescriptorHeap::WriteImageDesc(uint64 _src_offset, uint64 _dst_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    assert(_src_offset >= texture_desc_offset && "image descriptor offset precedes image cache");
    const uint64 relative_offset = _src_offset - texture_desc_offset;
    assert(relative_offset % image_desc_stride == 0 && "image descriptor offset is misaligned");
    const uint64 idx = relative_offset / image_desc_stride;
    assert(idx < image_desc_types.size() && "image descriptor offset is invalid");
    memcpy(
        map_ptr + _dst_offset,
        image_desc_data.data() + _src_offset,
        GetDescriptorSize(image_desc_types[idx])
    );
}

void VulkanDescriptorHeap::WriteSamplerDesc(uint64 _src_offset, uint64 _dst_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    memcpy(map_ptr + _dst_offset, image_desc_data.data() + _src_offset, sample_desc_stride);
}

void VulkanDescriptorHeap::WriteAccelDesc(uint64 _src_offset, uint64 _dst_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    memcpy(map_ptr + _dst_offset, accel_desc_data.data() + _src_offset, accel_desc_stride);
}
} // namespace Moer::Render
