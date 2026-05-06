
#ifndef VULKAN_COMMAND_H
#define VULKAN_COMMAND_H
#include "misc/Traits.h"
#include "rhi/RHIResource.h"

#include "misc/STL.h"

#include "VulkanDescriptor.h"
#include "VulkanPlatform.h"
#include "VulkanRHIResource.h"
#include "VulkanResourceTracker.h"

namespace Moer::Render {
class VulkanDevice;
class VulkanDescriptorSetAllocator;
class VulkanRHIGraphicsPipelineState;

struct PushConstantInfo;
class VulkanCmdList {
    friend struct VkCustomDispatchCmd;

private:
    VkCommandBuffer           command_buffer;
    class VulkanCmdAllocator* allocator;
    VulkanDevice&             device;
    VulkanDescriptorBinder    descriptor_binder{};
    bool                      descriptor_binder_active{false};
    bool                      descriptor_state_valid{false};

private:
    void BindGlobalDescriptorHeaps(bool _bind_resource_heap, bool _bind_sampler_heap);

public:
    VulkanCmdList(VulkanCmdAllocator* _allocator, VulkanDevice& _device);
    ~VulkanCmdList();
    void Begin();
    void End();
    void SetDescriptorBinder(VulkanDescriptorBinder _binder);
    VulkanDescriptorBinder ReleaseDescriptorBinder();
    void InvalidateDescriptorState();
    void RestoreDescriptorState();
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
        uint32         _mip_level
    );
    void  CopyData(const BufferView& _dst, const void* _data, uint64 _size);
    void  CopyData(void* _dst, const BufferView& _src, uint64 _size);
    void* MapBuffer(const BufferView& _src);
    void  UnmapBuffer(const BufferView& _buffer);
    void  DrawIndexedInstanced(
         uint32_t _index_count,
         uint32_t _instance_count,
         uint32_t _start_index_location,
         uint32_t _base_vertex_location,
         uint32_t _start_instance_location
     );
    void DrawInstanced(
        uint32_t _vertex_count,
        uint32_t _instance_count,
        uint32_t _start_vertex_location,
        uint32_t _start_instance_location
    );
    void DrawIndexedIndirectCnt(
        VulkanBuffer* _arg_buffer,
        uint64        _arg_offset,
        VulkanBuffer* _count_buffer,
        uint64        _count_buffer_offset,
        uint32_t      _max_draw_count,
        uint32_t      _stride
    );
    void DrawIndirectCnt(
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
    void DrawIndirect(VulkanBuffer* _arg_buffer, uint64 _arg_offset, uint32_t _draw_count, uint32_t _stride);
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

    void CopyTexture(
        VulkanTexture* _src,
        VulkanTexture* _dst,
        uint3          _extent,
        uint3          _src_offset,
        uint3          _dst_offset,
        uint32         _src_mip_level,
        uint32         _dst_mip_level
    );
    void BeginRendering(VkRenderingInfo&& _info);
    void EndRendering();
    void SetVertexBuffers(
        uint                _first_binding,
        uint                _binding_cnt,
        std::span<VkBuffer> _buffers,
        std::span<uint64>   _offsets
    );
    void SetIndexBuffer(VulkanBuffer* _buffer, uint64 _offset, VkIndexType _type);
    void SetPso(const PipelineHandle& _pso_handle);
    void SetScissor(const VkRect2D& _scissor);
    void SetViewPort(const VkViewport& _viewport);
    void ClearBufferUInt(VulkanBuffer* _buffer, uint64 _offset, uint64 _size, uint _data);
    void ClearTexture(
        VulkanTexture*                 _texture,
        const VkClearColorValue&       _color,
        const VkImageSubresourceRange& _range
    );
    void ResetQueryPool(class VkNativeQueryPool& _query_pool, uint32 _first_query, uint32 _query_count);
    void BeginQuery(
        class VkNativeQueryPool& _query_pool,
        uint32                   _query,
        VkQueryControlFlags      _flags = 0
    );
    void EndQuery(class VkNativeQueryPool& _query_pool, uint32 _query);
    void WriteTimeStamp(VkNativeQueryPool& _query_pool, uint32 _query, VkPipelineStageFlagBits2 _stage);

    void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z);
    void DispatchIndirect(VulkanBuffer* _buffer, uint64 _offset);

    void UploadDescriptors(PipelineHandle& _pso_handle);
    void UploadPushConstants(PipelineHandle& _pso_handle, std::span<const uint> _data);
    void BindDescriptors(PipelineHandle& _pso_handle, const ArrayArguments& _args);

    //Raytracing
    void BuildAccelerationStructures(
        const Array<VkAccelerationStructureBuildGeometryInfoKHR>& _build_infos,
        const Array<VkAccelerationStructureBuildRangeInfoKHR*>&   _range_infos
    );

    //Debug
    void BeginLabel(StringView _label, float4 _color);
    void EndLabel();

    void InsertLabel(StringView _label, float4 _color);

    VkCommandBuffer GetHandle() const {
        return command_buffer;
    }
};

} // namespace Moer::Render
#endif //VULKAN_COMMAND_H
