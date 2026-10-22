#pragma once

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

// Recorders share allocation state; the completion batch owns retirement.
struct VulkanDescriptorBufferLease {
    std::shared_ptr<VulkanDescriptorBufferLeaseState> state;

    bool IsValid() const {
        return state != nullptr;
    }
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

    // GetOrCreate returns a byte offset into the CPU cache. Resource objects
    // retain slot indices for release; a byte offset is not a slot index.
    uint GetOrCreateBufferDescriptorOffset(
        const BufferView& _buffer,
        VkDescriptorType  _type,
        VkFormat          _format = VK_FORMAT_UNDEFINED
    );
    uint GetOrCreateImageDescriptorOffset(
        const TextureView* _image,
        VkImageLayout      _layout,
        VkDescriptorType   _descriptor_type = VK_DESCRIPTOR_TYPE_MAX_ENUM
    );
    uint
    GetOrCreateAccelerationStructureDescriptorOffset(VulkanAccelerationStructure* _acceleration_structure);
    uint GetSamplerDescriptorOffset(Sampler _sampler) const;

    void FreeBufferDescriptorSlot(uint _slot_index);
    void FreeImageDescriptorSlot(uint _slot_index);
    void FreeAccelerationStructureDescriptorSlot(uint _slot_index);

    // Copy one cached descriptor to bindless update data or to a reserved GPU
    // descriptor-buffer range. The owning resource must remain alive.
    void CopyBufferDescriptorToMemory(uint64 _source_byte_offset, std::span<byte> _destination);
    void CopyImageDescriptorToMemory(uint64 _source_byte_offset, std::span<byte> _destination);
    void CopyBufferDescriptorToBuffer(uint64 _source_byte_offset, uint64 _destination_byte_offset);
    void CopyImageDescriptorToBuffer(uint64 _source_byte_offset, uint64 _destination_byte_offset);
    void CopySamplerDescriptorToBuffer(uint64 _source_byte_offset, uint64 _destination_byte_offset);
    void
    CopyAccelerationStructureDescriptorToBuffer(uint64 _source_byte_offset, uint64 _destination_byte_offset);

    // Creation reserves no storage. Flush after recorders join; recycle only
    // after GPU completion or after all referencing commands are discarded.
    VulkanDescriptorBufferLease CreateDescriptorBufferLease();
    void                        FlushDescriptorWrites(const VulkanDescriptorBufferLease& _lease);
    void                        RecycleDescriptorBufferLease(VulkanDescriptorBufferLease _lease);

    // Reserve an immutable range for one pipeline bind. The caller supplies a
    // size aligned for set offsets; concurrent recorders never share a range.
    std::optional<uint64> ReserveDescriptorRange(
        const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease,
        uint64                                                   _byte_size
    );
    uint64 GetUsedDescriptorBytes(const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease) const;

    VulkanDescriptorAllocationCheckpoint
    CreateDescriptorCheckpoint(const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease) const;

    // All recorders using this lease must have joined and discarded their work
    // before restoring allocation state for serial fallback.
    void RewindDescriptorWrites(
        const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease,
        const VulkanDescriptorAllocationCheckpoint&              _checkpoint
    );

private:
    void InitializeDescriptorCaches();
    void InitializeSamplerDescriptors();
    void CreateDescriptorBuffer();
    void MapDescriptorBuffer();

    // Call with cache_mutex held; views never escape the descriptor copy.
    std::span<const byte> GetCachedBufferDescriptor(uint64 _byte_offset) const;
    std::span<const byte> GetCachedImageDescriptor(uint64 _byte_offset) const;
    void                  CopyDescriptorToBuffer(std::span<const byte> _descriptor, uint64 _byte_offset);

    VulkanDevice* m_device;

    // Actual descriptor sizes returned by the device.
    uint storage_buffer_descriptor_size;
    uint uniform_buffer_descriptor_size;
    uint storage_texel_buffer_descriptor_size;
    uint uniform_texel_buffer_descriptor_size;
    uint sampled_image_descriptor_size;
    uint storage_image_descriptor_size;
    uint sampler_descriptor_size;
    uint acceleration_structure_descriptor_size;

    // CPU caches use fixed-size slots; the slot stride may exceed the size of
    // a particular descriptor type. Samplers precede images in their cache.
    uint                    buffer_cache_slot_stride;
    uint                    image_cache_slot_stride;
    uint64                  image_cache_base_byte_offset;
    Array<byte>             buffer_descriptor_cache;
    Array<byte>             sampler_and_image_descriptor_cache;
    Array<byte>             acceleration_structure_descriptor_cache;
    Array<VkDescriptorType> buffer_descriptor_types;
    Array<VkDescriptorType> image_descriptor_types;
    Array<uint>             free_buffer_slots;
    Array<uint>             free_image_slots;
    Array<uint>             free_acceleration_structure_slots;
    uint64                  next_buffer_cache_byte_offset{0};
    uint64                  next_image_cache_byte_offset{0};
    uint64                  next_acceleration_structure_cache_byte_offset{0};
    std::mutex              cache_mutex;

    // Owned GPU storage; destroyed before the device's VMA allocator.
    std::unique_ptr<VulkanBuffer> descriptor_buffer;
    uint8*                        mapped_descriptor_data{nullptr};

    // Graphics and compute share free storage; completion owns each lease's lifetime.
    VulkanDescriptorBufferAllocator descriptor_allocator;
};

} // namespace Moer::Render
