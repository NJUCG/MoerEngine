#ifndef VULKAN_RHI_H
#define VULKAN_RHI_H

#include "IVulkanRHI.h"

#include <vk_mem_alloc.h>

struct GLFWwindow;
class VulkanDevice;
class VulkanSwapChain;
class VulkanViewport;
class VulkanRHIBuffer;

class VulkanRHIImpl final : public IVulkanRHI {
public:
    VulkanRHIImpl(GLFWwindow* _window);

    void Initialize() final override;

    void ShutDown() final override;

    inline const char* GetName() final override { return "VulkanRHI"; }

#pragma region resources creation
    RHISamplerRef            RHICreateSampler(const RHISamplerInitializer& _initializer) final override;
    RHIRasterizationStateRef RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) final override;
    RHIDepthStencilStateRef  RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) final override;
    RHIMultisampleStateRef   RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) final override;
    RHIBlendStateRef         RHICreateBlendState(const RHIBlendStateInitializer& _init) final override;
    RHIVertexInputStateRef   RHICreateVertexInputState(const VertexInputStateInitializerList& _init) final override;

    RHIVertexShaderRef   RHICreateVertexShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) final override;
    RHIFragmentShaderRef RHICreateFragmentShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) final override;
    RHIGeometryShaderRef RHICreateGeometryShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) final override;

    RHIMeshShaderRef          RHICreateMeshShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) final override;
    RHIAmplificationShaderRef RHICreateAmplificationShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) final override;

    RHIComputeShaderRef RHICreateComputeShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) final override;

    RHIShaderLibraryRef RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) final override;

    RHIFenceRef RHICreateFence(const std::string& name) final override;

    RHIShaderBoundStateRef RHICreateShaderBoundStage(
        RHIVertexInputState* _vertex_input,
        RHIVertexShader*     _vertex_shader,
        RHIFragmentShader*   _fragment_shader,
        RHIGeometryShader*   _geometry_shader) final override;

    RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) final override;

    RHIComputePipelineStateRef RHICreateComputePipelineState(RHIComputeShader* _compute_shader) final override;

    void         RHIUploadBuffer(RHIBufferRef _buffer_ref, const uint8_t* _data, uint32_t _size) final override;
    void         RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) final override;
    RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info) final override;
    void*        RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) final override;
    void         RHIUnmapBuffer(RHIBuffer* _buffer) final override;

    RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) final override;

    RHIShaderResourceViewRef  RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) final override;
    RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) final override;

    RHICommandQueue*        CreateCommandQueue(ECommandQueueType type) final override;
    RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr) final override;
    RHIComputeCommandList*  CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr) final override;

#pragma endregion

protected:
    VkInstance               m_instance;
    std::vector<std::string> m_instance_layers;

    std::vector<std::string> m_instance_extensions;
    std::vector<std::string> m_enabled_instance_extensions;

    VkSurfaceKHR m_surface;
    VmaAllocator m_allocator;

    VulkanDevice*                m_device;
    VulkanSwapChain*             m_swap_chain;
    std::vector<VulkanViewport*> m_viewports;
    VulkanViewport*              m_current_viewport;

protected:
    void InitSurface(GLFWwindow* _window);
    void InitVulkan();
    void InitVulkanMemoryAllocator();

#pragma region vulkan functions
private:
    void CreateInstance();

#pragma endregion

#pragma region helper functions
private:
    bool CheckEnabledExtensions();

    bool CheckValidationLayer(const std::string& layer_name);

    VkCommandBuffer BeginSingleTimeCommands(VkCommandPool _pool);
    void            EndSingleTimeCommands(VkCommandBuffer _command_buffer, VkCommandPool _pool, VkQueue _queue);

    void CopyBuffer(VulkanRHIBuffer* _src, VulkanRHIBuffer* _dst);

#pragma endregion
};
#endif// !VULKAN_RHI_H
