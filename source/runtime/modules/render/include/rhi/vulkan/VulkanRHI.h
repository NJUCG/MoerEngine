#ifndef VULKAN_RHI_H
#define VULKAN_RHI_H

#include "IVulkanRHI.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "window/WindowContext.h"

class VulkanDevice;
class VulkanSwapChain;
class VulkanViewport;
class VulkanRHIBuffer;
class VulkanDescriptorSetAllocator;
class VulkanCommandAllocator;

class VulkanRHIImpl final : public IVulkanRHI {
public:
    RENDER_API VulkanRHIImpl();

    void Initialize(const RHIInitInfo& _init) final override;

    void PostInit() final override;

    void ShutDown() final override;

    inline const char* GetName() final override { return "VulkanRHI"; }

#pragma region resources creation
    RHISamplerRef RHICreateSampler(const RHISamplerCreateInfo& _initializer) final override;

    RHIVertexShaderRef   RHICreateVertexShader(const class ShaderCodeEntry*, const Shader*) final override;
    RHIFragmentShaderRef RHICreateFragmentShader(const class ShaderCodeEntry*, const Shader*) final override;
    RHIGeometryShaderRef RHICreateGeometryShader(const class ShaderCodeEntry*, const Shader*) final override;

    RHIMeshShaderRef          RHICreateMeshShader(const class ShaderCodeEntry*, const Shader*) final override;
    RHIAmplificationShaderRef RHICreateAmplificationShader(const class ShaderCodeEntry*, const Shader*) final override;

    RHIComputeShaderRef RHICreateComputeShader(const class ShaderCodeEntry*, const Shader*) final override;

    RHIRayGenShaderRef          RHICreateRayGenShader(const class ShaderCodeEntry*, const Shader*) final override;
    RHIRayMissShaderRef         RHICreateRayMissShader(const class ShaderCodeEntry*, const Shader*) final override;
    RHIRayClosestHitShaderRef   RHICreateRayClosestHitShader(const class ShaderCodeEntry*, const Shader*) final override;
    RHIRayCallableShaderRef     RHICreateRayCallableShader(const class ShaderCodeEntry*, const Shader*) final override;
    RHIRayIntersectionShaderRef RHICreateRayIntersectionShader(const class ShaderCodeEntry*, const Shader*) final override;
    RHIRayAnyhitShaderRef       RHICreateRayAnyhitShader(const class ShaderCodeEntry*, const Shader*) final override;

    RHIShaderLibraryRef RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) final override;

    RHIFenceRef RHICreateFence(const RHIFenceCreateInfo&) final override;
    // RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInfo& _init) final override;
    RHIGraphicsPipelineStateRef RHICreateGraphicsPSO(RHIGraphicsPSOCreateInfo&& _init) final override;
    RHIComputePipelineStateRef  RHICreateComputePipelineState(RHIShader* _compute_shader) final override;

    RHIRayTracingPipelineStateRef RHICreateRayTracingPipelineState(const RHIRayTracingPipelineStateInitializer& _init) final override;

    void                 RHIBatchedBuildRayTracingBLAS(int batch_size, const RHIRayTracingBLASInitializer* _inits, RHIRayTracingBLASRef* results) final override;
    RHIRayTracingTLASRef RHIBuildRayTracingTLAS(const RHIRayTracingTLASInitializer& _init) final override;

    void* RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) final override;
    void  RHIUnmapBuffer(RHIBuffer* _buffer) final override;

    RHIBufferRef RHICreateStagingBuffer(uint64_t _byte_size) final override;

    RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) final override;

    RHICommandQueue* RHICreateCommandQueue(ECommandQueueType _type) final override;
    // RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr) final override;
    RHIGraphicsCommandList*   RHICreateGraphicsCommandList(RHICommandAllocator* _allocator, RHIGraphicsPipelineState* _initial_state = nullptr) final override;
    RHIComputeCommandList*    RHICreateComputeCommandList(RHICommandAllocator* _allocator, RHIComputePipelineState* _initial_state = nullptr) final override;
    RHIRayTracingCommandList* RHICreateRayTracingCommandList(RHICommandAllocator* _allocator, RHIRayTracingPipelineState* _initial_state = nullptr) final override;
    // RHIComputeCommandList* CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr) final override;
    RHICopyCommandList* RHICreateCopyCommandList(RHICommandAllocator* _allocator) final override;

    RHICommandAllocator* RHIGetCurrentCommandAllocator() final override;
#pragma endregion

#pragma region viewport

    virtual RHIViewport* RHIGetMainViewport() override;

    virtual RHIViewportRef RHICreateViewport(const RHIViewportInitializer& _init) override;

    virtual void RHIResizeViewport(RHIViewport* _viewport, Extent2D _size, bool _b_full_screen, EPixelFormat _format = PF_UNDEFINED) override;

    virtual RHIViewportNextBackBufferInfo RHIGetNextFrameViewportBufferInfo(RHIViewport* _viewport) override;

    virtual RHIUAV* RHIGetViewportBackBufferUAV(RHIViewport* _viewport, uint32_t index) override;

    virtual void RHIPresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) override;

#pragma endregion

protected:
    void         RHISetBatchedShaderParametersInner(RHIResource* _resource, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant) final override;
    RHIBufferRef RHICreateBufferInner(const RHIBufferCreateInfo& info) final override;
    RHISRVRef    RHICreateSRVInner(RHIViewableResource* _resource, const RHIViewInfo& _view_info) final override;
    RHIUAVRef    RHICreateUAVInner(RHIViewableResource* _resource, const RHIViewInfo& _view_info) final override;

protected:
    VkInstance               m_instance;
    Moer::Array<std::string> m_instance_layers;

    Moer::Array<std::string> m_instance_extensions;
    Moer::Array<std::string> m_enabled_instance_extensions;

    VkSurfaceKHR m_surface;

    VulkanDevice*   m_device;
    VulkanViewport* m_main_viewport;
    // VulkanSwapChain* m_swap_chain;
    // Moer::Array<VulkanViewport*> m_viewports;
    // VulkanViewport* m_current_viewport;

protected:
    void InitSurface(Moer::WindowHandle* _window);
    void InitVulkan();

#pragma region vulkan functions
private:
    void CreateInstance();

#pragma endregion

#pragma region helper functions
private:
    friend VulkanSwapChain;
    bool CheckValidationLayer(const std::string& layer_name);
    bool CheckEnabledExtensions();

    RHIBufferRef CreateBufferFromData(const RHIBufferCreateInfo& info, uint32_t size, void* data);

    VkCommandBuffer BeginSingleTimeCommands(VkCommandPool _pool);
    void            EndSingleTimeCommands(VkCommandBuffer _command_buffer, VkCommandPool _pool, VkQueue _queue);

    VkDeviceAddress GetDeviceAddress(RHIBufferRef _buffer);

    // void CopyBuffer(VulkanRHIBuffer* _src, VulkanRHIBuffer* _dst);

#pragma endregion
};
#endif// !VULKAN_RHI_H
