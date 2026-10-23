#ifndef MOER_VK_RESOURCE_TRACKER_H
#define MOER_VK_RESOURCE_TRACKER_H

#include "VulkanCommon.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {
class VulkanBuffer;
class VulkanTexture;
class VulkanCommandList;
class VulkanSerialGoldenTrace;
struct SerialQueueFamilyMap;

struct BarrierSemanticDiagnostics {
    uint64 digest{0};
    uint32 buffer_count{0};
    uint32 texture_count{0};
    uint32 memory_count{0};
};

// A self-contained, ordered barrier prefix produced by the coordinator. It
// can outlive the tracker's pending arrays and be emitted later while a single
// primary command buffer assembles pass secondaries in graph order.
struct VulkanResolvedBarrierBatch {
    Array<VkBufferMemoryBarrier2> buffer_barriers;
    Array<VkImageMemoryBarrier2>  texture_barriers;
    Array<VkMemoryBarrier2>       memory_barriers;

    [[nodiscard]] bool Empty() const noexcept {
        return buffer_barriers.empty() && texture_barriers.empty() &&
               memory_barriers.empty();
    }

    void Record(VulkanCommandList& _cmd_list) const;
};

template<typename TTexture>
[[nodiscard]] constexpr bool TextureSubresourceRangesOverlap(
    const TextureSubresourceKeyT<TTexture>& lhs,
    const TextureSubresourceKeyT<TTexture>& rhs
) {
    const auto overlaps = [](uint8 first_a,
                             uint8 count_a,
                             uint8 first_b,
                             uint8 count_b) {
        const uint16 end_a =
            static_cast<uint16>(first_a) + static_cast<uint16>(count_a);
        const uint16 end_b =
            static_cast<uint16>(first_b) + static_cast<uint16>(count_b);
        return static_cast<uint16>(first_a) < end_b &&
               static_cast<uint16>(first_b) < end_a;
    };
    return lhs.texture == rhs.texture &&
           overlaps(
               lhs.mip_level,
               lhs.mip_count,
               rhs.mip_level,
               rhs.mip_count
           ) &&
           overlaps(
               lhs.array_layer,
               lhs.array_count,
               rhs.array_layer,
               rhs.array_count
           );
}

template<typename TBuffer>
struct BufferByteRangeT {
    TBuffer* buffer{nullptr};
    uint64   offset{0};
    uint64   byte_size{0};
};

template<typename TBuffer>
[[nodiscard]] constexpr bool BufferByteRangesOverlap(
    const BufferByteRangeT<TBuffer>& lhs,
    const BufferByteRangeT<TBuffer>& rhs
) {
    if (lhs.buffer != rhs.buffer || lhs.byte_size == 0 || rhs.byte_size == 0) {
        return false;
    }
    return lhs.offset <= rhs.offset ?
               lhs.byte_size > rhs.offset - lhs.offset :
               rhs.byte_size > lhs.offset - rhs.offset;
}

template<typename TTexture>
struct TextureAspectSubresourceRangeT {
    TextureSubresourceKeyT<TTexture> subresource{};
    VkImageAspectFlags               aspects{0};
};

template<typename TTexture>
[[nodiscard]] constexpr bool TextureAspectSubresourceRangesOverlap(
    const TextureAspectSubresourceRangeT<TTexture>& lhs,
    const TextureAspectSubresourceRangeT<TTexture>& rhs
) {
    return (lhs.aspects & rhs.aspects) != 0 &&
           TextureSubresourceRangesOverlap(lhs.subresource, rhs.subresource);
}

// Coordinator-owned state for one submission. ResolveBarriers advances tracked
// accesses; Reset discards that submission's state when its context is reused.
class VulkanResourceTracker {
public:
    explicit VulkanResourceTracker(EQueueType _queue = EQueueType::Graphics);

    // Queue-family ownership handoff for legacy resource declarations.
    void QueueTransferReleaseResource(VulkanBuffer* _buffer, uint _src_queue, uint _dst_queue);
    void QueueTransferReleaseResource(
        VulkanTexture* _texture,
        uint           _src_queue,
        uint           _dst_queue,
        VkImageLayout  _src_layout,
        VkImageLayout  _dst_layout
    );

    void QueueTransferAcquireResource(
        VulkanBuffer*         _buffer,
        uint                  _src_queue,
        uint                  _dst_queue,
        VkAccessFlags2        _dst_access,
        VkPipelineStageFlags2 _dst_stage
    );
    void QueueTransferAcquireResource(
        VulkanTexture*        _texture,
        uint                  _src_queue,
        uint                  _dst_queue,
        VkImageLayout         _src_layout,
        VkImageLayout         _dst_layout,
        VkAccessFlags2        _dst_access,
        VkPipelineStageFlags2 _dst_stage
    );

    // Accumulate requested accesses until the next ResolveBarriers call.
    void RecordState(
        VulkanBuffer*         _buffer,
        VkAccessFlags2        _access,
        VkPipelineStageFlags2 _stage,
        uint32_t              _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t              _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    // An external command has already established this state. Update existing
    // records only; this does not generate a barrier or introduce new records.
    void AdoptExternalState(VulkanBuffer* _buffer, VkAccessFlags2 _access, VkPipelineStageFlags2 _stage);
    void AdoptExternalState(
        VulkanTexture*        _texture,
        VkAccessFlags2        _access,
        VkImageLayout         _layout,
        VkPipelineStageFlags2 _stage
    );

    void RecordState(
        VulkanBuffer* _buffer,
        std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&&,
        uint32_t _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    void RecordState(
        VulkanTexture*        _texture,
        VkAccessFlags2        _access,
        VkImageLayout         _layout,
        VkPipelineStageFlags2 _stage,
        uint8_t               _mip_level        = 0,
        uint8_t               _mip_count        = 1,
        uint8_t               _array_layer      = 0,
        uint8_t               _array_count      = 1,
        uint32_t              _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t              _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    void RegisterFlushBufferRange(
        const BufferView&     _view,
        VkAccessFlags2        _access,
        VkPipelineStageFlags2 _stage,
        VkAccessFlags2        _src_access = VK_ACCESS_2_NONE,
        VkPipelineStageFlags2 _src_stage  = VK_PIPELINE_STAGE_2_NONE
    );
    void RecordState(
        VulkanTexture* _texture,
        std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&&,
        uint32_t _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    // Graph-owned barriers bypass state inference. Local and acquire barriers
    // adopt dst so a following inferred access observes the materialized
    // state. A release only emits its native half: destination state belongs
    // exclusively to the acquire-side tracker.
    void EmitExplicitBarrier(
        VulkanBuffer*         _buffer,
        uint64                _offset,
        uint64                _byte_size,
        EBarrierQueueTransferPhase _phase,
        uint32_t              _src_queue_family,
        uint32_t              _dst_queue_family,
        VkPipelineStageFlags2 _src_stage,
        VkAccessFlags2        _src_access,
        VkPipelineStageFlags2 _dst_stage,
        VkAccessFlags2        _dst_access
    );
    void EmitExplicitBarrier(
        VulkanTexture*        _texture,
        VkImageAspectFlags    _aspects,
        uint8                 _mip_level,
        uint8                 _mip_count,
        uint8                 _array_layer,
        uint8                 _array_count,
        EBarrierQueueTransferPhase _phase,
        uint32_t              _src_queue_family,
        uint32_t              _dst_queue_family,
        VkPipelineStageFlags2 _src_stage,
        VkAccessFlags2        _src_access,
        VkImageLayout         _src_layout,
        VkPipelineStageFlags2 _dst_stage,
        VkAccessFlags2        _dst_access,
        VkImageLayout         _dst_layout
    );

    // Current write/layout-transition candidates used by bindless preprocessing.
    const UnorderedSet<TextureSubresourceKeyT<VulkanTexture>, TextureSubresourceKeyHashT<VulkanTexture>>&
    GetWriteStateTextures() const {
        return write_state_textures;
    }

    const Set<VulkanBuffer*>& GetWriteStateBuffers() const {
        return write_state_buffers;
    }

    void ResolveBarriers();

    VulkanResolvedBarrierBatch TakeResolvedBarriers();

    BarrierSemanticDiagnostics GetPendingBarrierDiagnostics(
        VulkanSerialGoldenTrace*   _serial_golden,
        uint64_t                   _group_ordinal,
        const SerialQueueFamilyMap& _queue_family_map
    ) const;

    void DispatchBarriers(VulkanCommandList& _cmd_list);
    void RestoreState();
    void Reset();

    // Convert legacy RHI access declarations without recording a transition.
    auto ReadBuffer(VulkanBuffer*, EBufferState, EPassType _type = EPassType::Graphics)
        -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2>;
    auto WriteBuffer(VulkanBuffer*, EBufferState, EPassType _type = EPassType::Graphics)
        -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2>;
    auto ReadTexture(VulkanTexture*, ETextureState, EPassType _type = EPassType::Graphics)
        -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>;
    auto WriteTexture(VulkanTexture*, ETextureState, EPassType _type = EPassType::Graphics)
        -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>;

    // Pure legacy-state resolvers keep backend validation testable without a
    // Vulkan device. Copy is valid only for TRANSFER/UNDEFINED; shader and
    // attachment state/pass mismatches are rejected instead of indexing past
    // a lookup table.
    RENDER_API static auto ResolveBufferState(
        EBufferState _state,
        EPassType    _type,
        bool         _is_write
    )
        -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2>;
    RENDER_API static auto ResolveTextureState(
        ETextureState _state,
        EPassType     _type,
        bool          _is_write,
        bool          _is_depth
    ) -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>;

    void MarkBlasWritten(uint64 _geometry_handle);

    const Set<uint64>& GetWrittenBlasHandles() const {
        return written_blas_handles;
    }

private:
    struct BufferRange {
        uint64 begin;
        uint64 end;
    };
    // src is the resolved state; dst accumulates the next unresolved access.
    struct BufferState {
        VkAccessFlags2        src_access;
        VkPipelineStageFlags2 src_stage;
        VkAccessFlags2        dst_access;
        VkPipelineStageFlags2 dst_stage;
        uint32_t              src_queue_family;
        uint32_t              dst_queue_family;
    };
    struct TextureState {
        VkAccessFlags2        src_access;
        VkImageLayout         src_layout;
        VkPipelineStageFlags2 src_stage;
        VkAccessFlags2        dst_access;
        VkImageLayout         dst_layout;
        VkPipelineStageFlags2 dst_stage;
        uint32_t              src_queue_family;
        uint32_t              dst_queue_family;
    };

    void SetWriteState(const TextureSubresourceKeyT<VulkanTexture>& _key, bool _is_write);
    void SetWriteState(VulkanBuffer* _buffer, bool _is_write);

    TextureSubresourceKeyT<VulkanTexture> MakeTextureStateKey(
        VulkanTexture* _texture,
        uint8          _mip_level,
        uint8          _mip_count,
        uint8          _array_layer,
        uint8          _array_count
    ) const;
    EQueueType queue_type;

    // Materialized barriers awaiting recording or transfer to a barrier batch.
    Array<VkBufferMemoryBarrier2> buffer_barriers;
    Array<VkImageMemoryBarrier2>  texture_barriers;
    Array<VkMemoryBarrier2>       memory_barriers;

    // Resolved/pending resource states and resources leaving this queue.
    UnorderedMap<VulkanBuffer*, BufferState> buffer_states;
    UnorderedMap<
        TextureSubresourceKeyT<VulkanTexture>,
        TextureState,
        TextureSubresourceKeyHashT<VulkanTexture>>
                        texture_states;
    Set<VulkanTexture*> exported_textures;
    Set<VulkanBuffer*>  exported_buffers;

    // Entries requiring resolution before the next command group.
    Set<VulkanBuffer*> pending_buffers;
    UnorderedSet<TextureSubresourceKeyT<VulkanTexture>, TextureSubresourceKeyHashT<VulkanTexture>>
        pending_textures;

    // Bindless and acceleration-structure dependency candidates.
    Set<uint64> written_blas_handles;
    UnorderedSet<TextureSubresourceKeyT<VulkanTexture>, TextureSubresourceKeyHashT<VulkanTexture>>
                       write_state_textures;
    Set<VulkanBuffer*> write_state_buffers;

    // Internal buffer ranges; state survives range flushing until Reset.
    UnorderedMap<VulkanBuffer*, BufferState> flush_buffer_states;
    UnorderedMap<VulkanBuffer*, BufferRange> flush_buffer_ranges;
    // Guards against incompatible inferred accesses after explicit barriers.
    Set<VulkanBuffer*>                       explicit_partial_buffers;
    Set<VulkanTexture*>                      explicit_partial_aspect_textures;
    Array<BufferByteRangeT<VulkanBuffer>> explicit_released_buffer_ranges;
    Array<TextureAspectSubresourceRangeT<VulkanTexture>>
        explicit_released_texture_ranges;
    // Explicit texture state is tracked at atomic mip/layer granularity.
    // Inferred accesses to a texture in this set are decomposed the same way,
    // so legal RDG range-shape changes cannot restart from preferred state.
    UnorderedSet<
        TextureSubresourceKeyT<VulkanTexture>,
        TextureSubresourceKeyHashT<VulkanTexture>>
        explicit_texture_ranges;
};
} // namespace Moer::Render
#endif
