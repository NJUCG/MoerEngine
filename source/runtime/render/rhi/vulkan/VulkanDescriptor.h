#pragma once

#include "VulkanCommon.h"
#include "misc/STL.h"
#include "misc/Traits.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace Moer::Render {
class VulkanDevice;
class VulkanBuffer;
class VulkanAccelerationStructure;
class BufferView;
struct TextureView;
struct Sampler;
enum class EQueueType : uint8;

struct VulkanDescriptorBufferLeaseState {
    VulkanDescriptorBufferLeaseState(uint32 _slot_index, uint64 _begin_byte_offset, uint64 _end_byte_offset) :
        slot_index(_slot_index),
        begin_byte_offset(_begin_byte_offset),
        end_byte_offset(_end_byte_offset),
        next_byte_offset(_begin_byte_offset) {}

    uint32              slot_index{0};
    uint64              begin_byte_offset{0};
    uint64              end_byte_offset{0};
    std::atomic<uint64> next_byte_offset{0};
};

struct VulkanDescriptorBufferLease {
    std::shared_ptr<VulkanDescriptorBufferLeaseState> state;

    bool IsValid() const {
        return state != nullptr;
    }
};
struct VulkanDescriptorHeap {
public:
    explicit VulkanDescriptorHeap(VulkanDevice& _device);
    ~VulkanDescriptorHeap();

    VulkanDescriptorHeap(const VulkanDescriptorHeap&)            = delete;
    VulkanDescriptorHeap& operator=(const VulkanDescriptorHeap&) = delete;
    VulkanDescriptorHeap(VulkanDescriptorHeap&&)                 = delete;
    VulkanDescriptorHeap& operator=(VulkanDescriptorHeap&&)      = delete;

    Array<byte> buffer_descriptor_cache;
    Array<byte> sampler_and_image_descriptor_cache;
    Array<byte> acceleration_structure_descriptor_cache;

    Array<VkDescriptorType> buffer_descriptor_types;
    Array<VkDescriptorType> image_descriptor_types;

    Array<uint> free_buffer_slots;
    Array<uint> free_image_slots;
    Array<uint> free_acceleration_structure_slots;

    uint64 next_buffer_cache_byte_offset;
    uint64 next_image_cache_byte_offset;
    uint64 next_acceleration_structure_cache_byte_offset;

    uint GetOrCreateBufferDescriptorOffset(
        const BufferView& _in_buffer,
        VkDescriptorType  _type,
        VkFormat          _format = VK_FORMAT_UNDEFINED
    );
    void FreeBufferDescriptorSlot(uint _slot_index);
    uint GetOrCreateImageDescriptorOffset(
        const TextureView* _in_image,
        VkImageLayout      _layout,
        VkDescriptorType   _descriptor_type = VK_DESCRIPTOR_TYPE_MAX_ENUM
    );
    void FreeImageDescriptorSlot(uint _slot_index);
    uint GetSamplerDescriptorOffset(Sampler _sampler);
    uint GetOrCreateAccelerationStructureDescriptorOffset(VulkanAccelerationStructure* _as);
    void FreeAccelerationStructureDescriptorSlot(uint _slot_index);

public:
    VulkanDescriptorBufferLease AcquireDescriptorBufferLease(EQueueType _queue_type);
    void                        FlushDescriptorWrites(const VulkanDescriptorBufferLease& _lease);
    void                        RecycleDescriptorBufferLease(VulkanDescriptorBufferLease _lease);

    // Reserve one immutable descriptor range for a single pipeline bind. The
    // returned absolute offset stays valid until the owning queue timeline is
    // retired; concurrent recorders never share a range.
    std::optional<uint64>
    ReserveDescriptorRange(const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease, uint64 _size);
    uint64 GetDescriptorWriteOffset(const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease) const;
    // All recorders using this lease must have joined and discarded their work
    // before rolling the cursor back for serial fallback.
    void
    RewindDescriptorWrites(const std::shared_ptr<VulkanDescriptorBufferLeaseState>& _lease, uint64 _offset);

    uint GetDescriptorSize(VkDescriptorType _type) const;

    void WriteUniformDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset);
    void WriteStorageDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset);
    void WriteImageDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset);
    void WriteSamplerDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset);
    void WriteAccelDesc(uint64 _source_byte_offset, uint64 _destination_byte_offset);

public:
    VulkanDevice* m_device;

    VulkanBuffer* descriptor_buffer;

    uint storage_buffer_descriptor_size;
    uint uniform_buffer_descriptor_size;
    uint storage_texel_buffer_descriptor_size;
    uint uniform_texel_buffer_descriptor_size;
    uint buffer_cache_slot_stride;

    uint sampled_image_descriptor_size;
    uint storage_image_descriptor_size;
    uint image_cache_slot_stride;
    uint sampler_descriptor_size;
    uint acceleration_structure_descriptor_size;

    std::mutex                                               cache_mutex;
    mutable std::mutex                                       lease_mutex;
    std::condition_variable                                  lease_available;
    Array<std::shared_ptr<VulkanDescriptorBufferLeaseState>> active_leases;
    StaticArray<uint32, 2>                                   next_queue_slots{};
    uint64                                                   image_cache_byte_offset;

    Array<uint64> lease_slot_byte_offsets;
    uint8*        mapped_descriptor_data;
};

} // namespace Moer::Render
