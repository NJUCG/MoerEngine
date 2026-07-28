#ifndef MOER_VK_RESOURCE_TRACKER_H
#define MOER_VK_RESOURCE_TRACKER_H
#include "VulkanPlatform.h"
#include "VulkanRHIResource.h"
#include "misc/Crc32.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"
namespace Moer::Render {

class VkTracker {
private:
    struct BufferRange {
        uint64 min;
        uint64 max;
    };
    struct BufferState {
        VkAccessFlagBits2        src_access;
        VkPipelineStageFlagBits2 src_stage;
        VkAccessFlagBits2        dst_access;
        VkPipelineStageFlagBits2 dst_stage;
        uint32_t                 src_queue_family;
        uint32_t                 dst_queue_family;
    };
    struct TextureState {
        VkAccessFlagBits2        src_access;
        VkImageLayout            src_layout;
        VkPipelineStageFlagBits2 src_stage;
        VkAccessFlagBits2        dst_access;
        VkImageLayout            dst_layout;
        VkPipelineStageFlagBits2 dst_stage;
        uint32_t                 src_queue_family;
        uint32_t                 dst_queue_family;
    };

public:
    VkTracker(EQueueType _queue = EQueueType::Graphics) : queue_type(_queue) {
        switch (_queue) {

            case EQueueType::Graphics: {
                first_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                last_stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                break;
            }
            case EQueueType::Compute: {
                first_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                last_stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                break;
            }
            case EQueueType::Copy: {
                first_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                last_stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                break;
            }
            case EQueueType::Ignore:
            case EQueueType::Num: {
                assert(false && "Invalid queue type for vk tracker");
            }
        }
    };
    ~VkTracker() = default;

    void EmitReleasePlan(VulkanBuffer* _buffer, uint _src_queue, uint _dst_queue);
    void EmitReleasePlan(
        VulkanTexture* _texture,
        uint           _src_queue,
        uint           _dst_queue,
        VkImageLayout  _src_layout,
        VkImageLayout  _dst_layout
    );

    void EmitAcquirePlan(
        VulkanBuffer*            _buffer,
        uint                     _src_queue,
        uint                     _dst_queue,
        VkAccessFlagBits2        _dst_access,
        VkPipelineStageFlagBits2 _dst_stage
    );
    void EmitAcquirePlan(
        VulkanTexture*           _texture,
        uint                     _src_queue,
        uint                     _dst_queue,
        VkImageLayout            _src_layout,
        VkImageLayout            _dst_layout,
        VkAccessFlagBits2        _dst_access,
        VkPipelineStageFlagBits2 _dst_stage
    );

    void EmitLocalTransition(
        VulkanBuffer*            _buffer,
        VkAccessFlagBits2        _access,
        VkPipelineStageFlagBits2 _stage,
        uint32_t                 _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t                 _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );
    void EmitLocalTransition(
        VulkanBuffer* _buffer,
        std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&& _state,
        uint32_t _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );
    void EmitLocalTransition(
        VulkanTexture*           _texture,
        VkAccessFlagBits2        _access,
        VkImageLayout            _layout,
        VkPipelineStageFlagBits2 _stage,
        uint8_t                  _mip_level        = 0,
        uint8_t                  _mip_count        = 1,
        uint8_t                  _array_layer      = 0,
        uint8_t                  _array_count      = 1,
        uint32_t                 _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t                 _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );
    void EmitLocalTransition(
        VulkanTexture* _texture,
        std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&& _state,
        uint32_t _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    void RecordState(
        VulkanBuffer*            _buffer,
        VkAccessFlagBits2        _access,
        VkPipelineStageFlagBits2 _stage,
        uint32_t                 _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t                 _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    void FlushSrcState(VulkanBuffer* _buffer, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage);
    void FlushSrcState(
        VulkanTexture*           _texture,
        VkAccessFlagBits2        _access,
        VkImageLayout            _layout,
        VkPipelineStageFlagBits2 _stage
    );

    void RecordState(
        VulkanBuffer* _texture,
        std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&&,
        uint32_t _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    void RecordState(
        VulkanTexture*           _texture,
        VkAccessFlagBits2        _access,
        VkImageLayout            _layout,
        VkPipelineStageFlagBits2 _stage,
        uint8_t                  _mip_level        = 0,
        uint8_t                  _mip_count        = 1,
        uint8_t                  _array_layer      = 0,
        uint8_t                  _array_count      = 1,
        uint32_t                 _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t                 _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    void
    RegisterFlushBuffer(const BufferView& _view, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage);
    void RegisterFlushBufferRange(
        const BufferView&        _view,
        VkAccessFlagBits2        _access,
        VkPipelineStageFlagBits2 _stage,
        VkAccessFlagBits2        _src_access = VK_ACCESS_2_NONE,
        VkPipelineStageFlagBits2 _src_stage  = VK_PIPELINE_STAGE_2_NONE
    );
    void RecordState(
        VulkanTexture* _texture,
        std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&&,
        uint32_t _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t _dst_queue_family = VK_QUEUE_FAMILY_IGNORED
    );

    void SetPassType(EPassType _type) {
        pass_type = _type;
    }

    const UnorderedSet<TextureSubresourceKeyT<VulkanTexture>, TextureSubresourceKeyHashT<VulkanTexture>>&
    GetWritedStateTextures() const {
        return writed_state_textures;
    }

    const Set<VulkanBuffer*>& GetWritedStateBuffers() const {
        return writed_state_buffers;
    }

    // Load persistent state from resource into tracker when src is unknown at translate time.
    void LoadPersistentState(VulkanBuffer* buffer);
    void LoadPersistentState(
        VulkanTexture* texture,
        uint8 mip_level, uint8 mip_count,
        uint8 array_layer, uint8 array_count
    );
    void SetTrackedState(VulkanBuffer* buffer, EBufferState state, EQueueType owner_queue, bool access_write);
    void SetTrackedState(
        VulkanTexture* texture,
        ETextureState  state,
        EQueueType     owner_queue,
        bool           access_write,
        uint8          mip_level,
        uint8          mip_count,
        uint8          array_layer,
        uint8          array_count
    );

    void ResolveBarriers();

    void DispatchBarriers(class VulkanCmdList& _cmd_list);
    void RestoreState();
    void Reset();

    //automic state transition
    auto ReadBuffer(VulkanBuffer*, EBufferState, EPassType _type = EPassType::Graphics)
        -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2>;
    auto WriteBuffer(VulkanBuffer*, EBufferState, EPassType _type = EPassType::Graphics)
        -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2>;
    auto ReadTexture(VulkanTexture*, ETextureState, EPassType _type = EPassType::Graphics)
        -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>;
    auto WriteTexture(VulkanTexture*, ETextureState, EPassType _type = EPassType::Graphics)
        -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>;

    void MarkWriteable(const TextureSubresourceKeyT<VulkanTexture>& _key, bool _writeable = true);
    void MarkWriteable(VulkanBuffer* _buffer, bool _writeable = true);
    void SetBufferOverlap(VulkanBuffer* _buffer, bool _enabled);

private:
    TextureSubresourceKeyT<VulkanTexture> MakeTextureStateKey(
        VulkanTexture* _texture,
        uint8          _mip_level,
        uint8          _mip_count,
        uint8          _array_layer,
        uint8          _array_count
    ) const;
    EPassType             pass_type;
    EQueueType            queue_type  = EQueueType::Graphics;
    VkPipelineStageFlags2 last_stage  = 0;
    VkPipelineStageFlags2 first_stage = 0;

    Array<VkBufferMemoryBarrier2> buffer_barriers;
    Array<VkImageMemoryBarrier2>  texture_barriers;
    Array<VkMemoryBarrier2>       memory_barriers;

    UnorderedMap<VulkanBuffer*, BufferState> buffer_states;
    UnorderedMap<
        TextureSubresourceKeyT<VulkanTexture>,
        TextureState,
        TextureSubresourceKeyHashT<VulkanTexture>>
                        texture_states;
    Set<VulkanTexture*> exported_textures;
    Set<VulkanBuffer*>  exported_buffers;

    Set<VulkanBuffer*> pending_buffers;
    UnorderedSet<TextureSubresourceKeyT<VulkanTexture>, TextureSubresourceKeyHashT<VulkanTexture>>
        pending_textures;

    UnorderedSet<TextureSubresourceKeyT<VulkanTexture>, TextureSubresourceKeyHashT<VulkanTexture>>
                       writed_state_textures;
    Set<VulkanBuffer*> writed_state_buffers;

    UnorderedMap<VulkanBuffer*, BufferState> flush_buffer_states;
    UnorderedMap<VulkanBuffer*, BufferRange> flush_buffer_ranges;
    Set<VulkanBuffer*>                        buffer_overlap_states;
};
} // namespace Moer::Render
#endif
