#pragma once

#include "VulkanDescriptorBufferAllocator.h"
#include "VulkanRHIResource.h"
#include "misc/Types.h"
#include "rhi/RHICommand.h"

#include <deque>
#include <memory>
#include <mutex>

namespace Moer::Render {

class VulkanBindlessArray final : public BindlessArray {
public:
    enum class EResourceType : uint8 {
        Texture,
        Buffer
    };
    struct BindingRecord {
        // Borrowed from resource_allocation_counts. A null resource marks an
        // empty record; descriptor_slot and type are meaningful only when bound.
        RHIResource*  resource{nullptr};
        uint32        descriptor_slot{0};
        EResourceType type{EResourceType::Texture};

        bool IsTexture() const {
            return resource != nullptr && type == EResourceType::Texture;
        }
        bool IsBuffer() const {
            return resource != nullptr && type == EResourceType::Buffer;
        }
    };

    // Array lifetime and frontend slot allocation.
    VulkanBindlessArray(VulkanDevice* _device, uint32 _max_size);
    ~VulkanBindlessArray() override;

    uint32 AllocateTexture(const TextureView& _texture, Sampler _sampler) override;
    uint32 AllocateBuffer(BufferView _buffer) override;
    void   UnbindTexture(uint32 _slot) override;
    void   UnbindBuffer(uint32 _slot) override;

    // Ordered update admission and completion.
    void FinalizeUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates, bool _gpu_update_succeeded);
    bool BeginUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates);


    // GPU index table and descriptor storage borrowed by command recording.
    uint64 ArrayHandle() const override {
        return reinterpret_cast<uint64>(m_index_buffer.Get());
    }

    // Borrowed storage; the array owns it through completion of its updates.
    VulkanBuffer* GetIndexBuffer() const {
        return m_index_buffer.Get();
    }
    VulkanBuffer* GetBufferDescriptorBuffer() const {
        return heap_buffer ? heap_buffer : m_buffer_descriptors.Get();
    }
    VulkanBuffer* GetTextureSamplerDescriptorBuffer() const {
        return heap_buffer ? heap_buffer : m_texture_sampler_descriptors.Get();
    }

    VkDescriptorSet GetLegacySet(VkDescriptorType type) const;
    VkAccessFlags2  GetDescriptorReadAccess() const;

public:
    // Recording-visible table locations and current CPU bindings.
    uint64               texture_offset_in_buffer;
    uint64               buffer_descriptors_byte_offset = 0;
    Array<BindingRecord> bindings;

private:
    VulkanDevice* m_device;

    // Owned descriptor sets and persistent GPU storage.
    struct LegacyDescriptorSets;
    std::unique_ptr<LegacyDescriptorSets> legacy_sets;
    void                                  CreateLegacySets(uint32 max_size);
    // Persistent ranges share the existing lease allocator; the array owns them.
    VulkanDescriptorBufferLease heap_lease;
    VulkanBuffer*               heap_buffer = nullptr;
    // Initialization uploads cross the frontend CommandList boundary, so the
    // table and its submissions must share ownership of this storage.
    // Plain uint index table, read by device address from push constants.
    VulkanBufferRef m_index_buffer;
    // Descriptor storage bound through vkCmdBindDescriptorBuffersEXT.
    VulkanBufferRef m_buffer_descriptors;
    VulkanBufferRef m_texture_sampler_descriptors;

    // Completion validates a plan before applying slot/reference retirement.
    struct UpdateCompletionPlan;
    // Caller holds mtx. Plans borrow command data and never outlive that call.
    UpdateCompletionPlan
                         BuildFinalizationPlan(const Array<UpdateCmd>& updates, bool gpu_update_succeeded) const;
    UpdateCompletionPlan BuildDiscardPlan(const Array<UpdateCmd>& updates) const;
    void                 ValidateReferenceDecrements(const UpdateCompletionPlan& plan) const;
    void                 ApplyCompletionPlan(const UpdateCompletionPlan& plan, uint64 command_token);
    void                 ApplySlotClaims(
        const UnorderedSet<uint32>& array_slots,
        const UnorderedSet<uint32>& texture_slots,
        const UnorderedSet<uint32>& buffer_slots,
        uint64                      command_token
    );

    void UnbindSlot(uint32 _array_idx);

    // Serializes slot mutation, pending updates and resource reference accounting.
    std::mutex mtx;
    void       TrackResource(RHIResource* _resource);
    void       UntrackResource(uint64 _handle);
    void       TrackTextureView(uint32 _array_idx, const TextureView& _view);
    void       UntrackTextureView(uint32 _array_idx);

private:
    // Frontend update-command publication and cancellation hooks.
    UniquePtr<Command>          CreateUpdateCommand() override;
    void                        DiscardUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates) override;
    class VulkanDescriptorHeap& g_heap;

    // All slot allocation and retirement is serialized by mtx; retain FIFO reuse.
    std::deque<uint32> free_texture_slots;
    std::deque<uint32> free_buffer_slots;
    std::deque<uint32> free_slots;
    uint32             texture_slot_offset;
    uint32             buffer_slot_offset;
    uint32             slot_offset;

    // Generations prevent stale updates from retiring reused slots.
    Array<uint64>                   array_slot_generations;
    Array<uint64>                   texture_slot_generations;
    Array<uint64>                   buffer_slot_generations;

    // One command may claim each slot lifecycle transition.
    Array<uint64>                   array_slot_claim_tokens;
    Array<uint64>                   texture_slot_claim_tokens;
    Array<uint64>                   buffer_slot_claim_tokens;

    // Pending frontend mutations, coalesced by array slot.
    uint64                          next_update_command_token{0};
    Array<UpdateCmd>                update_cmds;
    UnorderedMap<uint32, uint32>    temp_slot_to_cmd;

    // Keep resources and distinct texture views alive through update retirement.
    struct ResourceAllocationRecord {
        CountableRef<RHIResource> resource;
        uint32                    count{0};
    };
    UnorderedMap<uint64, ResourceAllocationRecord> resource_allocation_counts;
    UnorderedMap<
        uint64,
        UnorderedMap<
            TextureSubresourceKeyT<VulkanTexture>,
            uint32,
            TextureSubresourceKeyHashT<VulkanTexture>>>
                                                 texture_view_ref_counts;
    Array<TextureSubresourceKeyT<VulkanTexture>> texture_view_infos;
};

} // namespace Moer::Render
