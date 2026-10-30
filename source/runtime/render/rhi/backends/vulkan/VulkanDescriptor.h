#pragma once

#include "RenderAPI.h"
#include "VulkanCommon.h"
#include "VulkanDescriptorBufferAllocator.h"
#include "VulkanDescriptorMode.h"
#include "VulkanRaytracingResource.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "misc/Types.h"

#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace Moer::Render {
class VulkanDevice;
class VulkanBuffer;
class VulkanAccelerationStructure;
class BufferView;
struct TextureView;
struct Sampler;

// Byte offset of the image table after the shared sampler table.
uint64 GetBindlessTextureDescriptorOffset(const VulkanDevice& device);

// Non-owning location in the CPU cache. The resource must outlive every copy.
struct VulkanCachedDescriptor {
    VkDescriptorType type       = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32           slot_index = 0;
};

struct VulkanDescriptorHeapFunctions {
    PFN_vkWriteResourceDescriptorsEXT write_resources = nullptr;
    PFN_vkWriteSamplerDescriptorsEXT  write_samplers  = nullptr;
    PFN_vkCmdBindResourceHeapEXT      bind_resources  = nullptr;
    PFN_vkCmdBindSamplerHeapEXT       bind_samplers   = nullptr;
    PFN_vkCmdPushDataEXT              push_data       = nullptr;
};

class VulkanDescriptorHeap {
public:
    explicit VulkanDescriptorHeap(VulkanDevice& _device);
    ~VulkanDescriptorHeap();

    VulkanDescriptorHeap(const VulkanDescriptorHeap&)            = delete;
    VulkanDescriptorHeap& operator=(const VulkanDescriptorHeap&) = delete;
    VulkanDescriptorHeap(VulkanDescriptorHeap&&)                 = delete;
    VulkanDescriptorHeap& operator=(VulkanDescriptorHeap&&)      = delete;

    // Native descriptor size and GPU storage address.
    uint32          GetDescriptorSize(VkDescriptorType _type) const;
    VkDeviceAddress GetDescriptorBufferAddress() const;

    VulkanBuffer* GetResourceHeapBuffer() const {
        return descriptor_buffer.get();
    }
    const VkBindHeapInfoEXT& GetResourceHeapBinding() const {
        return resource_heap_binding;
    }
    const VkBindHeapInfoEXT& GetSamplerHeapBinding() const {
        return sampler_heap_binding;
    }
    RENDER_API uint64 GetStorageAlignment() const;
    uint64 GetBindlessSamplerSetSize() const {
        return bindless_sampler_set_size;
    }
    uint64 GetBindlessSamplerBindingOffset() const {
        return bindless_sampler_binding_offset;
    }

    // Resource objects retain slot indices for release. Cache byte offsets
    // stay internal; GPU destination offsets are supplied only when copying.
    VulkanCachedDescriptor GetOrCreateBufferDescriptor(
        const BufferView& _buffer,
        VkDescriptorType  _type,
        VkFormat          _format = VK_FORMAT_UNDEFINED
    );
    VulkanCachedDescriptor GetOrCreateImageDescriptor(
        const TextureView* _image,
        VkImageLayout      _layout,
        VkDescriptorType   _descriptor_type = VK_DESCRIPTOR_TYPE_MAX_ENUM
    );
    VulkanCachedDescriptor
    GetOrCreateAccelerationStructureDescriptor(VulkanAccelerationStructure* _acceleration_structure);
    VulkanCachedDescriptor GetSamplerDescriptor(Sampler _sampler) const;

    void FreeBufferDescriptorSlot(uint32 _slot_index);
    void FreeImageDescriptorSlot(uint32 _slot_index);
    void FreeAccelerationStructureDescriptorSlot(uint32 _slot_index);

    // Copy one cached descriptor to bindless update data or to a reserved GPU
    // descriptor-buffer range. The owning resource must remain alive.
    void CopyDescriptorToMemory(VulkanCachedDescriptor _descriptor, std::span<byte> _destination);
    void CopyDescriptorToBuffer(VulkanCachedDescriptor _descriptor, uint64 _destination_byte_offset);

    // Creation reserves no storage. Flush after recorders join; recycle only
    // after GPU completion or after all referencing commands are discarded.
    RENDER_API VulkanDescriptorBufferLease CreateDescriptorBufferLease();
    void                        FlushDescriptorWrites(const VulkanDescriptorBufferLease& _lease);
    RENDER_API void RecycleDescriptorBufferLease(VulkanDescriptorBufferLease _lease);

    // Persistent bindless tables retain a contiguous range until their own
    // lease retires. Transient binds use PrepareDescriptorRecordings instead.
    RENDER_API std::optional<uint64>
           ReserveDescriptorRange(const VulkanDescriptorBufferLease& _lease, uint64 _byte_size);
    uint64 GetUsedDescriptorBytes(const VulkanDescriptorBufferLease& _lease) const;

    RENDER_API std::optional<std::vector<VulkanDescriptorBufferAllocator::Recording>> PrepareDescriptorRecordings(
        const VulkanDescriptorBufferLease&   _lease,
        std::span<const std::vector<uint64>> _requests
    );

private:
    // Fixed-size CPU slots. All mutations are protected by cache_mutex.
    struct DescriptorCache {
        explicit DescriptorCache(uint32 _slot_stride);
        uint32 AllocateSlot();

        uint32        slot_stride;
        Array<byte> bytes;
        Array<uint32> free_slots;
        uint32        next_slot{0};
    };

    void InitializeSamplerDescriptors();
    void InitializeSamplerSetLayout();
    void CreateNativeHeaps();
    void CreateDescriptorBuffer();
    void MapDescriptorBuffer();

    // Call with cache_mutex held; views never escape the descriptor copy.
    std::span<const byte> GetCachedDescriptor(VulkanCachedDescriptor _descriptor) const;

    VulkanDevice* m_device;

    // Resource caches have reusable slots; immutable samplers need no allocator.
    // Buffer/image slots can contain different descriptor types and sizes.
    DescriptorCache         buffer_cache;
    Array<VkDescriptorType> buffer_descriptor_types;
    DescriptorCache         image_cache;
    Array<VkDescriptorType> image_descriptor_types;
    DescriptorCache         acceleration_structure_cache;
    Array<byte>             sampler_descriptors;
    uint64                        bindless_sampler_set_size       = 0;
    uint64                        bindless_sampler_binding_offset = 0;
    std::mutex              cache_mutex;
    std::unique_ptr<VulkanBuffer> sampler_heap;
    VkBindHeapInfoEXT             resource_heap_binding{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    VkBindHeapInfoEXT             sampler_heap_binding{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};

    // Owned GPU storage; destroyed before the device's VMA allocator.
    std::unique_ptr<VulkanBuffer> descriptor_buffer;
    uint8*                        mapped_descriptor_data{nullptr};

    // Graphics and compute share free storage; completion owns each lease's lifetime.
    VulkanDescriptorBufferAllocator descriptor_allocator;
};

} // namespace Moer::Render
