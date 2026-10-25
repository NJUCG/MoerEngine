#pragma once

#include "VulkanCommon.h"
#include "VulkanDescriptorRecordingPlan.h"
#include "VulkanDeviceObject.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHIResource.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace Moer::Render {

class VulkanDevice;
class VulkanBuffer;
class VulkanTexture;
struct ArrayArguments;
class VkNativeQueryPool;
struct VulkanResourceBinding;

class VulkanCommandPool : public VulkanDeviceObject {
public:
    VulkanCommandPool(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanCommandPool();
    VulkanCommandPool(const VulkanCommandPool&)            = delete;
    VulkanCommandPool& operator=(const VulkanCommandPool&) = delete;
    VulkanCommandPool(VulkanCommandPool&&)                 = delete;
    VulkanCommandPool& operator=(VulkanCommandPool&&)      = delete;

    VkCommandPool GetHandle() const {
        return command_pool;
    }
    VulkanDevice& GetDevice() const {
        return *m_device;
    }
    EQueueType GetQueueType() const {
        return queue_type;
    }
    std::string_view GetQueueName() const;

private:
    EQueueType    queue_type;
    VkCommandPool command_pool{VK_NULL_HANDLE};
};

// Owns one native command buffer. Submission completion and pool reset belong
// to the surrounding command context; recording here does not submit work.
class VulkanCommandList {
public:
    explicit VulkanCommandList(
        VulkanCommandPool&   _command_pool,
        VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY
    );
    ~VulkanCommandList();
    VulkanCommandList(const VulkanCommandList&)            = delete;
    VulkanCommandList& operator=(const VulkanCommandList&) = delete;
    VulkanCommandList(VulkanCommandList&&)                 = delete;
    VulkanCommandList& operator=(VulkanCommandList&&)      = delete;

    VkCommandBuffer GetHandle() const {
        return command_buffer;
    }

    // Recording lifecycle.
    [[nodiscard]] VkResult Begin();
    [[nodiscard]] VkResult BeginSecondary(
        const VkCommandBufferInheritanceInfo& _inheritance,
        VkCommandBufferUsageFlags             _flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    );
    [[nodiscard]] VkResult End();
    void                   ExecuteSecondaryCommands(std::span<const VkCommandBuffer> _secondary_buffers);

    // Rendering scope.
    void BeginRendering(const VkRenderingInfo& _info);
    void EndRendering();

    // Pipeline and shader arguments.
    void BindPipeline(const PipelineHandle& _pipeline_handle);
    void BindShaderArguments(const PipelineHandle& _pipeline_handle, const ArrayArguments& _arguments);
    void SetDescriptorRecordingPlan(std::shared_ptr<const VulkanDescriptorRecordingPlan> _plan) {
        descriptor_cursor = {};
        descriptor_recording_plan = std::move(_plan);
    }
    void BeginDescriptorCommand(const Command* _command) {
        descriptor_cursor = {};
        if (descriptor_recording_plan) {
            const auto it = descriptor_recording_plan->commands.find(_command);
            if (it != descriptor_recording_plan->commands.end()) {
                descriptor_cursor = VulkanDescriptorBufferAllocator::Cursor(it->second);
            }
        }
    }
    uint64 GetUsedDescriptorBytes() const {
        return used_descriptor_bytes;
    }

    // Vertex input and dynamic state.
    void SetVertexBuffers(
        uint32_t                      _first_binding,
        std::span<const VkBuffer>     _buffers,
        std::span<const VkDeviceSize> _offsets
    );
    void SetIndexBuffer(VulkanBuffer* _buffer, uint64 _offset, VkIndexType _index_type);
    void SetScissor(const VkRect2D& _scissor);
    void SetViewport(const VkViewport& _viewport);

    // Draw and mesh commands.
    void DrawInstanced(
        uint32_t _vertex_count,
        uint32_t _instance_count,
        uint32_t _first_vertex,
        uint32_t _first_instance
    );
    void DrawIndexedInstanced(
        uint32_t _index_count,
        uint32_t _instance_count,
        uint32_t _first_index,
        uint32_t _vertex_offset,
        uint32_t _first_instance
    );
    void DrawIndirect(VulkanBuffer* _buffer, uint64 _offset, uint32_t _draw_count, uint32_t _stride);
    void DrawIndirectCount(
        VulkanBuffer* _commands,
        uint64        _commands_offset,
        VulkanBuffer* _count_buffer,
        uint64        _count_buffer_offset,
        uint32_t      _max_draw_count,
        uint32_t      _stride
    );
    void DrawIndexedIndirect(VulkanBuffer* _buffer, uint64 _offset, uint32_t _draw_count, uint32_t _stride);
    void DrawIndexedIndirectCount(
        VulkanBuffer* _commands,
        uint64        _commands_offset,
        VulkanBuffer* _count_buffer,
        uint64        _count_buffer_offset,
        uint32_t      _max_draw_count,
        uint32_t      _stride
    );
    void DispatchMesh(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z);
    void DispatchMeshIndirect(VulkanBuffer* _buffer, uint64 _offset, uint32_t _draw_count, uint32_t _stride);
    void DispatchMeshIndirectCount(
        VulkanBuffer* _commands,
        uint64        _commands_offset,
        VulkanBuffer* _count_buffer,
        uint64        _count_buffer_offset,
        uint32_t      _max_draw_count,
        uint32_t      _stride
    );

    // Compute commands.
    void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z);
    void DispatchIndirect(VulkanBuffer* _buffer, uint64 _offset);

    // Transfer and clear commands.
    void CopyBuffer(
        VulkanBuffer* _src,
        VulkanBuffer* _dst,
        uint64        _byte_size,
        uint64        _src_offset,
        uint64        _dst_offset
    );
    // Tightly packed copies of one mip/layer in a supported single-aspect format.
    void CopyBufferToTexture(
        VulkanBuffer*  _src,
        VulkanTexture* _dst,
        uint64         _src_offset,
        uint3          _dst_offset,
        uint3          _extent,
        uint32         _mip_level,
        uint32         _array_layer
    );
    void CopyTextureToBuffer(
        VulkanTexture* _src,
        VulkanBuffer*  _dst,
        uint3          _src_offset,
        uint64         _dst_offset,
        uint3          _extent,
        uint32         _mip_level,
        uint32         _array_layer
    );
    // Copies array layer zero only; the RHI image-copy command has no layer fields.
    void CopyTexture(
        VulkanTexture* _src,
        VulkanTexture* _dst,
        uint3          _extent,
        uint3          _src_offset,
        uint3          _dst_offset,
        uint32         _src_mip_level,
        uint32         _dst_mip_level,
        VkImageLayout  _src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VkImageLayout  _dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
    );
    void ClearBufferUInt(VulkanBuffer* _buffer, uint64 _offset, uint64 _byte_size, uint32 _data);
    void ClearColorTexture(
        VulkanTexture*                 _texture,
        const VkClearColorValue&       _color,
        const VkImageSubresourceRange& _range
    );

    // Acceleration structures.
    void BuildAccelerationStructures(
        const Array<VkAccelerationStructureBuildGeometryInfoKHR>& _build_infos,
        const Array<VkAccelerationStructureBuildRangeInfoKHR*>&   _build_ranges
    );

    // Queries.
    void ResetQueryPool(VkNativeQueryPool& _query_pool, uint32 _first_query, uint32 _query_count);
    void BeginQuery(VkNativeQueryPool& _query_pool, uint32 _query, VkQueryControlFlags _flags = 0);
    void EndQuery(VkNativeQueryPool& _query_pool, uint32 _query);
    void WriteTimestamp(VkNativeQueryPool& _query_pool, uint32 _query, VkPipelineStageFlagBits2 _stage);

    // Debug labels.
    void BeginLabel(std::string_view _label, float4 _color);
    void EndLabel();
    void InsertLabel(std::string_view _label, float4 _color);

private:
    VkCommandBuffer             command_buffer{VK_NULL_HANDLE};
    VulkanCommandPool&          command_pool;
    VkCommandBufferLevel        level;
    std::shared_ptr<const VulkanDescriptorRecordingPlan> descriptor_recording_plan;
    VulkanDescriptorBufferAllocator::Cursor descriptor_cursor;
    uint64 used_descriptor_bytes{0};
    // Same lifetime as this command buffer. Reset only when Begin is legal again.
    struct DescriptorPoolPage {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        uint remaining_sets = 64;
        Array<VkDescriptorPoolSize> remaining_descriptors;
    };
    Array<DescriptorPoolPage> descriptor_pools;
    Array<VkBufferView>     descriptor_buffer_views;
    void                    ResetTransientDescriptors();
    VkDescriptorSet
         AllocateDescriptorSet(VkDescriptorSetLayout layout, std::span<const VulkanResourceBinding> bindings);
    void BindPipelineDescriptorSets(
        const class VulkanPipelineState& pipeline,
        const ArrayArguments& arguments,
        uint64 resource_byte_offset = 0
    );
    void BindHeapDescriptors(
        const class VulkanPipelineState& pipeline,
        const ArrayArguments&            arguments,
        uint64                           allocation_offset
    );
};

} // namespace Moer::Render
