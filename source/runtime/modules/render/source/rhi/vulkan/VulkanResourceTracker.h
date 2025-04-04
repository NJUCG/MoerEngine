#ifndef MOER_VK_RESOURCE_TRACKER_H
#define MOER_VK_RESOURCE_TRACKER_H
#include <volk.h>
#include "VulkanRHIResource.h"
#include "misc/Crc32.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"
namespace Moer::Render {
    class VkTracker {
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
                    last_stage  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
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

        void QueueTransferReleaseResource(VulkanBuffer* _buffer, uint _src_queue, uint _dst_queue);
        void QueueTransferReleaseResource(VulkanTexture* _texture, uint _src_queue, uint _dst_queue, VkImageLayout _src_layout, VkImageLayout _dst_layout);

        void QueueTransferAcquireResource(VulkanBuffer* _buffer, uint _src_queue, uint _dst_queue, VkAccessFlagBits2 _dst_access, VkPipelineStageFlagBits2 _dst_stage);
        void QueueTransferAcquireResource(VulkanTexture* _texture, uint _src_queue, uint _dst_queue, VkImageLayout _src_layout, VkImageLayout _dst_layout, VkAccessFlagBits2 _dst_access, VkPipelineStageFlagBits2 _dst_stage);

        void RecordState(
            VulkanBuffer*            _buffer,
            VkAccessFlagBits2        _access,
            VkPipelineStageFlagBits2 _stage,
            uint32_t                 _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
            uint32_t                 _dst_queue_family = VK_QUEUE_FAMILY_IGNORED);

        void FlushSrcState(VulkanBuffer* _buffer, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage);
        void FlushSrcState(VulkanTexture* _texture, VkAccessFlagBits2 _access, VkImageLayout _layout, VkPipelineStageFlagBits2 _stage);

        void RecordState(
            VulkanBuffer* _texture,
            std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&&,
            uint32_t _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
            uint32_t _dst_queue_family = VK_QUEUE_FAMILY_IGNORED);

        void RecordState(
            VulkanTexture*           _texture,
            VkAccessFlagBits2        _access,
            VkImageLayout            _layout,
            VkPipelineStageFlagBits2 _stage,
            uint8_t                  _mip_level        = 0,
            uint8_t                  _mip_count        = 1,
            uint32_t                 _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
            uint32_t                 _dst_queue_family = VK_QUEUE_FAMILY_IGNORED);

        void RegisterFlushBuffer(const BufferView& _view, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage);
        void RecordState(
            VulkanTexture* _texture,
            std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&&,
            uint32_t _src_queue_family = VK_QUEUE_FAMILY_IGNORED,
            uint32_t _dst_queue_family = VK_QUEUE_FAMILY_IGNORED);

        void SetPassType(EPassType _type) {
            pass_type = _type;
        }

        const Set<VulkanTexture*>& GetWritedStateTextures() const {
            return writed_state_textures;
        }

        const Set<VulkanBuffer*>& GetWritedStateBuffers() const {
            return writed_state_buffers;
        }

        void ResolveBarriers();

        void DispatchBarriers(class VulkanCmdList& _cmd_list);
        void RestoreState();
        void Reset();

        //automic state transition
        auto ReadBuffer(VulkanBuffer*, EBufferState, EPassType _type = EPassType::Graphics) -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2>;
        auto WriteBuffer(VulkanBuffer*, EBufferState, EPassType _type = EPassType::Graphics) -> std::tuple<VkAccessFlags2, VkPipelineStageFlags2>;
        auto ReadTexture(VulkanTexture*, ETextureState, EPassType _type = EPassType::Graphics) -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>;
        auto WriteTexture(VulkanTexture*, ETextureState, EPassType _type = EPassType::Graphics) -> std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>;

        void EmplaceWriteBLAS(uint64 _blas_buf);
        bool ContainsWriteBLAS(uint64 _blas_buf);

        const Set<uint64>& GetWriteBLASStates() const {
            return write_blas_states;
        }

        void MarkWriteable(VulkanTexture* _texture, bool _writeable = true);
        void MarkWriteable(VulkanBuffer* _buffer, bool _writeable = true);

    private:
        EPassType             pass_type;
        EQueueType            queue_type  = EQueueType::Graphics;
        VkPipelineStageFlags2 last_stage  = 0;
        VkPipelineStageFlags2 first_stage = 0;

        Array<VkBufferMemoryBarrier2> buffer_barriers;
        Array<VkImageMemoryBarrier2>  texture_barriers;
        Array<VkMemoryBarrier2>       memory_barriers;

        UnorderedMap<VulkanBuffer*, BufferState>   buffer_states;
        UnorderedMap<VulkanTexture*, TextureState> texture_states;
        Set<VulkanTexture*>                        exported_textures;
        Set<VulkanBuffer*>                         exported_buffers;

        Set<VulkanBuffer*>  pending_buffers;
        Set<VulkanTexture*> pending_textures;

        Set<uint64>         write_blas_states;
        Set<VulkanTexture*> writed_state_textures;
        Set<VulkanBuffer*>  writed_state_buffers;

        UnorderedMap<VulkanBuffer*, BufferState> flush_buffer_states;
    };
}// namespace Moer::Render
#endif