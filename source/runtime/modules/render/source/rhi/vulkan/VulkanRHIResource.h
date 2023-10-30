//
// Created by 74535 on 2023/10/12.
//

#ifndef VULKAN_RHI_RESOURCE_H
#define VULKAN_RHI_RESOURCE_H

#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"
#include "shader/ShaderCommon.h"

#include <vulkan/vulkan.h>

#include <vk_mem_alloc.h>

class VulkanDevice;
class VulkanRHIImpl;

#pragma region forward definitions
class VulkanRHICommandList;
class VulkanRHITexture;
class VulkanRHIAmplificationShader;
class VulkanRHIBlendState;
class VulkanRHIShaderBoundStateInput;
class VulkanRHIBuffer;
class VulkanRHIComputePipelineState;
class VulkanRHIComputeShader;
class VulkanRHIDepthStencilState;
class VulkanRHIGeometryShader;
class VulkanRHIFence;
class VulkanRHIGraphicsPipelineState;
class VulkanRHIMeshShader;
class VulkanRHIPipelineBinaryDataLibrary;
class VulkanRHIFragmentShader;
class VulkanRHIRasterizationState;
class VulkanRHIRayTracingGeometry;
class VulkanRHIRayTracingPipelineState;
class VulkanRHIRayTracingScene;
class VulkanRHIRayTracingAccelerationStructure;
class VulkanRHIRayTracingShader;
class VulkanRHIRenderQuery;
class VulkanRHIRenderQueryPool;
class VulkanRHISampler;
class VulkanRHIMultisampleState;
class VulkanRHIShader;
class VulkanRHIShaderLibrary;
class VulkanRHIShaderResourceView;
class VulkanRHIStagingBuffer;
class VulkanRHITextureReference;
class VulkanRHIGlobalBufferLayout;
class VulkanRHIGlobalBuffer;
class VulkanRHIUnorderedAccessView;
class VulkanRHIVertexInputState;
class VulkanRHIVertexShader;
class VulkanRHIViewableResource;
class VulkanRHIViewport;
#pragma endregion

#pragma region utils definition

class VulkanMemoryManager final {
public:
    VulkanMemoryManager()                                      = delete;
    VulkanMemoryManager(const VulkanMemoryManager&)            = delete;
    VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;

    static VmaMemoryUsage MEGenerateVmaMemoryUsage();
};

class VulkanEnumTranslator final {
public:
    static VkFormat METoVKFormat(EPixelFormat _format);

    static VkSampleCountFlagBits METoVKSampleCountFlagBits(uint32_t _me_count);
    static VkImageAspectFlags    METoVKImageAspectFlags(ETextureAspectFlags _flags);
    static VkImageViewType       METoVKImageViewType(ETextureDimension _dim);
    static VkImageLayout         METoVKImageLayout(ETextureLayout _layout);
    static VkAttachmentLoadOp    METoVKAttachmentLoadOp(EAttachmentLoadOp _load_op);
    static VkAttachmentStoreOp   METoVKAttachmentStoreOp(EAttachmentStoreOp _store_op);
    static VkFilter              METoVKImageFilter(ESamplerFilter _filter);

    static VkPipelineStageFlags METoVkPipelineStageFlags(ERHIPipelineStageFlags _flags);
    static VkAccessFlags        METoVkAccessFlags(ERHIAccessFlags _flags);

    static VkCullModeFlags     METoVKCullModeFlags(ERasterizerCullMode _cull_mode);
    static VkPrimitiveTopology METoVKPrimitiveTopology(EPrimitiveTopology _primitive_type);
    static VkPolygonMode       METoVKPolygonMode(ERasterizerFillMode _fill_mode);

    static VkDescriptorType   METoVKDescriptorType(EShaderParameterType _type);
    static VkShaderStageFlags METoVKShaderStageFlags(EShaderType _type);
};

#pragma endregion

class VulkanRHISampler final : public RHISampler {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHISampler() : RHISampler() {}

    void GenerateSamplerFromInitializer(const VulkanDevice* _device, const RHISamplerInitializer& _initializer);

private:
    VkFilter             METoVKMinMagFilterMode(ESamplerFilter _filter);
    VkSamplerMipmapMode  METoVKMipmapMode(ESamplerFilter _filter);
    VkSamplerAddressMode METoVKWrapMode(ESamplerAddressMode _address_mode);
    VkCompareOp          METoVKCompareOp(ESamplerCompareFunction _compare_op);

private:
    VkSampler m_sampler;
};

class VulkanRHIVertexInputState final : public RHIVertexInputState {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIVertexInputState() : RHIVertexInputState() {}

    void GenerateVertexInputStateFromInitializer(const VertexInputStateInitializerList& _init);

private:
    VkVertexInputRate METoVKVertexInputRate(EVertexInputRate _me_rate);

private:
    VkPipelineVertexInputStateCreateInfo m_input_state_create_info;

    uint32_t m_binding_count   = 0;
    uint32_t m_attribute_count = 0;

    std::array<VkVertexInputBindingDescription, MAX_VERTEX_ELEMENT_COUNT>   m_bindings;
    std::array<VkVertexInputAttributeDescription, MAX_VERTEX_ELEMENT_COUNT> m_attributes;
};

class VulkanRHIRasterizationState : public RHIRasterizationState {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIRasterizationState() : RHIRasterizationState() {}

    void GenerateRasterizationStateFromInitializer(const RHIRasterizationStateInitializer& _init);

    VkPipelineRasterizationStateCreateInfo GetHandle() const {
        return m_rasterization_state_create_info;
    }

private:
    VkPolygonMode   METoVKPolygonMode(ERasterizerFillMode _fill_mode);
    VkCullModeFlags METoVKCullModeFlags(ERasterizerCullMode _cull_mode);

private:
    VkPipelineRasterizationStateCreateInfo m_rasterization_state_create_info;
};

class VulkanRHIDepthStencilState : public RHIDepthStencilState {
public:
    explicit VulkanRHIDepthStencilState() : RHIDepthStencilState() {}

    void GenerateDepthStencilStateFromInitializer(const RHIDepthStencilStateInitializer& _init);

    VkPipelineDepthStencilStateCreateInfo GetHandle() const {
        return m_depth_stencil_state_create_info;
    }

private:
    VkCompareOp METoVKCompareOp(ECompareOption _compare_op);
    VkStencilOp METoVKStencilOp(EStencilOp _stencil_op);

private:
    VkPipelineDepthStencilStateCreateInfo m_depth_stencil_state_create_info;
};

class VulkanRHIMultisampleState : public RHIMultisampleState {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIMultisampleState() : RHIMultisampleState() {}

    void GenerateMultisampleStateFromInitializer(const RHIMultisampleStateInitializer& _init);

    VkPipelineMultisampleStateCreateInfo GetHandle() const {
        return m_multisample_state_create_info;
    }

private:
    VkPipelineMultisampleStateCreateInfo m_multisample_state_create_info;
};

class VulkanRHIBlendState : public RHIBlendState {
public:
    explicit VulkanRHIBlendState() : RHIBlendState() {}

    void GenerateBlendStateFromInitializer(const RHIBlendStateInitializer& _init);

    VkPipelineColorBlendStateCreateInfo GetHandle() const {
        return m_blend_state_create_info;
    }

private:
    VkBlendOp     METoVKBlendOp(EBlendOperation _blend_op);
    VkBlendFactor METoVKBlendFactor(EBlendFactor _blend_factor);

private:
    VkPipelineColorBlendStateCreateInfo m_blend_state_create_info;
};

#pragma region shader definitions

class VulkanRHIGraphicsShader {
public:
    explicit VulkanRHIGraphicsShader(const Shader* _shader) : m_shader(_shader) {}

    void CreateShaderModule(const VulkanDevice* _device, const std::vector<uint8_t>& _code);

    VkShaderModule GetHandle() const {
        return m_shader_module;
    }

protected:
    VkShaderModule m_shader_module;
    const Shader*  m_shader;
};

class VulkanRHIVertexShader : public RHIVertexShader, public VulkanRHIGraphicsShader {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIVertexShader(const Shader* _shader) : RHIVertexShader(), VulkanRHIGraphicsShader(_shader) {}
};

class VulkanRHIFragmentShader : public RHIFragmentShader, public VulkanRHIGraphicsShader {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIFragmentShader(const Shader* _shader) : RHIFragmentShader(), VulkanRHIGraphicsShader(_shader) {}
};

class VulkanRHIGeometryShader : public RHIGeometryShader, public VulkanRHIGraphicsShader {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIGeometryShader(const Shader* _shader) : RHIGeometryShader(), VulkanRHIGraphicsShader(_shader) {}
};

class VulkanRHIComputeShader : public RHIComputeShader, public VulkanRHIGraphicsShader {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIComputeShader(const Shader* _shader) : RHIComputeShader(), VulkanRHIGraphicsShader(_shader) {}
};

class VulkanRHIMeshShader : public RHIMeshShader, public VulkanRHIGraphicsShader {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIMeshShader(const Shader* _shader) : RHIMeshShader(), VulkanRHIGraphicsShader(_shader) {}
};

class VulkanRHIAmplificationShader : public RHIAmplificationShader, public VulkanRHIGraphicsShader {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIAmplificationShader(const Shader* _shader) : RHIAmplificationShader(), VulkanRHIGraphicsShader(_shader) {}
};

#pragma endregion

#pragma region pipeline states definitions

class VulkanRHIGraphicsPipelineState final : public RHIGraphicsPipelineState {
    friend VulkanRHIImpl;

public:
    VulkanRHIGraphicsPipelineState() : RHIGraphicsPipelineState() {}

    static std::vector<VkPipelineShaderStageCreateInfo> METoVKShaderStageCreateInfo(const RHIShaderBoundStateInput& _shader_bound_state);
    static VkPipelineVertexInputStateCreateInfo         METoVKVertexInputStateCreateInfo(const RHIVertexInputState& _vertex_input_state);

private:
    VkPipeline m_pipeline;
};
#pragma endregion

#pragma region global buffer definitions
#pragma endregion

#pragma region viewable resources definitions

class VulkanRHIBuffer final : public RHIBuffer {
    friend VulkanRHIImpl;

public:
    VulkanRHIBuffer() = delete;
    VulkanRHIBuffer(const RHIBufferInfo& _info) : RHIBuffer(_info) {}

    inline const VmaAllocation GetAllocation() const {
        return m_alloc.alloc;
    }

    inline VkBuffer GetHandle() const {
        return m_alloc.buffer;
    }

    static VkIndexType        METoVKIndexType(EIndexElementType _type);
    static VkBufferUsageFlags METoVKBufferUsageFlags(VulkanDevice* _device, EBufferUsageFlags _me_flags);

private:
    struct BufferAlloc {
        VkBuffer      buffer;
        VmaAllocation alloc;
    } m_alloc;
};

class VulkanRHITexture final : public RHITexture {
    friend VulkanRHIImpl;

public:
    VulkanRHITexture() = delete;
    explicit VulkanRHITexture(const RHITextureCreateInfo& _info) : RHITexture(_info) {}

    inline const VmaAllocation GetAllocation() const {
        return m_alloc.alloc;
    }

    inline VkImage GetHandle() const {
        return m_alloc.image;
    }

    static VkImageType       METoVKImageType(ETextureDimension _dim);
    static VkImageUsageFlags METoVKImageUsageFlags(ETextureUsageFlags _me_flags);

private:
    struct TextureAlloc {
        VkImage       image;
        VmaAllocation alloc;
    } m_alloc;
};

#pragma endregion

#pragma region shader param
#pragma endregion

#pragma region synchronization

class VulkanRHIFence final : public RHIFence {
public:
    VulkanRHIFence(VulkanDevice* _device);

    uint64_t GetValue() const override;

    void Wait(uint64_t value) override;

private:
    VulkanDevice* m_device;
    VkFence       m_fence;
    VkSemaphore   m_semaphore;
};

#pragma endregion

#pragma region viewable resources view definitions

class VulkanRHIUnorderedAccessView final : public RHIUnorderedAccessView {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIUnorderedAccessView(_resource, _viewInfo) {}

    inline VkImageView GetView() const { return m_view; }

private:
    VkImageView m_view;
};

class VulkanRHIShaderResourceView final : public RHIShaderResourceView {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIShaderResourceView(_resource, _viewInfo) {}

    inline VkImageView GetView() const { return m_view; }

private:
    VkImageView m_view;
};

#pragma endregion

#pragma region graphic pipeline definitions
#pragma endregion

#pragma region raytracing
#pragma endregion

#pragma region render query
#pragma endregion

#pragma region RDG resource creater
#pragma endregion

#endif//VULKAN_RHI_RESOURCE_H
