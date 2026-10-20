#pragma once

#include "VulkanCommon.h"
#include "misc/STL.h"
#include "misc/Traits.h"

#include <atomic>
#include <condition_variable>
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
enum class EQueueType : uint8;

// Recorders share the allocation cursor; the submission owns slot retirement.
struct VulkanDescriptorBufferLeaseState {
    VulkanDescriptorBufferLeaseState(uint32 _slot_index, uint64 _begin_byte_offset, uint64 _end_byte_offset) :
        slot_index(_slot_index),
        begin_byte_offset(_begin_byte_offset),
        end_byte_offset(_end_byte_offset),
        next_byte_offset(_begin_byte_offset) {}

    const uint32        slot_index;
    const uint64        begin_byte_offset;
    const uint64        end_byte_offset;
    std::atomic<uint64> next_byte_offset;
};

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

    // Acquisition waits for a free slot in this queue's partition. Flush makes
    // writes visible; recycle only after GPU completion or discarded recording.
    VulkanDescriptorBufferLease AcquireDescriptorBufferLease(EQueueType _queue_type);
    void                        FlushDescriptorWrites(const VulkanDescriptorBufferLease& _lease);
    void                        RecycleDescriptorBufferLease(VulkanDescriptorBufferLease _lease);

    // Reserve an immutable range for one pipeline bind. The caller supplies a
    // size aligned for set offsets; concurrent recorders never share a range.
    std::optional<uint64> ReserveDescriptorRange(
        const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease,
        uint64                                                   _byte_size
    );
    uint64 GetDescriptorWriteOffset(const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease) const;

    // All recorders using this lease must have joined and discarded their work
    // before rolling the cursor back for serial fallback.
    void RewindDescriptorWrites(
        const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease,
        uint64                                                   _byte_offset
    );

private:
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
    VulkanBuffer* descriptor_buffer{nullptr};
    uint8*        mapped_descriptor_data{nullptr};

    // Submission leases partition storage between graphics and compute.
    Array<uint64>                                            lease_slot_byte_offsets;
    Array<std::shared_ptr<VulkanDescriptorBufferLeaseState>> active_leases;
    StaticArray<uint32, 2>                                   next_queue_slots{};
    std::mutex                                               lease_mutex;
    std::condition_variable                                  lease_available;
};

} // namespace Moer::Render
