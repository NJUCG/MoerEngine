#pragma once

#include "VulkanDescriptorBufferAllocator.h"
#include "VulkanDeviceObject.h"
#include "VulkanRHIResource.h"
#include "misc/LockFree.h"
#include "misc/Types.h"
#include "rhi/RHICommand.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace Moer::Render {

class VulkanBindlessArray final : public BindlessArray, public VulkanDeviceObject {
public:
    enum EType : uint32 {
        Texture,
        Buffer
    };
    struct Handle {
        uint32 ptr_1;
        uint32 ptr_2;
        uint32 slot : 22;
        uint32 attrib : 8;
        uint32 type : 2;

        Handle(uint64 _ptr, uint32 _slot, uint32 _attrib, uint32 _type) :
            ptr_1(_ptr >> 32),
            ptr_2(_ptr & 0xffffffff),
            slot(_slot),
            attrib(_attrib),
            type(_type) {}
        Handle() : ptr_1(0), ptr_2(0), slot(0), attrib(0), type(0) {}
        bool IsTexture() const {
            return type == Texture;
        }
        bool IsBuffer() const {
            return type == Buffer;
        }
        uint64 Ptr() const {
            return uint64(ptr_1) << 32 | uint64(ptr_2);
        }
    };

    VulkanBindlessArray(VulkanDevice* _device, uint32 _max_size);
    ~VulkanBindlessArray() override;

    uint32 AllocateTexture(const TextureView& _texture, Sampler _sampler) override;
    uint32 AllocateBuffer(BufferView _buffer) override;
    void   UnbindTexture(uint32 _slot) override;
    void   UnbindBuffer(uint32 _slot) override;
    bool   IsResourceAllocated(uint64 _handle) const;
    bool   IsTextureViewAllocated(
        uint64 _texture,
        uint8  _mip_level,
        uint8  _mip_count,
        uint8  _array_layer,
        uint8  _array_count
    ) const;
    struct PresentationSourceMembershipRecord {
        TextureRef texture{};
        uint32     count{0};
    };
    struct PresentationSourceMembership {
        UnorderedMap<uint32, TextureRef>                                           slots{};
        UnorderedMap<::Moer::Render::Texture*, PresentationSourceMembershipRecord> textures{};
    };
    using PresentationSourceMembershipSnapshot = std::shared_ptr<const PresentationSourceMembership>;

    // Descriptor membership becomes visible only after the packet which
    // updates it reaches vkQueueSubmit. CPU allocation state belongs to a
    // different timeline and cannot classify an older immutable submit.
    [[nodiscard]] PresentationSourceMembershipSnapshot
    SnapshotAcceptedPresentationSourceMembership() const noexcept;
    void
         PublishAcceptedPresentationSourceMembership(PresentationSourceMembershipSnapshot _membership) noexcept;
    void FinalizeUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates, bool _gpu_update_succeeded);
    bool BeginUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates);

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

    void Lock() {
        mtx.lock();
    }
    void Unlock() {
        mtx.unlock();
    }
    std::unique_lock<std::mutex> AcquireLock() {
        return std::unique_lock<std::mutex>(mtx);
    }

public:
    uint64        texture_offset_in_buffer;
    uint64        buffer_descriptors_byte_offset = 0;
    Array<Handle> handles;

private:
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

    std::mutex mtx;
    void       TrackResource(RHIResource* _resource);
    void       UntrackResource(uint64 _handle);
    void       TrackTextureView(uint32 _array_idx, const TextureView& _view);
    void       UntrackTextureView(uint32 _array_idx);

protected:
    UniquePtr<Command>          CreateUpdateCommand() override;
    void                        DiscardUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates) override;
    class VulkanDescriptorHeap& g_heap;

    LockFreeQueueBase<uint32, true> free_texture_slots;
    LockFreeQueueBase<uint32, true> free_buffer_slots;
    LockFreeQueueBase<uint32, true> free_slots;
    std::atomic<uint32>             texture_slot_offset;
    std::atomic<uint32>             buffer_slot_offset;
    std::atomic<uint32>             slot_offset;
    Array<uint64>                   array_slot_generations;
    Array<uint64>                   texture_slot_generations;
    Array<uint64>                   buffer_slot_generations;
    Array<uint64>                   array_slot_claim_tokens;
    Array<uint64>                   texture_slot_claim_tokens;
    Array<uint64>                   buffer_slot_claim_tokens;
    uint64                          next_update_command_token{0};
    Array<UpdateCmd>                update_cmds;
    UnorderedMap<uint32, uint32>    temp_slot_to_cmd;

    struct ResourceAllocationRecord {
        CountableRef<RHIResource> resource;
        uint32                    count{0};
    };
    UnorderedMap<uint64, ResourceAllocationRecord> resource_allocation_counts;
    PresentationSourceMembershipSnapshot           accepted_presentation_source_membership;
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
