#pragma once

#include "VulkanRaytracingResource.h"
#include "VulkanCommon.h"
#include "VulkanDescriptorBufferAllocator.h"
#include "misc/STL.h"
#include "misc/Traits.h"

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
    uint             slot_index = 0;
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
    uint            GetDescriptorSize(VkDescriptorType _type) const;
    VkDeviceAddress GetDescriptorBufferAddress() const;

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

    void FreeBufferDescriptorSlot(uint _slot_index);
    void FreeImageDescriptorSlot(uint _slot_index);
    void FreeAccelerationStructureDescriptorSlot(uint _slot_index);

    // Copy one cached descriptor to bindless update data or to a reserved GPU
    // descriptor-buffer range. The owning resource must remain alive.
    void CopyDescriptorToMemory(VulkanCachedDescriptor _descriptor, std::span<byte> _destination);
    void CopyDescriptorToBuffer(VulkanCachedDescriptor _descriptor, uint64 _destination_byte_offset);

    // Creation reserves no storage. Flush after recorders join; recycle only
    // after GPU completion or after all referencing commands are discarded.
    VulkanDescriptorBufferLease CreateDescriptorBufferLease();
    void                        FlushDescriptorWrites(const VulkanDescriptorBufferLease& _lease);
    void                        RecycleDescriptorBufferLease(VulkanDescriptorBufferLease _lease);

    // Reserve an immutable range for one pipeline bind. The caller supplies a
    // size aligned for set offsets; concurrent recorders never share a range.
    std::optional<uint64>
           ReserveDescriptorRange(const VulkanDescriptorBufferLease& _lease, uint64 _byte_size);
    uint64 GetUsedDescriptorBytes(const VulkanDescriptorBufferLease& _lease) const;

    VulkanDescriptorAllocationCheckpoint
    CreateDescriptorCheckpoint(const VulkanDescriptorBufferLease& _lease) const;

    // All recorders using this lease must have joined and discarded their work
    // before restoring allocation state for serial fallback.
    void RewindDescriptorWrites(
        const VulkanDescriptorBufferLease&          _lease,
        const VulkanDescriptorAllocationCheckpoint& _checkpoint
    );

private:
    // Fixed-size CPU slots. All mutations are protected by cache_mutex.
    struct DescriptorCache {
        explicit DescriptorCache(uint _slot_stride);
        uint AllocateSlot();

        uint        slot_stride;
        Array<byte> bytes;
        Array<uint> free_slots;
        uint        next_slot{0};
    };

    void InitializeSamplerDescriptors();
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
    std::mutex              cache_mutex;

    // Owned GPU storage; destroyed before the device's VMA allocator.
    std::unique_ptr<VulkanBuffer> descriptor_buffer;
    uint8*                        mapped_descriptor_data{nullptr};

    // Graphics and compute share free storage; completion owns each lease's lifetime.
    VulkanDescriptorBufferAllocator descriptor_allocator;
};

} // namespace Moer::Render
