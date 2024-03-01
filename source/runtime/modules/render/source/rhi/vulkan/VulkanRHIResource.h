//
// Created by 74535 on 2023/10/12.
//

#ifndef VULKAN_RHI_RESOURCE_H
#define VULKAN_RHI_RESOURCE_H

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/misc/VulkanTypeDefs.h"

#include "misc/STL.h"

#include "shader/ShaderCommon.h"

#include <vulkan/vulkan_core.h>

#include <vk_mem_alloc.h>

class VulkanDevice;
class VulkanRHIImpl;
class VulkanDescriptorSetsLayout;
class VulkanPipelineResourceCache;

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
class VulkanRHITextureSRV;
class VulkanRHIStagingBuffer;
class VulkanRHITextureReference;
class VulkanRHIGlobalBufferLayout;
class VulkanRHIGlobalBuffer;
class VulkanRHITextureUAV;
class VulkanRHIVertexInputState;
class VulkanRHIVertexShader;
class VulkanRHIViewableResource;
class VulkanViewport;
#pragma endregion

#pragma region utils definition

class VulkanMemoryManager final {
public:
    VulkanMemoryManager()                                      = delete;
    VulkanMemoryManager(const VulkanMemoryManager&)            = delete;
    VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;

    static VmaAllocationCreateFlags MEGenerateVmaMemoryFlags(EBufferUsageFlags _flags);
    static VmaMemoryUsage           MEGenerateVmaMemoryUsage();
};

class VulkanEnumTranslator final {
public:
    static VkFormat     METoVKFormat(EPixelFormat _format);
    static EPixelFormat VKToMEFormat(VkFormat _format);

    static VkSampleCountFlagBits METoVKSampleCountFlagBits(uint32_t _me_count);
    static VkImageAspectFlags    METoVKImageAspectFlags(ETextureAspectFlags _flags);
    static VkImageViewType       METoVKImageViewType(ETextureDimension _dim);
    static VkImageLayout         METoVKImageLayout(ETextureLayout _layout);
    static VkAttachmentLoadOp    METoVKAttachmentLoadOp(EAttachmentLoadOp _load_op);
    static VkAttachmentStoreOp   METoVKAttachmentStoreOp(EAttachmentStoreOp _store_op);
    static VkFilter              METoVKImageFilter(ESamplerFilter _filter);

    static VkPipelineStageFlags2 METoVkPipelineStageFlags2(ERHIPipelineStageFlags _flags);
    static VkAccessFlags2        METoVkAccessFlags2(ERHIAccessFlags _flags);

    static VkCullModeFlags     METoVKCullModeFlags(ERasterizerCullMode _cull_mode);
    static VkPrimitiveTopology METoVKPrimitiveTopology(EPrimitiveTopology _primitive_type);
    static VkPolygonMode       METoVKPolygonMode(ERasterizerFillMode _fill_mode);

    static VkDescriptorType   METoVKDescriptorType(EShaderParameterType _type, EShaderCodeResourceBindingType _binding_type);
    static VkShaderStageFlags METoVKShaderStageFlags(EShaderType _type);

    static uint32_t METoVkQueueFamilyIndex(ECommandQueueType _type, const VulkanDevice* _device);
    static uint32_t METoVkQueueFamilyIndex(ECommandListType _type, const VulkanDevice* _device);

    static VkFilter             METoVKMinMagFilterMode(ESamplerFilter _filter);
    static VkSamplerMipmapMode  METoVKMipmapMode(ESamplerFilter _filter);
    static VkSamplerAddressMode METoVKWrapMode(ESamplerAddressMode _address_mode);
    static VkCompareOp          METoVKCompareOpSampler(ESamplerCompareFunction _compare_op);

    static VkCompareOp METoVKCompareOp(ECompareOption _compare_op);
    static VkStencilOp METoVKStencilOp(EStencilOp _stencil_op);

    static VkBlendOp     METoVKBlendOp(EBlendOperation _blend_op);
    static VkBlendFactor METoVKBlendFactor(EBlendFactor _blend_factor);

    static VkVertexInputRate METoVKVertexInputRate(EVertexInputRate _me_rate);
};

#pragma endregion

class VulkanRHISampler final : public RHISampler {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHISampler() : RHISampler() {}

    void GenerateSamplerFromInitializer(const VulkanDevice* _device, const RHISamplerCreateInfo& _initializer);

    inline VkSampler GetHandle() const {
        return m_sampler;
    }

    inline VkImageLayout GetImageLayout() const {
        return m_image_layout;
    }

private:
    VkFilter             METoVKMinMagFilterMode(ESamplerFilter _filter);
    VkSamplerMipmapMode  METoVKMipmapMode(ESamplerFilter _filter);
    VkSamplerAddressMode METoVKWrapMode(ESamplerAddressMode _address_mode);
    VkCompareOp          METoVKCompareOp(ESamplerCompareFunction _compare_op);

private:
    VkSampler     m_sampler;
    VkImageLayout m_image_layout;
};

#pragma region shader definitions

class VulkanRHIGraphicsShader {
    friend VulkanRHIImpl;

public:
    explicit VulkanRHIGraphicsShader() : m_shader_module(VK_NULL_HANDLE) {}

    inline VkShaderModule GetHandle() const {
        return m_shader_module;
    }

protected:
    VkShaderModule m_shader_module;
};

class VulkanRHIVertexShader : public RHIVertexShader, public VulkanRHIGraphicsShader {
public:
    explicit VulkanRHIVertexShader(const Shader* _meta_shader) : RHIVertexShader(_meta_shader), VulkanRHIGraphicsShader() {}
};

class VulkanRHIFragmentShader : public RHIFragmentShader, public VulkanRHIGraphicsShader {
public:
    explicit VulkanRHIFragmentShader(const Shader* _meta_shader) : RHIFragmentShader(_meta_shader), VulkanRHIGraphicsShader() {}
};

class VulkanRHIGeometryShader : public RHIGeometryShader, public VulkanRHIGraphicsShader {
public:
    explicit VulkanRHIGeometryShader(const Shader* _meta_shader) : RHIGeometryShader(_meta_shader), VulkanRHIGraphicsShader() {}
};

class VulkanRHIComputeShader : public RHIComputeShader, public VulkanRHIGraphicsShader {
public:
    explicit VulkanRHIComputeShader(const Shader* _meta_shader) : RHIComputeShader(_meta_shader), VulkanRHIGraphicsShader() {}
};

class VulkanRHIMeshShader : public RHIMeshShader, public VulkanRHIGraphicsShader {
public:
    explicit VulkanRHIMeshShader(const Shader* _meta_shader) : RHIMeshShader(_meta_shader), VulkanRHIGraphicsShader() {}
};

class VulkanRHIAmplificationShader : public RHIAmplificationShader, public VulkanRHIGraphicsShader {
public:
    explicit VulkanRHIAmplificationShader(const Shader* _meta_shader) : RHIAmplificationShader(_meta_shader), VulkanRHIGraphicsShader() {}
};

#pragma endregion

#pragma region pipeline states definitions

class VulkanPipelineState {
public:
    VulkanPipelineState() : m_pipeline(VK_NULL_HANDLE), m_pipeline_layout(VK_NULL_HANDLE), m_pipeline_state_cache(nullptr){};
    virtual ~VulkanPipelineState() = default;

    inline VkPipeline GetHandle() const {
        return m_pipeline;
    }

    inline const VkPipelineLayout GetPipelineLayout() const {
        return m_pipeline_layout;
    }

    inline const VulkanDescriptorSetsLayout* GetDescriptorSetsLayout() const {
        return m_descriptor_sets_layout;
    }

    inline VulkanPipelineResourceCache* GetPipelineResourceCache() const {
        return m_pipeline_state_cache;
    }

    void GenerateDescriptorSetLayouts(const VulkanDevice* _device, Moer::Array<TDescriptorSetLayoutInfo>& _layout_mappings);
    void CreateResourceCache();

protected:
    VkPipeline       m_pipeline;
    VkPipelineLayout m_pipeline_layout;
    // descriptor sets
    VulkanDescriptorSetsLayout* m_descriptor_sets_layout;
    // resource cache
    VulkanPipelineResourceCache* m_pipeline_state_cache;
    // descriptor sets
};

class VulkanRHIGraphicsPipelineState final : public RHIGraphicsPipelineState, public VulkanPipelineState {
    friend VulkanRHIImpl;

public:
    VulkanRHIGraphicsPipelineState()
        : RHIGraphicsPipelineState(),
          VulkanPipelineState() {}

    virtual ~VulkanRHIGraphicsPipelineState();

    static Moer::Array<VkPipelineShaderStageCreateInfo> METoVKShaderStageCreateInfo(const RHIShaderBoundStateInput& _shader_bound_state);
    static VkPipelineVertexInputStateCreateInfo         METoVKVertexInputStateCreateInfo(const RHIVertexInputInfo& _vertex_input_state);
    static Moer::Array<const Shader*>                   GetShaderInfoList(const RHIShaderBoundStateInput& _shader_bound_state);
};

class VulkanRHIComputePipelineState final : public RHIComputePipelineState, public VulkanPipelineState {
    friend VulkanRHIImpl;

public:
    VulkanRHIComputePipelineState()
        : RHIComputePipelineState(),
          VulkanPipelineState() {}
};
#pragma endregion

#pragma region global buffer definitions
#pragma endregion

#pragma region viewable resources definitions

class VulkanRHIBuffer : public RHIBuffer {
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

class VulkanStagingBuffer : public RHIBuffer {
    friend VulkanRHIImpl;
    ~VulkanStagingBuffer();

public:
    VulkanStagingBuffer() = delete;
    VulkanStagingBuffer(VulkanRHIBuffer* _buffer);
};
class VulkanDeviceObject {
public:
    VulkanDeviceObject(VulkanDevice* _device = nullptr);

protected:
    VulkanDevice* m_device;
};
class VulkanRHITexture final : public RHITexture, public VulkanDeviceObject {
    friend VulkanRHIImpl;

public:
    VulkanRHITexture() = delete;
    ~VulkanRHITexture();

    explicit VulkanRHITexture(const RHITextureCreateInfo& _info, VulkanDevice* _device);

    //for inner usage only
    explicit VulkanRHITexture(const RHITextureCreateInfo& _info, VkImage _image, VulkanDevice* _device);

    inline const VmaAllocation GetAllocation() const {
        return m_alloc.alloc;
    }

    inline VkImage GetHandle() const {
        return m_alloc.image;
    }
    //for inner usage only
    inline void SetAttachedImageInner(VkImage _image) {
        m_alloc.image = _image;
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
    VulkanRHIFence(VulkanDevice* _device, EFenceUsageFlags _usage);
    virtual ~VulkanRHIFence();

    uint64_t GetValue() const override;

    void                    Wait(uint64_t value) override;
    inline VkSemaphore      GetSemaphoreHandle() { return m_timeline; }
    inline VkSemaphore      GetBinaryHandle() { return m_binary; }
    inline EFenceUsageFlags GetUsage() { return usage; }

private:
    VulkanDevice*    m_device;
    VkSemaphore      m_timeline;
    VkSemaphore      m_binary;
    EFenceUsageFlags usage;
};

#pragma endregion

#pragma region viewable resources view definitions

class VulkanRHITextureUAV final : public RHIUAV, public VulkanDeviceObject {
    friend VulkanRHIImpl;
    friend VulkanViewport;

public:
    virtual ~VulkanRHITextureUAV();
    explicit VulkanRHITextureUAV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIUAV(_resource, _viewInfo), VulkanDeviceObject(_device) {}

    inline VkImageView GetView() const { return m_view; }

private:
    VkImageView m_view;
};

class VulkanRHIBufferUAV final : public RHIUAV, public VulkanDeviceObject {
    friend VulkanRHIImpl;

public:
    virtual ~VulkanRHIBufferUAV();
    explicit VulkanRHIBufferUAV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIUAV(_resource, _viewInfo), VulkanDeviceObject(_device) {}

    inline VkBufferView GetView() const { return m_view; }

private:
    VkBufferView m_view;
};

class VulkanRHITextureSRV final : public RHISRV, public VulkanDeviceObject {
    friend VulkanRHIImpl;

public:
    virtual ~VulkanRHITextureSRV();
    explicit VulkanRHITextureSRV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHISRV(_resource, _viewInfo), VulkanDeviceObject(_device) {}
    inline VkImageView GetView() const { return m_view; }

private:
    VkImageView m_view = VK_NULL_HANDLE;
};

class VulkanRHIBufferSRV final : public RHISRV, public VulkanDeviceObject {
    friend VulkanRHIImpl;

public:
    virtual ~VulkanRHIBufferSRV();
    explicit VulkanRHIBufferSRV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHISRV(_resource, _viewInfo), VulkanDeviceObject(_device) {}

    inline VkBufferView GetView() const { return m_view; }

private:
    VkBufferView m_view = VK_NULL_HANDLE;
};

class VulkanImageView final : public RHIView {
    friend VulkanRHIImpl;

public:
    explicit VulkanImageView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIView(RRT_ATTACHMENT_VIEW, _resource, _viewInfo) {}

    explicit VulkanImageView(RHIViewableResource* _resource, VkImageView _view, const RHIViewInfo& _viewInfo) : RHIView(RRT_ATTACHMENT_VIEW, _resource, _viewInfo), m_view(_view) {
    }
    inline VkImageView GetView() const { return m_view; }

private:
    VkImageView m_view = VK_NULL_HANDLE;
};

#pragma endregion

#pragma region viewport

class VulkanViewport final : public RHIViewport {
public:
    VulkanViewport(class VulkanSwapChain* _swapchain, uint32_t _max_frame_in_flight);
    ~VulkanViewport();
    virtual void OnResize(Extent2D _size) override;

    virtual void    Present(RHIFence* _render_finished) override;
    VulkanRHIFence* GetAcquireNextImageFence();

    RHIViewportNextBackBufferInfo GetNextFrameBackBufferInfo() override;

    VulkanRHITextureUAV* GetCurrentBackBuffer(uint32_t index);

    virtual void WaitForQueueComplete(class RHICommandQueue* _command_queue, RHIFence* _optional_fence) override;

    virtual ViewPort GetViewportExtent() const override;

private:
    void InnerCreateResources();
    void InnerDestroyResources();
    void ResetResources();

    VulkanRHITextureUAV* InnerCreateVulkanUAV(VulkanDevice* _device, VulkanRHITexture* texture, const RHIViewInfo& _view_info);

    class VulkanSwapChain* swapchain;

    Moer::Array<VulkanRHIFence*> image_aquire_fences;

    Moer::Array<VulkanRHITextureUAV*> swapchain_image_uavs;

    Moer::Array<VulkanRHITexture*> swapchain_images;

    uint32_t frame_offset = 0;

    // uint32_t max_frame_in_flight = 3;
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
