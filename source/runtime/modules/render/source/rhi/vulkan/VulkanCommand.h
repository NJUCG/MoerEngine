
#ifndef VULKAN_COMMAND_H
#define VULKAN_COMMAND_H
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "vulkan/vulkan_core.h"
#include "VulkanRHIResource.h"

#include <vulkan/vulkan.h>

class VulkanDevice;
class VulkanDescriptorSetAllocator;
class VulkanRHIGraphicsPipelineState;

struct PushConstantInfo;

class VulkanRHICommandListBase : public VulkanDeviceObject {
public:
    VulkanRHICommandListBase(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    virtual ~VulkanRHICommandListBase();
    void Open();
    void Close();
    void Reset();

    void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z);

    void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset);

    void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst);
    void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst);
    void CopyBufferToTexture(RHIBuffer* src_buffer, RHITexture* dst_texture, const RHICopyBufferToTextureInfo& _info);

    void CopyTextureToBuffer(RHITexture* src_texture, RHIBuffer* dst_buffer, const RHICopyTextureToBufferInfo& _info);

    //To copy regions of a source texture into a destination texture, potentially performing format conversion, arbitrary scaling, and filtering.
    //must not be used for multi-sampled source or destination textures, use resolve instead
    void BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst);

    //To resolve a multi-sample color texture to a non-multisample color texture
    void ResolveTexture(
        const RHIResolveTextureInfo& _blit_info,
        RHITexture*                  _src,
        RHITexture*                  _dst);

    void                   SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency);
    inline VkCommandBuffer GetHandle() const { return m_command_buffer; }

protected:
    VkCommandPool        m_current_command_pool;
    VkCommandBuffer      m_command_buffer;
    VkCommandBufferLevel m_level;
};

class VulkanCommandAllocator final : public RHICommandAllocator, public VulkanDeviceObject {
public:
    VulkanCommandAllocator(VulkanDevice* _device);
    virtual ~VulkanCommandAllocator();

    void Reset() override;

    inline VkCommandPool GetHandle(ECommandListType _type) const { return m_command_pool[(size_t)_type]; }

private:
    std::array<VkCommandPool, (size_t)ECommandListType::Num> m_command_pool;
};

class VulkanRHIGraphicsCommandList final : public RHIGraphicsCommandList,
                                           public VulkanRHICommandListBase {
public:
    VulkanRHIGraphicsCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    virtual ~VulkanRHIGraphicsCommandList();

    void* GetNativeHandle() const override {
        return m_command_buffer;
    }

    void SetPipelineState(RHIGraphicsPipelineState* _graphics_pso) override;
    void Open() override;
    void Close() override;
    void Reset() override;
    void ClearState(RHIGraphicsPipelineState* _graphics_pso) override;

    void DrawIndexedInstanced(uint32_t _index_count,
                              uint32_t _instance_count,
                              uint32_t _start_index_location,
                              uint32_t _start_vertex_location,
                              uint32_t _start_instance_location) override;

    void DrawIndexedIndirect(
        RHIBuffer* _argument_buffer,
        uint64_t   _arg_offset,
        RHIBuffer* _count_buffer,
        uint64_t   _count_buffer_offset,
        uint32_t   _max_draw_count,
        uint32_t   _stride) override;

    void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) override;

    void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) override;

    void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) override;
    void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) override;
    void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) override;

    void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) override;

    //To copy regions of a source texture into a destination texture, potentially performing format conversion, arbitrary scaling, and filtering.
    //must not be used for multi-sampled source or destination textures, use resolve instead
    void BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) override;

    //To resolve a multi-sample color texture to a non-multisample color texture
    void ResolveTexture(const RHIResolveTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) override;

    void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) override;

    void SetCullMode(ERasterizerCullMode _cull_mode) override;
    void SetPrimitiveTopology(EPrimitiveTopology _topology) override;
    void SetViewPorts(uint32_t num_viewports, const ViewPort* p_viewports) override;
    void SetViewPort(const ViewPort& _viewport) override;
    void SetScissors(uint32_t num_scissors, const Rect2D* p_scissors) override;
    void SetScissor(const Rect2D& _scissor) override;
    void SetBlendFactors(const float _factors[4]) override;

    void BindVertexBuffers(
        uint32_t            _start_index,
        uint32_t            _num_buffers,
        const RHIBufferRef* p_vertex_buffers,
        const uint32_t*     _offsets) override;

    void BindIndexBuffer(
        const RHIBuffer*  p_index_buffer,
        uint32_t          _offset,
        EIndexElementType _type) override;

    void SetAttachments() override {
    }

    void ClearDepthStencil() override;
    void ClearUAVInt(
        RHIUnorderedAccessView* _uav,
        const Moer::Vector4i&   _values) override;
    void ClearUAVFloat(
        RHIUnorderedAccessView* _uav,
        const Moer::Vector4f&   _values) override;

    void BeginRenderPass(const RHIRenderPassInfo& _pass_info, const char* _pass_name) override;
    void EndRenderPass() override;

    void NextSubpass() override;

    //todo: query data declaration
    void BeginQuery(RHIRenderQuery* _query) override;
    void EndQuery(RHIRenderQuery* _query) override;

    void GetQueryData(
        ERenderQueryType _query_type,
        uint32_t         _first_index,
        uint32_t         _num_queries,
        RHIBuffer*       _dst_buffer,
        uint64_t         _dst_offset) override;

    void ExecuteSubCommands(uint32_t                _num,
                            RHIGraphicsCommandList* _sub_commands) override;

#pragma region ray-tracing
    void BuildAccelerationStructure(
        RHIBuffer* _instance_data,
        uint64_t   _instance_offset,
        bool       _b_update,
        RHIBuffer* _scratch,
        RHIBuffer* _scratch_offset) override;

#pragma endregion
protected:
    friend class VulkanRHICommandQueue;

private:
    VulkanRHIGraphicsPipelineState* m_current_pipeline_state;

private:
    VkRenderingAttachmentInfo FromColorAttachmentInfo(const RHIRenderPassInfo::ColorAttachmentInfo& _color_attachment_info) const;
    VkRenderingAttachmentInfo FromDepthStencilAttachmentInfo(const RHIRenderPassInfo::DepthStencilAttachmentInfo& _depth_stencil_attachment_info) const;

    void PrepareDrawCommand();

    std::vector<VkDescriptorSet> m_bound_sets;
};

class VulkanRHICopyCommandList final : public RHICopyCommandList, public VulkanRHICommandListBase {
public:
    VulkanRHICopyCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    virtual ~VulkanRHICopyCommandList();
    void* GetNativeHandle() const override {
        return m_command_buffer;
    }
    void Open() override;
    void Close() override;
    void Reset() override;

    void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) override;
    void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) override;
    void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) override;

    void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) override;
    void BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) override;
    void ResolveTexture(const RHIResolveTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) override;

    void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) override;
};

class VulkanRHIComputeCommandList final : public RHIComputeCommandList, public VulkanRHICommandListBase {
public:
    VulkanRHIComputeCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

    virtual ~VulkanRHIComputeCommandList();

    void* GetNativeHandle() const override {
        return m_command_buffer;
    }
    void Open() override;
    void Close() override;
    void Reset() override;

    void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) override;
    void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) override;

    void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) override;
    void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) override;
    void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) override;

    void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) override;
    void BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) override;
    void ResolveTexture(const RHIResolveTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) override;

    void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) override;
};

class VulkanRHICommandQueue final : public RHICommandQueue,
                                    public VulkanDeviceObject {
public:
    VulkanRHICommandQueue(VulkanDevice* _device, ECommandQueueType _type);
    virtual ~VulkanRHICommandQueue();
    virtual void SubmitCommands(
        uint32_t                  _num_command_lists,
        const RHICommandListBase* _command_lists,
        const RHISubmitInfo*      _submit_info = nullptr) override;
    inline VkQueue GetHandle() { return queue; }

    virtual void WaitForQueueComplete() override;

private:
    VkQueue queue;
};

#endif//VULKAN_COMMAND_H
