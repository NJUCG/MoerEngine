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

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

const float default_pool_size[VK_DESCRIPTOR_TYPE_RANGE_SIZE] = {
    4096,
    4096,
    4096,
};

namespace Moer::Render {
static constexpr std::string_view s_ring_desc_buffer_name        = "VkDescriporHeap::RingDescriptorBuffer";
static constexpr std::string_view s_sampler_heap_buffer_name     = "VkDescriptorHeap::SamplerHeapBuffer";
static constexpr uint32           s_offline_buffer_desc_capacity = 10086;
static constexpr uint32           s_offline_image_desc_capacity  = 10086;
static constexpr uint32           s_offline_accel_desc_capacity  = 10086;
static constexpr uint64           s_online_heap_slack_size       = 1024ull * 1024ull;

static uint64 GetCanonicalResourceHeapStride(const VulkanDevice& _device) {
    const auto& heap_props = _device.GetOptionalProperties().descriptor_heap_properties;
    return std::max(
        Moer::AlignUp(uint64(heap_props.bufferDescriptorSize), heap_props.bufferDescriptorAlignment),
        Moer::AlignUp(uint64(heap_props.imageDescriptorSize), heap_props.imageDescriptorAlignment)
    );
}

static VkHostAddressRangeEXT MakeHostRange(void* _address, size_t _size) {
    return VkHostAddressRangeEXT{.address = _address, .size = _size};
}

enum class EBindlessSizeType : uint8 {
    Buffer,
    Sampler,
    Image,
    Num,
};

enum class EBindlessSetType : uint8 {
    Buffer,
    SamplerAndImage,
    Num,
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

struct BufferCpuDescHandle {
    uint  data[4];
    void* Data() {
        return data;
    }
    const void* Data() const {
        return data;
    }
};

struct VulkanOfflineDescriptorArena {
    Array<byte> bytes;
    Array<uint> free_list;
    uint64      next_offset{0};
    uint        stride{0};
};

struct VulkanOfflineBufferDescriptor {
    VkResourceDescriptorInfoEXT    resource_info{};
    VkTexelBufferDescriptorInfoEXT texel_buffer_info{};
    VkDeviceAddressRangeEXT        address_range{};
};

struct VulkanOfflineImageDescriptor {
    VkResourceDescriptorInfoEXT resource_info{};
    VkImageDescriptorInfoEXT    image_info{};
};

struct VulkanOfflineSamplerDescriptor {
    VkSamplerCreateInfo sampler_info{};
};

struct VulkanOfflineAccelDescriptor {
    VkResourceDescriptorInfoEXT resource_info{};
    VkDeviceAddressRangeEXT     address_range{};
};

struct VulkanDescriptorFreeRange {
    uint64 offset{0};
    uint64 size{0};
};

struct VulkanResourceHeapStorage {
    VulkanBuffer* buffer{nullptr};
    uint8*        map_ptr{nullptr};
    uint64        size{0};
    uint32        heap_index{UINT32_MAX};
};

struct VulkanDescriptorBinderState {
    VulkanResourceHeapStorage*                         heap_storage{nullptr};
    uint64                                             range_size{0};
    uint64                                             range_limit{0};
    std::atomic_uint64_t                               next_offset{0};
    std::mutex                                         bindless_cache_mutex;
    UnorderedMap<uint64, VulkanBindlessRingCacheEntry> bindless_ring_cache;
};

static thread_local VulkanDescriptorBinderState* s_active_descriptor_binder = nullptr;

void VulkanDescriptorBinder::ActivateOnCurrentThread() const {
    assert(state != nullptr && "Descriptor binder is invalid");
    assert(
        s_active_descriptor_binder == nullptr || s_active_descriptor_binder == state.get() &&
            "Another descriptor binder is already active on this thread"
    );
    s_active_descriptor_binder = state.get();
}

void VulkanDescriptorBinder::DeactivateOnCurrentThread() const {
    if (state == nullptr) {
        return;
    }
    assert(s_active_descriptor_binder == state.get() && "Descriptor binder/thread mismatch");
    s_active_descriptor_binder = nullptr;
}

class VulkanOfflineDescriptorManager;

class VulkanHeapManager {
public:
    explicit VulkanHeapManager(VulkanDevice& _device) :
        device(_device),
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
        sample_desc_stride(
            _device.GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize
        ),
        accel_desc_stride(
            _device.GetOptionalProperties().descriptor_buffer_properties.accelerationStructureDescriptorSize
        ),
        heap_buffer_desc_stride(_device.GetOptionalProperties().descriptor_heap_properties.bufferDescriptorSize),
        heap_image_desc_stride(_device.GetOptionalProperties().descriptor_heap_properties.imageDescriptorSize),
        heap_sample_desc_stride(_device.GetOptionalProperties().descriptor_heap_properties.samplerDescriptorSize),
        heap_accel_desc_stride(_device.GetOptionalProperties().descriptor_heap_properties.bufferDescriptorSize),
        resource_heap_stride(GetCanonicalResourceHeapStride(_device)) {

        const auto& descriptor_heap_props = _device.GetOptionalProperties().descriptor_heap_properties;
        resource_heap_buffer_base_index   = 1;
        resource_heap_image_base_index    = resource_heap_buffer_base_index + s_offline_buffer_desc_capacity;
        resource_heap_accel_base_index    = resource_heap_image_base_index + s_offline_image_desc_capacity;

        const uint64 canonical_resource_heap_size =
            uint64(resource_heap_accel_base_index + s_offline_accel_desc_capacity) * resource_heap_stride;
        resource_heap_reserved_size = Moer::AlignUp(
            std::max(descriptor_heap_props.minResourceHeapReservedRange, canonical_resource_heap_size),
            std::max(
                descriptor_heap_props.bufferDescriptorAlignment,
                descriptor_heap_props.imageDescriptorAlignment
            )
        );
        sampler_heap_reserved_size = Moer::AlignUp(
            descriptor_heap_props.minSamplerHeapReservedRange,
            descriptor_heap_props.samplerDescriptorAlignment
        );
        online_heap_alignment = std::max(
            device.GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment,
            descriptor_heap_props.resourceHeapAlignment
        );
        online_heap_size = Moer::AlignUp(
            std::max(resource_heap_reserved_size + s_online_heap_slack_size, uint64(2) * resource_heap_reserved_size),
            online_heap_alignment
        );
    }

    ~VulkanHeapManager() {
        DestroyResourceHeaps();
        DestroySamplerHeap();
    }

    void Initialize(const VulkanOfflineDescriptorManager& _offline_manager);
    VulkanResourceHeapStorage& CreateResourceHeap(const VulkanOfflineDescriptorManager& _offline_manager);

    VulkanResourceHeapStorage& GetResourceHeap(uint32 _heap_index) {
        std::lock_guard<std::mutex> lock(resource_heaps_mutex);
        assert(_heap_index < resource_heaps.size() && resource_heaps[_heap_index] != nullptr);
        return *resource_heaps[_heap_index];
    }

    const VulkanResourceHeapStorage& GetResourceHeap(uint32 _heap_index) const {
        std::lock_guard<std::mutex> lock(resource_heaps_mutex);
        assert(_heap_index < resource_heaps.size() && resource_heaps[_heap_index] != nullptr);
        return *resource_heaps[_heap_index];
    }

    template <typename Fn>
    void ForEachResourceHeap(Fn&& _fn) const {
        std::lock_guard<std::mutex> lock(resource_heaps_mutex);
        for (const auto& heap : resource_heaps) {
            if (heap == nullptr) {
                continue;
            }
            _fn(*heap);
        }
    }

    void FlushResourceHeapRange(uint32 _heap_index, uint64 _offset, uint64 _size) const {
        if (_size == 0) {
            return;
        }
        const VulkanResourceHeapStorage& heap = GetResourceHeap(_heap_index);
        FlushMappedResourceHeapRange(heap, _offset, _size);
    }

    void FlushMappedResourceHeapRange(const VulkanResourceHeapStorage& _heap, uint64 _offset, uint64 _size) const {
        if (_size == 0) {
            return;
        }
        vmaFlushAllocation(device.GetVmaAllocator(), _heap.buffer->GetAllocation(), _offset, _size);
    }

    VkBindHeapInfoEXT MakeResourceHeapBindInfo(uint32 _heap_index) const {
        const VulkanResourceHeapStorage& heap = GetResourceHeap(_heap_index);
        return VkBindHeapInfoEXT{
            .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
            .pNext = nullptr,
            .heapRange = {
                .address = heap.buffer->DeviceAddress(),
                .size = heap.size,
            },
            .reservedRangeOffset = 0,
            .reservedRangeSize = resource_heap_reserved_size,
        };
    }

    VkBindHeapInfoEXT MakeSamplerHeapBindInfo() const {
        assert(sampler_heap_buffer != nullptr && "Sampler heap buffer must exist");
        return VkBindHeapInfoEXT{
            .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
            .pNext = nullptr,
            .heapRange = {
                .address = sampler_heap_buffer->DeviceAddress(),
                .size = sampler_heap_buffer->GetByteSize(),
            },
            .reservedRangeOffset = 0,
            .reservedRangeSize = sampler_heap_reserved_size,
        };
    }

private:
    void InitializeSamplerHeap(const VulkanOfflineDescriptorManager& _offline_manager);

    void DestroySamplerHeap() {
        if (sampler_heap_buffer == nullptr) {
            return;
        }
        MoerDelete(sampler_heap_buffer);
        sampler_heap_buffer = nullptr;
    }

    void DestroyResourceHeaps() {
        std::lock_guard<std::mutex> lock(resource_heaps_mutex);
        for (auto& heap : resource_heaps) {
            if (heap == nullptr) {
                continue;
            }
            vmaUnmapMemory(device.GetVmaAllocator(), heap->buffer->GetAllocation());
            MoerDelete(heap->buffer);
            heap->buffer = nullptr;
            heap.reset();
        }
        resource_heaps.clear();
    }

private:
    VulkanDevice& device;

public:
    uint storage_desc_stride{0};
    uint uniform_desc_stride{0};
    uint storage_texel_desc_stride{0};
    uint uniform_texel_desc_stride{0};
    uint buffer_desc_stride{0};
    uint image_desc_stride{0};
    uint sample_desc_stride{0};
    uint accel_desc_stride{0};
    uint heap_buffer_desc_stride{0};
    uint heap_image_desc_stride{0};
    uint heap_sample_desc_stride{0};
    uint heap_accel_desc_stride{0};

    uint64 resource_heap_stride{0};
    uint64 resource_heap_reserved_size{0};
    uint64 sampler_heap_reserved_size{0};
    uint64 online_heap_size{0};
    uint64 online_heap_alignment{0};
    uint32 resource_heap_buffer_base_index{1};
    uint32 resource_heap_image_base_index{1};
    uint32 resource_heap_accel_base_index{1};

private:
    VulkanBuffer* sampler_heap_buffer{nullptr};
    mutable std::mutex resource_heaps_mutex;
    Array<UniquePtr<VulkanResourceHeapStorage>> resource_heaps{};
};

class VulkanOfflineDescriptorManager {
public:
    VulkanOfflineDescriptorManager(VulkanDevice& _device, VulkanHeapManager& _heap_manager) :
        device(_device),
        heap_manager(_heap_manager),
        storage_desc_stride(_heap_manager.storage_desc_stride),
        uniform_desc_stride(_heap_manager.uniform_desc_stride),
        storage_texel_desc_stride(_heap_manager.storage_texel_desc_stride),
        uniform_texel_desc_stride(_heap_manager.uniform_texel_desc_stride),
        buffer_desc_stride(_heap_manager.buffer_desc_stride),
        image_desc_stride(_heap_manager.image_desc_stride),
        sample_desc_stride(_heap_manager.sample_desc_stride),
        accel_desc_stride(_heap_manager.accel_desc_stride) {

        m_offline_buffer_descs.stride  = buffer_desc_stride;
        m_offline_image_descs.stride   = image_desc_stride;
        m_offline_sampler_descs.stride = sample_desc_stride;
        m_offline_accel_descs.stride   = accel_desc_stride;

        m_offline_buffer_descs.bytes.resize(s_offline_buffer_desc_capacity * buffer_desc_stride);
        m_offline_image_descs.bytes.resize(s_offline_image_desc_capacity * image_desc_stride);
        m_offline_sampler_descs.bytes.resize(VulkanDevice::bindless_sampler_cnt * sample_desc_stride);
        m_offline_accel_descs.bytes.resize(s_offline_accel_desc_capacity * accel_desc_stride);

        m_offline_buffer_metadata.resize(s_offline_buffer_desc_capacity);
        m_offline_image_metadata.resize(s_offline_image_desc_capacity);
        m_offline_sampler_metadata.resize(VulkanDevice::bindless_sampler_cnt);
        m_offline_accel_metadata.resize(s_offline_accel_desc_capacity);

        InitializeImmutableSamplerMetadata();
    }

    uint GetBufferDescIdx(
        const BufferView& _in_buffer,
        VkDescriptorType  _type,
        VkFormat          _format = VK_FORMAT_UNDEFINED
    ) {
        assert(_in_buffer.GetBuffer() != nullptr && "buffer is nullptr");
        uint idx = 0;

        VulkanBuffer* vk_buffer = ResourceCast(_in_buffer.GetBuffer());
        auto&         indices   = vk_buffer->GetDescriptorIndices(_type);

        auto it = indices.find(_in_buffer.byte_offset);
        if (it != indices.end()) {
            return it->second * buffer_desc_stride;
        }

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
                buffer_info.format                         = _format;
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
                buffer_info.format                         = _format;
                buffer_desc_info.data.pUniformTexelBuffer = &buffer_info;
                desc_size                                 = uniform_texel_desc_stride;
                break;
            default:
                LOG_ERROR(
                    "Unsupported buffer descriptor type: {}",
                    VK_TYPE_TO_STRING(VkDescriptorType, _type)
                );
                assert(false && "Unsupported buffer descriptor type");
                return 0;
        }

        assert(
            device.GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize <=
            sizeof(BufferCpuDescHandle)
        );

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (m_offline_buffer_descs.free_list.empty()) {
                idx = m_offline_buffer_descs.next_offset / buffer_desc_stride;
                m_offline_buffer_descs.next_offset += buffer_desc_stride;
            } else {
                idx = m_offline_buffer_descs.free_list.back();
                m_offline_buffer_descs.free_list.pop_back();
            }

            indices[_in_buffer.byte_offset] = idx;

            VulkanOfflineBufferDescriptor& metadata = m_offline_buffer_metadata[idx];
            metadata.resource_info                  = {VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
            metadata.resource_info.type             = _type;
            metadata.address_range = {
                .address = vk_buffer->DeviceAddress() + _in_buffer.byte_offset,
                .size = _in_buffer.GetByteSize(),
            };
            if (_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || _type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
                metadata.texel_buffer_info = {VK_STRUCTURE_TYPE_TEXEL_BUFFER_DESCRIPTOR_INFO_EXT};
                metadata.texel_buffer_info.format        = _format;
                metadata.texel_buffer_info.addressRange  = metadata.address_range;
                metadata.resource_info.data.pTexelBuffer = &metadata.texel_buffer_info;
            } else {
                metadata.resource_info.data.pAddressRange = &metadata.address_range;
            }

            vkGetDescriptorEXT(
                device.GetDevice(),
                &buffer_desc_info,
                desc_size,
                m_offline_buffer_descs.bytes.data() + idx * buffer_desc_stride
            );
        }

        ReplicateOfflineBufferDescriptor(uint32(idx));
        return idx * buffer_desc_stride;
    }

    void FreeBufferDescIdx(uint _idx) {
        std::lock_guard<std::mutex> lock(mutex);
        m_offline_buffer_descs.free_list.push_back(_idx);
    }

    uint GetImageDescIdx(const TextureView* _in_image, VkImageLayout _layout) {
        auto* texture = ResourceCast(_in_image->texture);
        assert(texture != nullptr && "texture is nullptr");
        uint8 layer_count = _in_image->num_array == 0 ? 1 : _in_image->num_array;
        VkTextureDescKey key{_layout, _in_image->mip_level, _in_image->num_mips, _in_image->array_layer, layer_count};
        auto             res = texture->m_descriptor_indices.try_emplace(key, -1);
        if (!res.second) {
            return res.first->second * image_desc_stride;
        }

        auto& idx = res.first->second;
        {
            VkImageDescriptorInfoEXT    native_image_info{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
            VkResourceDescriptorInfoEXT native_resource_info{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
            texture->BuildNativeImageDescriptorInfo(*_in_image, _layout, native_image_info, native_resource_info);

            VkDescriptorImageInfo image_info{
                .imageView = texture->GetView(
                    _in_image->mip_level,
                    _in_image->num_mips,
                    _in_image->array_layer,
                    _in_image->num_array == 0 ? VK_REMAINING_ARRAY_LAYERS : _in_image->num_array
                ),
                .imageLayout = native_image_info.layout,
            };
            VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            desc_info.type = native_resource_info.type;
            if (desc_info.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                desc_info.data.pStorageImage = &image_info;
            } else {
                desc_info.data.pSampledImage = &image_info;
            }

            std::lock_guard<std::mutex> lock(mutex);
            if (m_offline_image_descs.free_list.empty()) {
                idx = m_offline_image_descs.next_offset / image_desc_stride;
                m_offline_image_descs.next_offset += image_desc_stride;
            } else {
                idx = m_offline_image_descs.free_list.back();
                m_offline_image_descs.free_list.pop_back();
            }

            VulkanOfflineImageDescriptor& metadata = m_offline_image_metadata[idx];
            metadata.image_info                    = native_image_info;
            metadata.resource_info                 = native_resource_info;
            metadata.resource_info.data.pImage     = &metadata.image_info;
            vkGetDescriptorEXT(
                device.GetDevice(),
                &desc_info,
                image_desc_stride,
                m_offline_image_descs.bytes.data() + idx * image_desc_stride
            );
        }

        ReplicateOfflineImageDescriptor(uint32(idx));
        return idx * image_desc_stride;
    }

    void FreeImageDescIdx(uint _idx) {
        std::lock_guard<std::mutex> lock(mutex);
        m_offline_image_descs.free_list.push_back(_idx);
    }

    uint GetSamplerDescIdx(Sampler _sampler) {
        return device.GetSamplerIdx(_sampler) * sample_desc_stride;
    }

    uint GetAccelDescIdx(VulkanAccelerationStructure* _as) {
        assert(_as != nullptr && "accel struct is nullptr");
        if (_as->m_descriptor_idx >= 0) {
            return _as->m_descriptor_idx * accel_desc_stride;
        }

        VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        desc_info.type                       = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        desc_info.data.accelerationStructure = _as->tlas_device_address;

        uint idx = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (m_offline_accel_descs.free_list.empty()) {
                idx = m_offline_accel_descs.next_offset / accel_desc_stride;
                m_offline_accel_descs.next_offset += accel_desc_stride;
            } else {
                idx = m_offline_accel_descs.free_list.back();
                m_offline_accel_descs.free_list.pop_back();
            }

            _as->m_descriptor_idx = idx;
            VulkanOfflineAccelDescriptor& metadata = m_offline_accel_metadata[idx];
            metadata.resource_info                 = {VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
            metadata.resource_info.type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            metadata.address_range                 = {.address = _as->tlas_device_address, .size = 0};
            metadata.resource_info.data.pAddressRange = &metadata.address_range;
            vkGetDescriptorEXT(
                device.GetDevice(),
                &desc_info,
                accel_desc_stride,
                m_offline_accel_descs.bytes.data() + idx * accel_desc_stride
            );
        }

        ReplicateOfflineAccelDescriptor(uint32(idx));
        return idx * accel_desc_stride;
    }

    uint FreeAccelDescIdx(uint _idx) {
        std::lock_guard<std::mutex> lock(mutex);
        m_offline_accel_descs.free_list.push_back(_idx);
        return 0;
    }

    void CopyOfflineBufferDesc(uint64 _src_offset, void* _dst, uint64 _size) const {
        CopyOfflineDescriptor(m_offline_buffer_descs, _src_offset, _dst, _size);
    }

    void CopyOfflineImageDesc(uint64 _src_offset, void* _dst, uint64 _size) const {
        CopyOfflineDescriptor(m_offline_image_descs, _src_offset, _dst, _size);
    }

    void CopyOfflineSamplerDesc(uint64 _src_offset, void* _dst, uint64 _size) const {
        CopyOfflineDescriptor(m_offline_sampler_descs, _src_offset, _dst, _size);
    }

    void CopyOfflineAccelDesc(uint64 _src_offset, void* _dst, uint64 _size) const {
        CopyOfflineDescriptor(m_offline_accel_descs, _src_offset, _dst, _size);
    }

    void WriteOfflineBufferDescriptor(uint64 _src_offset, void* _dst) const {
        const uint32 index = uint32(_src_offset / buffer_desc_stride);
        const VulkanOfflineBufferDescriptor& metadata = m_offline_buffer_metadata[index];
        VkHostAddressRangeEXT dst_range = MakeHostRange(_dst, device.GetPhysicalDescriptorSize(metadata.resource_info.type));
        VK_CHECK_RESULT(device.WriteResourceDescriptors(1, &metadata.resource_info, &dst_range));
    }

    void WriteOfflineImageDescriptor(uint64 _src_offset, void* _dst) const {
        const uint32 index = uint32(_src_offset / image_desc_stride);
        const VulkanOfflineImageDescriptor& metadata = m_offline_image_metadata[index];
        VkHostAddressRangeEXT dst_range = MakeHostRange(_dst, device.GetPhysicalDescriptorSize(metadata.resource_info.type));
        VK_CHECK_RESULT(device.WriteResourceDescriptors(1, &metadata.resource_info, &dst_range));
    }

    void WriteOfflineSamplerDescriptor(uint64 _src_offset, void* _dst) const {
        const uint32 index = uint32(_src_offset / sample_desc_stride);
        const VulkanOfflineSamplerDescriptor& metadata = m_offline_sampler_metadata[index];
        VkHostAddressRangeEXT dst_range = MakeHostRange(_dst, device.GetPhysicalDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER));
        VK_CHECK_RESULT(device.WriteSamplerDescriptors(1, &metadata.sampler_info, &dst_range));
    }

    void WriteOfflineAccelDescriptor(uint64 _src_offset, void* _dst) const {
        const uint32 index = uint32(_src_offset / accel_desc_stride);
        const VulkanOfflineAccelDescriptor& metadata = m_offline_accel_metadata[index];
        VkHostAddressRangeEXT dst_range = MakeHostRange(_dst, device.GetPhysicalDescriptorSize(metadata.resource_info.type));
        VK_CHECK_RESULT(device.WriteResourceDescriptors(1, &metadata.resource_info, &dst_range));
    }

    void PopulateResourceHeapReservedRange(VulkanResourceHeapStorage& _heap) const {
        memset(_heap.map_ptr, 0, heap_manager.resource_heap_reserved_size);

        const uint32 buffer_count = uint32(m_offline_buffer_descs.next_offset / buffer_desc_stride);
        for (uint32 idx = 0; idx < buffer_count; ++idx) {
            WriteOfflineBufferDescriptor(
                uint64(idx) * buffer_desc_stride,
                _heap.map_ptr + uint64(heap_manager.resource_heap_buffer_base_index + idx) * heap_manager.resource_heap_stride
            );
        }

        const uint32 image_count = uint32(m_offline_image_descs.next_offset / image_desc_stride);
        for (uint32 idx = 0; idx < image_count; ++idx) {
            WriteOfflineImageDescriptor(
                uint64(idx) * image_desc_stride,
                _heap.map_ptr + uint64(heap_manager.resource_heap_image_base_index + idx) * heap_manager.resource_heap_stride
            );
        }

        const uint32 accel_count = uint32(m_offline_accel_descs.next_offset / accel_desc_stride);
        for (uint32 idx = 0; idx < accel_count; ++idx) {
            WriteOfflineAccelDescriptor(
                uint64(idx) * accel_desc_stride,
                _heap.map_ptr + uint64(heap_manager.resource_heap_accel_base_index + idx) * heap_manager.resource_heap_stride
            );
        }
    }

    const Array<VulkanOfflineSamplerDescriptor>& GetOfflineSamplerMetadata() const {
        return m_offline_sampler_metadata;
    }

    uint32 GetBindlessBufferHeapIndex(uint64 _src_offset) const {
        return heap_manager.resource_heap_buffer_base_index + uint32(_src_offset / buffer_desc_stride);
    }

    uint32 GetBindlessImageHeapIndex(uint64 _src_offset) const {
        return heap_manager.resource_heap_image_base_index + uint32(_src_offset / image_desc_stride);
    }

    uint64 GetOfflineSamplerDescriptorStride() const {
        return sample_desc_stride;
    }

private:
    static void CopyOfflineDescriptor(
        const VulkanOfflineDescriptorArena& _arena,
        uint64                              _src_offset,
        void*                               _dst,
        uint64                              _size
    ) {
        memcpy(_dst, _arena.bytes.data() + _src_offset, _size);
    }

    VkSamplerCreateInfo BuildSamplerCreateInfo(Sampler _sampler) const {
        VkSamplerCreateInfo sampler_create_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_create_info.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        sampler_create_info.unnormalizedCoordinates = VK_FALSE;
        sampler_create_info.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_create_info.mipLodBias              = 0.0f;
        sampler_create_info.minLod                  = 0.0f;
        sampler_create_info.maxLod                  = VK_LOD_CLAMP_NONE;
        sampler_create_info.minFilter = sampler_create_info.magFilter =
            VulkanEnumTranslator::METoVKMinMagFilterMode(_sampler.filter);
        sampler_create_info.addressModeU = sampler_create_info.addressModeV = sampler_create_info.addressModeW =
            VulkanEnumTranslator::METoVKWrapMode(_sampler.address_mode);
        sampler_create_info.compareOp = VulkanEnumTranslator::METoVKCompareOp(ECompareOption(_sampler.compare_function));
        sampler_create_info.compareEnable = _sampler.compare_function != SCF_NEVER;
        return sampler_create_info;
    }

    void InitializeImmutableSamplerMetadata() {
        for (uint32_t i = 0; i < VulkanDevice::bindless_sampler_cnt; ++i) {
            if (i >= device.ImmutableSamplerCount()) {
                break;
            }
            VkDescriptorGetInfoEXT desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            VkSampler sampler       = device.GetImmutableSamplers()[i];
            desc_info.data.pSampler = &sampler;
            desc_info.type          = VK_DESCRIPTOR_TYPE_SAMPLER;
            m_offline_sampler_metadata[i].sampler_info = BuildSamplerCreateInfo(Sampler{
                ESamplerFilter(i % SF_Num),
                ESamplerAddressMode((i / SF_Num) % SAM_Num),
                ESamplerCompareFunction(i / (SAM_Num * uint(SF_Num))),
            });
            vkGetDescriptorEXT(
                device.GetDevice(),
                &desc_info,
                sample_desc_stride,
                m_offline_sampler_descs.bytes.data() + i * sample_desc_stride
            );
        }
    }

    void ReplicateOfflineBufferDescriptor(uint32 _idx) const {
        heap_manager.ForEachResourceHeap([&](VulkanResourceHeapStorage& heap) {
            const uint64 offset = uint64(heap_manager.resource_heap_buffer_base_index + _idx) * heap_manager.resource_heap_stride;
            WriteOfflineBufferDescriptor(uint64(_idx) * buffer_desc_stride, heap.map_ptr + offset);
            heap_manager.FlushMappedResourceHeapRange(heap, offset, heap_manager.resource_heap_stride);
        });
    }

    void ReplicateOfflineImageDescriptor(uint32 _idx) const {
        heap_manager.ForEachResourceHeap([&](VulkanResourceHeapStorage& heap) {
            const uint64 offset = uint64(heap_manager.resource_heap_image_base_index + _idx) * heap_manager.resource_heap_stride;
            WriteOfflineImageDescriptor(uint64(_idx) * image_desc_stride, heap.map_ptr + offset);
            heap_manager.FlushMappedResourceHeapRange(heap, offset, heap_manager.resource_heap_stride);
        });
    }

    void ReplicateOfflineAccelDescriptor(uint32 _idx) const {
        heap_manager.ForEachResourceHeap([&](VulkanResourceHeapStorage& heap) {
            const uint64 offset = uint64(heap_manager.resource_heap_accel_base_index + _idx) * heap_manager.resource_heap_stride;
            WriteOfflineAccelDescriptor(uint64(_idx) * accel_desc_stride, heap.map_ptr + offset);
            heap_manager.FlushMappedResourceHeapRange(heap, offset, heap_manager.resource_heap_stride);
        });
    }

private:
    VulkanDevice&      device;
    VulkanHeapManager& heap_manager;
    mutable std::mutex mutex;

    uint storage_desc_stride{0};
    uint uniform_desc_stride{0};
    uint storage_texel_desc_stride{0};
    uint uniform_texel_desc_stride{0};
    uint buffer_desc_stride{0};
    uint image_desc_stride{0};
    uint sample_desc_stride{0};
    uint accel_desc_stride{0};

    VulkanOfflineDescriptorArena          m_offline_buffer_descs;
    VulkanOfflineDescriptorArena          m_offline_image_descs;
    VulkanOfflineDescriptorArena          m_offline_sampler_descs;
    VulkanOfflineDescriptorArena          m_offline_accel_descs;
    Array<VulkanOfflineBufferDescriptor>  m_offline_buffer_metadata;
    Array<VulkanOfflineImageDescriptor>   m_offline_image_metadata;
    Array<VulkanOfflineSamplerDescriptor> m_offline_sampler_metadata;
    Array<VulkanOfflineAccelDescriptor>   m_offline_accel_metadata;
};

void VulkanHeapManager::InitializeSamplerHeap(const VulkanOfflineDescriptorManager& _offline_manager) {
    if (sampler_heap_buffer != nullptr) {
        return;
    }

    const auto& descriptor_heap_props = device.GetOptionalProperties().descriptor_heap_properties;

    VkBuffer           sampler_buffer            = VK_NULL_HANDLE;
    VmaAllocation      sampler_buffer_allocation = VK_NULL_HANDLE;
    VkBufferCreateInfo sampler_buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    sampler_buffer_ci.size = Moer::AlignUp(
        sampler_heap_reserved_size + uint64(VulkanDevice::bindless_sampler_cnt) * heap_sample_desc_stride,
        descriptor_heap_props.samplerHeapAlignment
    );
    sampler_buffer_ci.usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT |
                              VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK_RESULT(vmaCreateBuffer(
        device.GetVmaAllocator(),
        &sampler_buffer_ci,
        &alloc_ci,
        &sampler_buffer,
        &sampler_buffer_allocation,
        nullptr
    ));

    BufferInfo sampler_buffer_info{};
    sampler_buffer_info.size   = sampler_buffer_ci.size;
    sampler_buffer_info.stride = 1;
    sampler_buffer_info.usage  = EBufferUsageFlags::UNORDERED_ACCESS;
    sampler_heap_buffer        = MoerNew(VulkanBuffer)(
        s_sampler_heap_buffer_name,
        sampler_buffer_info,
        device,
        sampler_buffer,
        sampler_buffer_allocation,
        false,
        true
    );

    uint8* sampler_map_ptr = nullptr;
    vmaMapMemory(device.GetVmaAllocator(), sampler_heap_buffer->GetAllocation(), (void**)&sampler_map_ptr);
    memset(sampler_map_ptr, 0, sampler_heap_reserved_size);
    const auto& sampler_metadata = _offline_manager.GetOfflineSamplerMetadata();
    for (uint32_t i = 0; i < device.ImmutableSamplerCount(); ++i) {
        const VkSamplerCreateInfo& sampler_info = sampler_metadata[i].sampler_info;
        VkHostAddressRangeEXT dst_range = MakeHostRange(
            sampler_map_ptr + sampler_heap_reserved_size + uint64(i) * heap_sample_desc_stride,
            device.GetPhysicalDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER)
        );
        VK_CHECK_RESULT(device.WriteSamplerDescriptors(1, &sampler_info, &dst_range));
    }
    vmaFlushAllocation(
        device.GetVmaAllocator(),
        sampler_heap_buffer->GetAllocation(),
        0,
        sampler_buffer_ci.size
    );
    vmaUnmapMemory(device.GetVmaAllocator(), sampler_heap_buffer->GetAllocation());
}

void VulkanHeapManager::Initialize(const VulkanOfflineDescriptorManager& _offline_manager) {
    InitializeSamplerHeap(_offline_manager);
    if (resource_heaps.empty()) {
        CreateResourceHeap(_offline_manager);
    }
}

VulkanResourceHeapStorage& VulkanHeapManager::CreateResourceHeap(const VulkanOfflineDescriptorManager& _offline_manager) {
    VkBuffer           desc_buffer            = VK_NULL_HANDLE;
    VmaAllocation      desc_buffer_allocation = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_ci.size  = online_heap_size;
    buffer_ci.usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT |
                      VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

#if WITH_CUDA
    buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK_RESULT(vmaCreateBuffer(
        device.GetVmaAllocator(), &buffer_ci, &alloc_ci, &desc_buffer, &desc_buffer_allocation, nullptr
    ));

    BufferInfo buffer_info{};
    buffer_info.size   = buffer_ci.size;
    buffer_info.stride = 1;
    buffer_info.usage  = EBufferUsageFlags::UNORDERED_ACCESS;

    auto heap_storage  = MakeUnique<VulkanResourceHeapStorage>();
    heap_storage->size = buffer_ci.size;
    heap_storage->buffer = MoerNew(VulkanBuffer)(
        s_ring_desc_buffer_name,
        buffer_info,
        device,
        desc_buffer,
        desc_buffer_allocation,
        false,
        true
    );

    vmaMapMemory(device.GetVmaAllocator(), heap_storage->buffer->GetAllocation(), (void**)&heap_storage->map_ptr);
    _offline_manager.PopulateResourceHeapReservedRange(*heap_storage);
    vmaFlushAllocation(
        device.GetVmaAllocator(),
        heap_storage->buffer->GetAllocation(),
        0,
        resource_heap_reserved_size
    );

    std::lock_guard<std::mutex> lock(resource_heaps_mutex);
    heap_storage->heap_index = uint32(resource_heaps.size());
    resource_heaps.emplace_back(std::move(heap_storage));
    return *resource_heaps.back();
}

class VulkanOnlineResourceDescriptorManager {
public:
    VulkanOnlineResourceDescriptorManager(
        VulkanDevice&                   _device,
        VulkanHeapManager&              _heap_manager,
        VulkanOfflineDescriptorManager& _offline_manager
    ) :
        device(_device),
        heap_manager(_heap_manager),
        offline_manager(_offline_manager) {
        RegisterExistingHeaps();
    }

    VulkanDescriptorBinder BeginPushDescriptors() {
        const auto [heap_index, range] = AcquireRecordingRange();
        auto& heap_storage             = heap_manager.GetResourceHeap(heap_index);

        auto binder_state              = std::make_shared<VulkanDescriptorBinderState>();
        binder_state->heap_storage     = &heap_storage;
        binder_state->range_size       = range.size;
        binder_state->range_limit      = range.offset + range.size;
        binder_state->next_offset.store(range.offset, std::memory_order_release);

        assert(s_active_descriptor_binder == nullptr && "Descriptor binder is already active on this thread");
        s_active_descriptor_binder = binder_state.get();

        VulkanDescriptorBinder binder{};
        binder.state               = std::move(binder_state);
        binder.resource_heap_index = heap_index;
        binder.base_offset         = range.offset;
        binder.leased_size         = range.size;
        return binder;
    }

    VulkanDescriptorBinder EndPushDescriptors(VulkanDescriptorBinder _binder) {
        if (!_binder.IsValid()) {
            return _binder;
        }

        assert(s_active_descriptor_binder == _binder.state.get() && "Descriptor binder/thread mismatch");
        s_active_descriptor_binder = nullptr;

        VulkanDescriptorBinderState& state = *_binder.state;
        _binder.used_size = state.next_offset.load(std::memory_order_acquire) - _binder.base_offset;
        heap_manager.FlushResourceHeapRange(_binder.resource_heap_index, _binder.base_offset, _binder.used_size);
        return _binder;
    }

    void RecycleOnlineDescriptorLease(VulkanDescriptorBinder _binder) {
        if (!_binder.IsValid()) {
            return;
        }
        VulkanOnlineHeapState& heap = GetHeapState(_binder.resource_heap_index);
        std::lock_guard<std::mutex> lock(heap.mutex);
        InsertFreeRange(heap, VulkanDescriptorFreeRange{_binder.base_offset, _binder.leased_size});
    }

    uint64 AllocateOnlineDescriptorRange(uint64 _size) {
        VulkanDescriptorBinderState& binder = GetActiveBinder();
        uint64 current = binder.next_offset.load(std::memory_order_acquire);
        while (true) {
            const uint64 next = current + _size;
            if (next > binder.range_limit) {
                LOG_ERROR(
                    "Descriptor heap online range overflow: current={}, requested={}, limit={}",
                    current,
                    _size,
                    binder.range_limit
                );
                assert(false && "Descriptor heap online range overflow");
                return 0;
            }
                if (binder.next_offset.compare_exchange_weak(
                        current,
                        next,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire
                    )) {
                    return current;
                }
            }
        }

        VulkanBindlessRingCacheEntry CacheBindlessArray(const VulkanBindlessArray& _array) {
            VulkanDescriptorBinderState& binder = GetActiveBinder();
            const uint64 handle                 = uint64(&_array);
            const uint64 version                = _array.GetDescriptorVersion();
            std::lock_guard<std::mutex> lock(binder.bindless_cache_mutex);
            auto& entry = binder.bindless_ring_cache[handle];
            if (entry.version == version) {
                return entry;
            }

            const auto& heap_props         = device.GetOptionalProperties().descriptor_heap_properties;
            const uint64 buffer_alignment  = std::max(heap_props.bufferDescriptorAlignment, heap_props.resourceHeapAlignment);
            const uint64 image_alignment   = std::max(heap_props.imageDescriptorAlignment, heap_props.resourceHeapAlignment);
            const uint64 array_desc_stride = Moer::AlignUp(uint64(heap_manager.heap_buffer_desc_stride), buffer_alignment);
            const uint64 buffer_size       = Moer::AlignUp(uint64(_array.GetMaxSize()) * array_desc_stride, buffer_alignment);
            const uint64 texture_size      = Moer::AlignUp(
                uint64(_array.GetMaxSize()) * heap_manager.heap_image_desc_stride,
                image_alignment
            );

            entry.version      = version;
            entry.array_offset = AllocateOnlineDescriptorRange(array_desc_stride);
            memset(binder.heap_storage->map_ptr + entry.array_offset, 0, array_desc_stride);
            {
                VkResourceDescriptorInfoEXT resource_info{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
                VkDeviceAddressRangeEXT     address_range{
                    .address = _array.bindless_array_buffer->DeviceAddress(),
                    .size = _array.bindless_array_buffer->GetByteSize(),
                };
                resource_info.type               = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                resource_info.data.pAddressRange = &address_range;
                VkHostAddressRangeEXT dst_range = MakeHostRange(
                    binder.heap_storage->map_ptr + entry.array_offset,
                    device.GetPhysicalDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                );
                VK_CHECK_RESULT(device.WriteResourceDescriptors(1, &resource_info, &dst_range));
            }

            entry.buffer_offset = AllocateOnlineDescriptorRange(buffer_size);
            memset(binder.heap_storage->map_ptr + entry.buffer_offset, 0, buffer_size);
            entry.texture_offset = AllocateOnlineDescriptorRange(texture_size);
            memset(binder.heap_storage->map_ptr + entry.texture_offset, 0, texture_size);

            const auto& handles          = _array.handles;
            const auto& resource_offsets = _array.GetOfflineResourceDescriptorOffsets();
            for (uint32 array_index = 0; array_index < handles.size(); ++array_index) {
                const auto& bindless_handle = handles[array_index];
                if (bindless_handle.slot == 0) {
                    continue;
                }
                if (bindless_handle.type == VulkanBindlessArray::Buffer) {
                    offline_manager.WriteOfflineBufferDescriptor(
                        resource_offsets[array_index],
                        binder.heap_storage->map_ptr + entry.buffer_offset + uint64(bindless_handle.slot) * array_desc_stride
                    );
                    continue;
                }
                if (bindless_handle.type == VulkanBindlessArray::Texture) {
                    offline_manager.WriteOfflineImageDescriptor(
                        resource_offsets[array_index],
                        binder.heap_storage->map_ptr + entry.texture_offset +
                            uint64(bindless_handle.slot) * heap_manager.heap_image_desc_stride
                    );
                }
            }

            return entry;
        }

        void PushUniformDesc(uint64 _src_offset, uint64 _set_offset) {
            VulkanDescriptorBinderState& binder = GetActiveBinder();
            offline_manager.WriteOfflineBufferDescriptor(_src_offset, binder.heap_storage->map_ptr + _set_offset);
        }

        void PushStorageDesc(uint64 _src_offset, uint64 _set_offset) {
            VulkanDescriptorBinderState& binder = GetActiveBinder();
            offline_manager.WriteOfflineBufferDescriptor(_src_offset, binder.heap_storage->map_ptr + _set_offset);
        }

        void PushImageDesc(uint64 _src_offset, uint64 _set_offset) {
            VulkanDescriptorBinderState& binder = GetActiveBinder();
            offline_manager.WriteOfflineImageDescriptor(_src_offset, binder.heap_storage->map_ptr + _set_offset);
        }

        void PushSamplerDesc(uint64 _src_offset, uint64 _set_offset) {
            VulkanDescriptorBinderState& binder = GetActiveBinder();
            offline_manager.WriteOfflineSamplerDescriptor(_src_offset, binder.heap_storage->map_ptr + _set_offset);
        }

        void PushAccelDesc(uint64 _src_offset, uint64 _set_offset) {
            VulkanDescriptorBinderState& binder = GetActiveBinder();
            offline_manager.WriteOfflineAccelDescriptor(_src_offset, binder.heap_storage->map_ptr + _set_offset);
        }

        VkBindHeapInfoEXT GetResourceHeapBindInfo() const {
            const VulkanDescriptorBinderState& binder = GetActiveBinder();
            return heap_manager.MakeResourceHeapBindInfo(binder.heap_storage->heap_index);
        }

    private:
        struct VulkanOnlineHeapState {
            uint32                           heap_index{UINT32_MAX};
            std::mutex                       mutex;
            Array<VulkanDescriptorFreeRange> free_ranges{};
        };

        void RegisterExistingHeaps() {
            heap_manager.ForEachResourceHeap([&](VulkanResourceHeapStorage& heap) {
                auto state        = MakeUnique<VulkanOnlineHeapState>();
                state->heap_index = heap.heap_index;
                state->free_ranges.push_back(
                    VulkanDescriptorFreeRange{
                        heap_manager.resource_heap_reserved_size,
                        heap.size - heap_manager.resource_heap_reserved_size,
                    }
                );
                heap_states.emplace_back(std::move(state));
            });
        }

        std::pair<uint32, VulkanDescriptorFreeRange> AcquireRecordingRange() {
            for (const auto& heap_state_ptr : heap_states) {
                VulkanOnlineHeapState& heap = *heap_state_ptr;
                std::lock_guard<std::mutex> lock(heap.mutex);
                if (heap.free_ranges.empty()) {
                    continue;
                }
                VulkanDescriptorFreeRange range = heap.free_ranges.back();
                heap.free_ranges.pop_back();
                return {heap.heap_index, range};
            }

            VulkanResourceHeapStorage& new_heap = heap_manager.CreateResourceHeap(offline_manager);
            auto new_state                      = MakeUnique<VulkanOnlineHeapState>();
            new_state->heap_index              = new_heap.heap_index;
            new_state->free_ranges.push_back(
                VulkanDescriptorFreeRange{
                    heap_manager.resource_heap_reserved_size,
                    new_heap.size - heap_manager.resource_heap_reserved_size,
                }
            );
            VulkanOnlineHeapState& new_state_ref = *new_state;
            heap_states.emplace_back(std::move(new_state));

            std::lock_guard<std::mutex> lock(new_state_ref.mutex);
            VulkanDescriptorFreeRange range = new_state_ref.free_ranges.back();
            new_state_ref.free_ranges.pop_back();
            return {new_state_ref.heap_index, range};
        }

        VulkanOnlineHeapState& GetHeapState(uint32 _heap_index) {
            auto it = std::find_if(heap_states.begin(), heap_states.end(), [&](const auto& state) {
                return state != nullptr && state->heap_index == _heap_index;
            });
            assert(it != heap_states.end() && "Descriptor heap state is missing");
            return **it;
        }

        static void InsertFreeRange(VulkanOnlineHeapState& _heap, VulkanDescriptorFreeRange _range) {
            _heap.free_ranges.push_back(_range);
            std::sort(
                _heap.free_ranges.begin(),
                _heap.free_ranges.end(),
                [](const VulkanDescriptorFreeRange& lhs, const VulkanDescriptorFreeRange& rhs) {
                    return lhs.offset < rhs.offset;
                }
            );

            Array<VulkanDescriptorFreeRange> merged{};
            merged.reserve(_heap.free_ranges.size());
            for (const VulkanDescriptorFreeRange& range : _heap.free_ranges) {
                if (merged.empty()) {
                    merged.push_back(range);
                    continue;
                }

                VulkanDescriptorFreeRange& back = merged.back();
                if (back.offset + back.size >= range.offset) {
                    const uint64 merged_end = std::max(back.offset + back.size, range.offset + range.size);
                    back.size               = merged_end - back.offset;
                    continue;
                }
                merged.push_back(range);
            }
            _heap.free_ranges = std::move(merged);
        }

        static VulkanDescriptorBinderState& GetActiveBinder() {
            assert(s_active_descriptor_binder != nullptr && "Descriptor binder is not active on this thread");
            return *s_active_descriptor_binder;
        }

    private:
        VulkanDevice&                            device;
        VulkanHeapManager&                       heap_manager;
        VulkanOfflineDescriptorManager&          offline_manager;
        Array<UniquePtr<VulkanOnlineHeapState>>  heap_states{};
    };

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

    VulkanDescriptorHeap::VulkanDescriptorHeap() = default;

    VulkanDescriptorHeap::VulkanDescriptorHeap(VulkanDevice& _device) : m_device(&_device) {
        m_heap_manager    = MakeUnique<VulkanHeapManager>(_device);
        m_offline_manager = MakeUnique<VulkanOfflineDescriptorManager>(_device, *m_heap_manager);
        m_heap_manager->Initialize(*m_offline_manager);
        m_online_manager  = MakeUnique<VulkanOnlineResourceDescriptorManager>(
            _device,
            *m_heap_manager,
            *m_offline_manager
        );
    }

    VulkanDescriptorHeap::~VulkanDescriptorHeap() {
        m_online_manager.reset();
        m_offline_manager.reset();
        m_heap_manager.reset();
    }

    uint VulkanDescriptorHeap::GetBufferDescIdx(
        const BufferView& _in_buffer,
        VkDescriptorType  _type,
        VkFormat          _format
    ) {
        return m_offline_manager->GetBufferDescIdx(_in_buffer, _type, _format);
    }

    void VulkanDescriptorHeap::FreeBufferDescIdx(uint _idx) {
        m_offline_manager->FreeBufferDescIdx(_idx);
    }

    uint VulkanDescriptorHeap::GetImageDescIdx(const TextureView* _in_image, VkImageLayout _layout) {
        return m_offline_manager->GetImageDescIdx(_in_image, _layout);
    }

    void VulkanDescriptorHeap::FreeImageDescIdx(uint _idx) {
        m_offline_manager->FreeImageDescIdx(_idx);
    }

    uint VulkanDescriptorHeap::GetSamplerDescIdx(Sampler _sampler) {
        return m_offline_manager->GetSamplerDescIdx(_sampler);
    }

    uint VulkanDescriptorHeap::GetAccelDescIdx(VulkanAccelerationStructure* _as) {
        return m_offline_manager->GetAccelDescIdx(_as);
    }

    uint VulkanDescriptorHeap::FreeAccelDescIdx(uint _idx) {
        return m_offline_manager->FreeAccelDescIdx(_idx);
    }

    void VulkanDescriptorHeap::CopyOfflineBufferDesc(uint64 _src_offset, void* _dst, uint64 _size) const {
        m_offline_manager->CopyOfflineBufferDesc(_src_offset, _dst, _size);
    }

    void VulkanDescriptorHeap::CopyOfflineImageDesc(uint64 _src_offset, void* _dst, uint64 _size) const {
        m_offline_manager->CopyOfflineImageDesc(_src_offset, _dst, _size);
    }

    void VulkanDescriptorHeap::CopyOfflineSamplerDesc(uint64 _src_offset, void* _dst, uint64 _size) const {
        m_offline_manager->CopyOfflineSamplerDesc(_src_offset, _dst, _size);
    }

    void VulkanDescriptorHeap::CopyOfflineAccelDesc(uint64 _src_offset, void* _dst, uint64 _size) const {
        m_offline_manager->CopyOfflineAccelDesc(_src_offset, _dst, _size);
    }

    VulkanDescriptorBinder VulkanDescriptorHeap::BeginPushDescriptors() {
        return m_online_manager->BeginPushDescriptors();
    }

    VulkanDescriptorBinder VulkanDescriptorHeap::EndPushDescriptors(VulkanDescriptorBinder _binder) {
        return m_online_manager->EndPushDescriptors(std::move(_binder));
    }

    void VulkanDescriptorHeap::RecycleOnlineDescriptorLease(VulkanDescriptorBinder _binder) {
        m_online_manager->RecycleOnlineDescriptorLease(std::move(_binder));
    }

    uint64 VulkanDescriptorHeap::AllocateOnlineDescriptorRange(uint64 _size) {
        return m_online_manager->AllocateOnlineDescriptorRange(_size);
    }

    VulkanBindlessRingCacheEntry VulkanDescriptorHeap::CacheBindlessArray(const VulkanBindlessArray& _array) {
        return m_online_manager->CacheBindlessArray(_array);
    }

    void VulkanDescriptorHeap::PushUniformDesc(uint64 _src_offset, uint64 _set_offset) {
        m_online_manager->PushUniformDesc(_src_offset, _set_offset);
    }

    void VulkanDescriptorHeap::PushStorageDesc(uint64 _src_offset, uint64 _set_offset) {
        m_online_manager->PushStorageDesc(_src_offset, _set_offset);
    }

    void VulkanDescriptorHeap::PushImageDesc(uint64 _src_offset, uint64 _set_offset) {
        m_online_manager->PushImageDesc(_src_offset, _set_offset);
    }

    void VulkanDescriptorHeap::PushSamplerDesc(uint64 _src_offset, uint64 _set_offset) {
        m_online_manager->PushSamplerDesc(_src_offset, _set_offset);
    }

    void VulkanDescriptorHeap::PushAccelDesc(uint64 _src_offset, uint64 _set_offset) {
        m_online_manager->PushAccelDesc(_src_offset, _set_offset);
    }

    VkBindHeapInfoEXT VulkanDescriptorHeap::GetResourceHeapBindInfo() const {
        return m_online_manager->GetResourceHeapBindInfo();
    }

    VkBindHeapInfoEXT VulkanDescriptorHeap::GetSamplerHeapBindInfo() const {
        return m_heap_manager->MakeSamplerHeapBindInfo();
    }

    uint64 VulkanDescriptorHeap::GetBindlessArrayDescriptorOffset() const {
        return 0;
    }

    uint64 VulkanDescriptorHeap::GetBindlessBufferDescriptorStride() const {
        return GetOnlineResourceDescriptorStride(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }

    uint64 VulkanDescriptorHeap::GetBindlessImageDescriptorStride() const {
        return GetOnlineResourceDescriptorStride(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    }

    uint64 VulkanDescriptorHeap::GetBindlessSamplerDescriptorOffset(uint32 _sampler_idx) const {
        return m_heap_manager->sampler_heap_reserved_size + uint64(_sampler_idx) * Moer::AlignUp(
            GetPhysicalDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER),
            m_device->GetOptionalProperties().descriptor_heap_properties.samplerDescriptorAlignment
        );
    }

    uint64 VulkanDescriptorHeap::GetBindlessSamplerHeapBaseOffset() const {
        return m_heap_manager->sampler_heap_reserved_size;
    }

    uint32 VulkanDescriptorHeap::GetBindlessBufferHeapIndex(uint64 _src_offset) const {
        return m_offline_manager->GetBindlessBufferHeapIndex(_src_offset);
    }

    uint32 VulkanDescriptorHeap::GetBindlessImageHeapIndex(uint64 _src_offset) const {
        return m_offline_manager->GetBindlessImageHeapIndex(_src_offset);
    }

    uint64 VulkanDescriptorHeap::GetPhysicalDescriptorSize(VkDescriptorType _type) const {
        return m_device->GetPhysicalDescriptorSize(_type);
    }

    uint64 VulkanDescriptorHeap::GetOfflineSamplerDescriptorStride() const {
        return m_offline_manager->GetOfflineSamplerDescriptorStride();
    }

    uint64 VulkanDescriptorHeap::GetOnlineResourceDescriptorStride(VkDescriptorType _type) const {
        switch (_type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                return Moer::AlignUp(GetPhysicalDescriptorSize(_type), GetOnlineResourceDescriptorAlignment(_type));
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                return Moer::AlignUp(GetPhysicalDescriptorSize(_type), GetOnlineResourceDescriptorAlignment(_type));
            default:
                LOG_ERROR("Unsupported heap descriptor stride type: {}", VK_TYPE_TO_STRING(VkDescriptorType, _type));
                assert(false && "Unsupported heap descriptor stride type");
                return 0;
        }
    }

    uint64 VulkanDescriptorHeap::GetOnlineResourceDescriptorAlignment(VkDescriptorType _type) const {
        switch (_type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                return m_device->GetOptionalProperties().descriptor_heap_properties.bufferDescriptorAlignment;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                return m_device->GetOptionalProperties().descriptor_heap_properties.imageDescriptorAlignment;
            default:
                LOG_ERROR("Unsupported heap descriptor alignment type: {}", VK_TYPE_TO_STRING(VkDescriptorType, _type));
                assert(false && "Unsupported heap descriptor alignment type");
                return 0;
        }
    }
    } // namespace Moer::Render
