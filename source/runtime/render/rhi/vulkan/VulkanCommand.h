#pragma once

#include "VulkanCommon.h"
#include "VulkanRHIResource.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHIResource.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace Moer::Render {

class VulkanDevice;
class VkNativeQueryPool;
struct VulkanDescriptorPushLeaseState;

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
    std::string_view GetQueueName() const;

private:
    EQueueType    queue_type;
    VkCommandPool command_pool{VK_NULL_HANDLE};
};

class VulkanCmdList {
public:
    VulkanCmdList(
        VulkanCommandPool*   _command_pool,
        VulkanDevice&        _device,
        VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY
    );
    ~VulkanCmdList();
    VulkanCmdList(const VulkanCmdList&)            = delete;
    VulkanCmdList& operator=(const VulkanCmdList&) = delete;
    VulkanCmdList(VulkanCmdList&&)                 = delete;
    VulkanCmdList& operator=(VulkanCmdList&&)      = delete;

    VkCommandBuffer GetHandle() const {
        return command_buffer;
    }
    VkCommandBufferLevel GetLevel() const {
        return level;
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
    void BindPipeline(const PipelineHandle& _pso_handle);
    void BindShaderArguments(const PipelineHandle& _pipeline_handle, const ArrayArguments& _arguments);
    void SetDescriptorPushLease(std::shared_ptr<VulkanDescriptorPushLeaseState> _lease) {
        descriptor_push_lease = std::move(_lease);
    }

    // Vertex input and dynamic state.
    void SetVertexBuffers(
        uint32_t                      _first_binding,
        std::span<const VkBuffer>     _buffers,
        std::span<const VkDeviceSize> _offsets
    );
    void SetIndexBuffer(VulkanBuffer* _buffer, uint64 _offset, VkIndexType _type);
    void SetScissor(const VkRect2D& _scissor);
    void SetViewport(const VkViewport& _viewport);

    // Draw and mesh commands.
    void DrawInstanced(
        uint32_t _vertex_count,
        uint32_t _instance_count,
        uint32_t _start_vertex_location,
        uint32_t _start_instance_location
    );
    void DrawIndexedInstanced(
        uint32_t _index_count,
        uint32_t _instance_count,
        uint32_t _start_index_location,
        uint32_t _base_vertex_location,
        uint32_t _start_instance_location
    );
    void DrawIndirect(VulkanBuffer* _arg_buffer, uint64 _arg_offset, uint32_t _draw_count, uint32_t _stride);
    void DrawIndirectCount(
        VulkanBuffer* _arg_buffer,
        uint64        _arg_offset,
        VulkanBuffer* _count_buffer,
        uint64        _count_buffer_offset,
        uint32_t      _max_draw_count,
        uint32_t      _stride
    );
    void DrawIndexedIndirect(
        VulkanBuffer* _arg_buffer,
        uint64        _arg_offset,
        uint32_t      _draw_count,
        uint32_t      _stride
    );
    void DrawIndexedIndirectCount(
        VulkanBuffer* _arg_buffer,
        uint64        _arg_offset,
        VulkanBuffer* _count_buffer,
        uint64        _count_buffer_offset,
        uint32_t      _max_draw_count,
        uint32_t      _stride
    );
    void DispatchMesh(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z);
    void DispatchMeshIndirect(VulkanBuffer* _buffer, uint64 _offset, uint32_t _draw_count, uint32_t _stride);
    void DispatchMeshIndirectCount(
        VulkanBuffer* _arg_buffer,
        uint64        _arg_offset,
        VulkanBuffer* _count_buffer,
        uint64        _count_buffer_offset,
        uint32_t      _max_draw_count,
        uint32_t      _stride
    );

    // Compute commands.
    void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z);
    void DispatchIndirect(VulkanBuffer* _buffer, uint64 _offset);

    // Transfer and clear commands.
    void
    CopyBuffer(VulkanBuffer* _src, VulkanBuffer* _dst, uint64 _size, uint64 _src_offset, uint64 _dst_offset);
    void CopyBufferToTexture(
        VulkanBuffer*  _src,
        VulkanTexture* _dst,
        uint64         _size,
        uint64         _src_offset,
        uint3          _dst_offset,
        uint3          _dst_extent,
        uint32         _mip_level,
        uint32         _array_layer
    );
    void CopyTextureToBuffer(
        VulkanTexture* _src,
        VulkanBuffer*  _dst,
        uint64         _size,
        uint3          _src_offset,
        uint64         _dst_offset,
        uint3          _src_extent,
        uint32         _mip_level,
        uint32         _array_layer
    );
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
    void ClearBufferUInt(VulkanBuffer* _buffer, uint64 _offset, uint64 _size, uint _data);
    void ClearTexture(
        VulkanTexture*                 _texture,
        const VkClearColorValue&       _color,
        const VkImageSubresourceRange& _range
    );

    // CPU buffer access.
    void  CopyData(const BufferView& _dst, const void* _data, uint64 _size);
    void  CopyData(void* _dst, const BufferView& _src, uint64 _size);
    void* MapBuffer(const BufferView& _src);
    void  UnmapBuffer(const BufferView& _buffer);

    // Acceleration structures.
    void BuildAccelerationStructures(
        const Array<VkAccelerationStructureBuildGeometryInfoKHR>& _build_infos,
        const Array<VkAccelerationStructureBuildRangeInfoKHR*>&   _range_infos
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
    VkCommandBuffer                                 command_buffer{VK_NULL_HANDLE};
    VulkanCommandPool*                              command_pool;
    VulkanDevice&                                   device;
    VkCommandBufferLevel                            level;
    std::shared_ptr<VulkanDescriptorPushLeaseState> descriptor_push_lease;
};

} // namespace Moer::Render
