//
// Created by 74535 on 2023/10/12.
//

#ifndef VULKAN_RHI_RESOURCE_H
#define VULKAN_RHI_RESOURCE_H

#include "rhi/RHIResource.h"

#include <vulkan.h>
#define VMA_IMPLEMENTATION
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
    static VkSampleCountFlagBits METoVKSampleCountFlagBits(uint32_t _me_count);
    static VkImageViewType       METoVKImageViewType(ETextureDimension _dim);
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
    VkFormat          METoVKFormat(EVertexElementType _me_format);

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

private:
    VkPipelineMultisampleStateCreateInfo m_multisample_state_create_info;
};

class VulkanRHIBlendState : public RHIBlendState {
public:
    explicit VulkanRHIBlendState() : RHIBlendState() {}

    void GenerateBlendStateFromInitializer(const RHIBlendStateInitializer& _init);

private:
    VkBlendOp     METoVKBlendOp(EBlendOperation _blend_op);
    VkBlendFactor METoVKBlendFactor(EBlendFactor _blend_factor);

private:
    VkPipelineColorBlendStateCreateInfo m_blend_state_create_info;
};

#pragma region shader definitions
#pragma endregion

#pragma region pipeline states definitions

class VulkanRHIGraphicsPipelineState final : public RHIGraphicsPipelineState {
public:
    VulkanRHIGraphicsPipelineState() : RHIGraphicsPipelineState() {}

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
    static VkImageLayout     METoVKImageLayout(ETextureLayout _layout);

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
    VulkanRHIFence(const std::string& _name, VulkanDevice* _device);
    bool Signaled() const final override;

private:
    VulkanDevice* m_device;
    VkFence       m_fence;
};

#pragma endregion

#pragma region viewable resources view definitions

class VulkanRHIUnorderedAccessView final : public RHIUnorderedAccessView {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIUnorderedAccessView(_resource, _viewInfo) {}

private:
    VkImageView m_view;
};

class VulkanRHIShaderResourceView final : public RHIShaderResourceView {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIShaderResourceView(_resource, _viewInfo) {}

private:
    VkImageView m_view;
};

#pragma endregion

class VulkanRHIStagingBuffer : public RHIStagingBuffer {
    friend VulkanRHIImpl;

public:
    VulkanRHIStagingBuffer(uint64_t _byte_size) : RHIStagingBuffer(), m_gpu_byte_size(_byte_size) {}
    ~VulkanRHIStagingBuffer() {}

    uint64_t GetGPUByteSize() const override { return m_gpu_byte_size; }

    inline const VmaAllocation GetAllocation() const {
        return m_alloc.alloc;
    }

    inline VkBuffer GetHandle() const {
        return m_alloc.buffer;
    }

    void* GetSuballocationFromBuffer(uint32_t _size);

private:
    struct BufferAlloc {
        VkBuffer      buffer;
        VmaAllocation alloc;
    } m_alloc;

    void*    m_head_ptr;
    void*    m_cur_ptr;
    void*    m_tail_ptr;
    uint64_t m_gpu_byte_size;
};

#pragma region graphic pipeline definitions
#pragma endregion

#pragma region raytracing
#pragma endregion

#pragma region render query
#pragma endregion

#pragma region RDG resource creater
#pragma endregion

#endif//VULKAN_RHI_RESOURCE_H
