#ifndef MOER_VK_RESOURCE_TRACKER_H
#define MOER_VK_RESOURCE_TRACKER_H
#include <volk.h>
#include "VulkanRHIResource.h"
#include "misc/Crc32.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
namespace Moer::Render {
    class VkTracker {
        struct BufferState {
            VkAccessFlagBits2        src_access;
            VkPipelineStageFlagBits2 src_stage;
            VkAccessFlagBits2        dst_access;
            VkPipelineStageFlagBits2 dst_stage;
        };
        struct TextureState {
            VkAccessFlagBits2        src_access;
            VkImageLayout            src_layout;
            VkPipelineStageFlagBits2 src_stage;
            VkAccessFlagBits2        dst_access;
            VkImageLayout            dst_layout;
            VkPipelineStageFlagBits2 dst_stage;
        };

    public:
        VkTracker()  = default;
        ~VkTracker() = default;

        void RecordState(
            VulkanBuffer*            _buffer,
            VkAccessFlagBits2        _access,
            VkPipelineStageFlagBits2 _stage);

        void RecordState(
            VulkanBuffer* _texture,
            std::tuple<VkAccessFlags2, VkPipelineStageFlags2>&&);

        void RecordState(
            VulkanTexture*           _texture,
            VkAccessFlagBits2        _access,
            VkImageLayout            _layout,
            VkPipelineStageFlagBits2 _stage,
            uint8_t                  _mip_level = 0,
            uint8_t                  _mip_count = 1);

        void RecordState(
            VulkanTexture* _texture,
            std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2>&&);

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
        EPassType                     pass_type;
        Array<VkBufferMemoryBarrier2> buffer_barriers;
        Array<VkImageMemoryBarrier2>  texture_barriers;
        Array<VkMemoryBarrier2>       memory_barriers;

        UnorderedMap<VulkanBuffer*, BufferState>   buffer_states;
        UnorderedMap<VulkanTexture*, TextureState> texture_states;

        Set<uint64>         write_blas_states;
        Set<VulkanTexture*> writed_state_textures;
        Set<VulkanBuffer*>  writed_state_buffers;
    };
}// namespace Moer::Render
#endif