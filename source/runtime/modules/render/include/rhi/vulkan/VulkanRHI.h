#ifndef VULKAN_RHI_H
#define VULKAN_RHI_H

#include "IVulkanRHI.h"

#include "VulkanDevice.h"
#include "VulkanSwapChain.h"

class VulkanViewport;

class VulkanRHIImpl final : IVulkanRHI {
public:
    VulkanRHIImpl();

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

    /* create cpu visible buffer for direct data transfer */
    RHIStagingBufferRef RHICreateStagingBuffer() final override;

    RHIShaderBoundStateRef RHICreateShaderBoundStage(
        RHIVertexInputState* _vertex_input,
        RHIVertexShader*     _vertex_shader,
        RHIFragmentShader*   _fragment_shader,
        RHIGeometryShader*   _geometry_shader) final override;

    RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) final override;

    RHIComputePipelineStateRef RHICreateComputePipelineState(RHIComputeShader* _compute_shader) final override;

    RHIUniformBufferRef RHICreateUniformBuffer(const void* data, const RHIUniformBufferLayout* layout, EBufferUsageFlags _usage) final override;

    void RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) final override;

    RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info) final override;

    RHIShaderResourceViewRef  RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) final override;
    RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) final override;
#pragma endregion

public:
    struct Settings {
        /** @brief Activates validation layers (and message output) when set to true */
        bool validation = false;
    } settings;

protected:
    VkInstance               m_instance;
    std::vector<const char*> m_instance_layers;

    std::vector<const char*> m_instance_extensions;
    std::vector<const char*> m_enabled_instance_extensions;

    //std::shared_ptr<VulkanWindow> m_window;
    VkSurfaceKHR m_surface;

    std::shared_ptr<VulkanDevice>    m_device;
    std::shared_ptr<VulkanSwapChain> m_swap_chain;
    std::vector<VulkanViewport*>     m_viewports;
    std::shared_ptr<VulkanViewport>  m_current_viewport;

protected:
    void InitWindow();
    void InitVulkan();

    VkPhysicalDeviceFeatures GetEnabledDeviceFeatures() {}
    std::vector<const char*> GetEnabledDeviceExtensions() {
        return {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    }

#pragma region vulkan functions
private:
    void CreateInstance(bool _enable_validation);

#pragma endregion

#pragma region helper functions
private:
    std::vector<const char*> GetInstanceExtensions();

    std::vector<const char*> GetRequiredExtensionsSupported();

    bool CheckValidationLayerSupport(const char* layer_name);

#pragma endregion
};
#endif// !VULKAN_RHI_H
