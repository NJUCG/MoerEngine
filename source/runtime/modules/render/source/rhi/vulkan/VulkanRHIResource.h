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

class VulkanMemoryManager final {
public:
    VulkanMemoryManager()                                      = delete;
    VulkanMemoryManager(const VulkanMemoryManager&)            = delete;
    VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;

    static VmaMemoryUsage MEGenerateVmaMemoryUsage(EBufferUsageFlags _flags);
};

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

class VulkanRHISampler final : public RHISampler {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHISampler() : RHISampler() {}

private:
    VkSampler m_sampler;
};

class VulkanRHIVertexInputState final : public RHIVertexInputState {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIVertexInputState(const VkPipelineVertexInputStateCreateInfo& _info) : RHIVertexInputState(), m_input_state_create_info(_info) {}

    static VkVertexInputRate METoVKVertexInputRate(EVertexInputRate _me_rate);
    static VkFormat          METoVKFormat(EVertexElementType _me_format);

private:
    VkPipelineVertexInputStateCreateInfo m_input_state_create_info;
};

class VulkanRHIRasterizationState : public RHIRasterizationState {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIRasterizationState(const VkPipelineRasterizationStateCreateInfo& _info) : RHIRasterizationState(), m_rasterization_state_create_info(_info) {}

    static VkPolygonMode   METoVKPolygonMode(ERasterizerFillMode _fill_mode);
    static VkCullModeFlags METoVKCullModeFlags(ERasterizerCullMode _cull_mode);

private:
    VkPipelineRasterizationStateCreateInfo m_rasterization_state_create_info;
};

class VulkanRHIDepthStencilState : public RHIDepthStencilState {
public:
    explicit VulkanRHIDepthStencilState(const VkPipelineDepthStencilStateCreateInfo& _info) : RHIDepthStencilState(), m_depth_stencil_state_create_info(_info) {}

    static VkCompareOp METoVKCompareOp(ECompareOption _compare_op);
    static VkStencilOp METoVKStencilOp(EStencilOp _stencil_op);

private:
    VkPipelineDepthStencilStateCreateInfo m_depth_stencil_state_create_info;
};

class VulkanRHIMultisampleState : public RHIMultisampleState {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIMultisampleState(const VkPipelineMultisampleStateCreateInfo& _info) : RHIMultisampleState(), m_multisample_state_create_info(_info) {}

    static VkSampleCountFlagBits METoVKSampleCountFlagBits(uint32_t _me_count);

private:
    VkPipelineMultisampleStateCreateInfo m_multisample_state_create_info;
};

#pragma region pipeline states definitions

class VulkanRHIGraphicsPipelineState final : public RHIGraphicsPipelineState {
public:
    VulkanRHIGraphicsPipelineState() : RHIGraphicsPipelineState() {}

private:
    VkPipeline m_pipeline;
};
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
#pragma endregion

#endif//VULKAN_RHI_RESOURCE_H
