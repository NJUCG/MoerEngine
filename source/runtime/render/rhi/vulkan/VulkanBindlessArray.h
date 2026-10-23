#pragma once

#include "VulkanDeviceObject.h"
#include "VulkanRHIResource.h"
#include "misc/LockFree.h"
#include "rhi/RHICommand.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace Moer::Render {

class VulkanBindlessArray final : public BindlessArray, public VulkanDeviceObject {
public:
    enum EType : uint {
        Texture,
        Buffer
    };
    struct Handle {
        uint ptr_1;
        uint ptr_2;
        uint slot : 22;
        uint attrib : 8;
        uint type : 2;

        Handle(uint64 _ptr, uint _slot, uint _attrib, uint _type) :
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

    uint AllocateTexture(const TextureView& _texture, Sampler _sampler) override;
    uint AllocateBuffer(BufferView _buffer) override;
    void UnbindTexture(uint _slot) override;
    void UnbindBuffer(uint _slot) override;
    bool IsResourceAllocated(uint64 _handle) const;
    bool IsTextureViewAllocated(
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
        UnorderedMap<uint, TextureRef>                                             slots{};
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
        return (uint64)bindless_index_buffer;
    }

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
    // Plain uint index table, read by device address from push constants.
    VulkanBuffer* bindless_index_buffer;
    // Descriptor storage bound through vkCmdBindDescriptorBuffersEXT.
    VulkanBuffer* bindless_buffer_descs;
    VulkanBuffer* bindless_texture_descs;
    uint64        texture_offset_in_buffer;
    uint64        buffer_descriptors_byte_offset;
    Array<Handle> handles;

private:
    std::mutex mtx;
    void       TrackResource(RHIResource* _resource);
    void       UntrackResource(uint64 _handle);
    void       TrackTextureView(uint _array_idx, const TextureView& _view);
    void       UntrackTextureView(uint _array_idx);

protected:
    UniquePtr<Command> CreateUpdateCommand() override;
    void               DiscardUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates) override;
    class VulkanDescriptorHeap& g_heap;

    LockFreeQueueBase<uint, true> free_texture_slots;
    LockFreeQueueBase<uint, true> free_buffer_slots;
    LockFreeQueueBase<uint, true> free_slots;
    std::atomic_uint              texture_slot_offset;
    std::atomic_uint              buffer_slot_offset;
    std::atomic_uint              slot_offset;
    Array<uint64>                 array_slot_generations;
    Array<uint64>                 texture_slot_generations;
    Array<uint64>                 buffer_slot_generations;
    Array<uint64>                 array_slot_claim_tokens;
    Array<uint64>                 texture_slot_claim_tokens;
    Array<uint64>                 buffer_slot_claim_tokens;
    uint64                        next_update_command_token{0};
    Array<UpdateCmd>              update_cmds;
    Array<std::pair<uint, uint>>  array_indices_dat;
    Array<byte>                   array_dat;
    Array<std::pair<uint, uint>>  texture_indices_dat;
    Array<byte>                   texture_dat;
    Array<std::pair<uint, uint>>  buffer_indices_dat;
    Array<byte>                   buffer_dat;
    UnorderedMap<uint, uint>      temp_slot_to_cmd;

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
