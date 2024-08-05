//
// Created by 74535 on 2023/10/12.
//

#ifndef VULKAN_RHI_RESOURCE_H
#define VULKAN_RHI_RESOURCE_H

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/vulkan/misc/VulkanTypeDefs.h"

#include "misc/STL.h"

#include "shader/ShaderCommon.h"

#include <variant>
#include <vulkan/vulkan_core.h>

#include <vk_mem_alloc.h>
#include "VulkanSwapChain.h"
class VulkanRHIImpl;

namespace Moer::Render {
    class VulkanDevice;
    class VulkanPipelineResourceCache;

    class VulkanDescriptorSetsLayout;
}// namespace Moer::Render
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
class VulkanRHIRayTracingPipelineState;
class VulkanRHIRayTracingScene;
class VulkanRHIRayTracingAccelerationStructure;
class VulkanRHIRayTracingBLAS;
class VulkanRHIRayTracingTLAS;
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

namespace Moer::Render {
    class VulkanMemoryManager final {
    public:
        VulkanMemoryManager()                                      = delete;
        VulkanMemoryManager(const VulkanMemoryManager&)            = delete;
        VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;

        static VmaAllocationCreateFlags MEGenerateVmaMemoryFlags(EBufferUsageFlags _flags);
        static VmaMemoryUsage           MEGenerateVmaMemoryUsage();
    };

    static uint64 EncodeReflectInfo(uint _set, uint _binding, uint _stage_flags) {
        return uint64(_set << 16) | uint64(_binding) | (uint64(_stage_flags) << 32);
    }
    static uint64 EncodeReflectPushConstant(uint _offset, uint _size, uint _stage_flags) {
        return uint64(_offset << 16) | uint64(_size) | (uint64(_stage_flags) << 32);
    }
    static auto DecodeReflectPushConstant(uint64 _val) {
        return std::make_tuple(uint(_val >> 16), uint(_val & 0xFFFF), uint(_val >> 32));
    }
    static auto DecodeReflectInfo(uint64 _val) {
        return std::make_tuple(uint(_val >> 16), uint(_val & 0xFFFF), uint(_val >> 32));
    }

    class VulkanEnumTranslator final {
    public:
        static VkIndexType           METoVKIndexType(EIndexElementType _type);
        static VkFormat              METoVKFormat(EPixelFormat _format);
        static VkImageType           METoVKImageType(ETextureDimension _dim);
        static VkImageUsageFlags     METoVKImageUsageFlags(ETextureUsageFlags _me_flags);
        static EPixelFormat          VKToMEFormat(VkFormat _format);
        static VkBufferUsageFlags    METoVKBufferUsageFlags(EBufferUsageFlags _me_flags);
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

    class VulkanRHIRayGenShader : public RHIRayGenShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayGenShader(const Shader* _meta_shader) : RHIRayGenShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayMissShader : public RHIRayMissShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayMissShader(const Shader* _meta_shader) : RHIRayMissShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayClosestHitShader : public RHIRayClosestHitShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayClosestHitShader(const Shader* _meta_shader) : RHIRayClosestHitShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayCallableShader : public RHIRayCallableShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayCallableShader(const Shader* _meta_shader) : RHIRayCallableShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayIntersectionShader : public RHIRayIntersectionShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayIntersectionShader(const Shader* _meta_shader) : RHIRayIntersectionShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayAnyhitShader : public RHIRayAnyhitShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayAnyhitShader(const Shader* _meta_shader) : RHIRayAnyhitShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

#pragma endregion

#pragma region pipeline states definitions

    class VulkanDeviceObject {
    public:
        VulkanDeviceObject(VulkanDevice* _device = nullptr);

    protected:
        VulkanDevice* m_device;
    };
    class VulkanPipelineState : public VulkanDeviceObject {
        enum EType {
            GFX,
            Compute,
            RT
        };

    public:
        VulkanPipelineState(VulkanDevice* _device, EType _type = EType::GFX) : VulkanDeviceObject(_device), m_pipeline(VK_NULL_HANDLE), m_pipeline_layout(VK_NULL_HANDLE), m_pipeline_state_cache(nullptr){};
        virtual ~VulkanPipelineState();

        inline VkPipeline GetHandle() const {
            return m_pipeline;
        }

        inline const VkPipelineLayout GetPipelineLayout() const {
            return m_pipeline_layout;
        }

        inline VulkanPipelineResourceCache* GetPipelineResourceCache() const {
            return m_pipeline_state_cache;
        }

        inline const Moer::Render::VulkanDescriptorSetsLayout* GetDescriptorSetsLayout() const {
            return m_descriptor_sets_layout;
        }

        void                InitDescriptorSetLayouts(Moer::Array<Moer::Render::TDescriptorSetLayoutBindingArray>& _descriptor_bindings);
        void                InitPipelineResourceCache(const Moer::Array<Moer::Render::TDescriptorSetLayoutBindingArray>& _descriptor_bindings);
        void                CreatePipelineLayout(const VkPipelineLayoutCreateInfo& _pipeline_layout_ci);
        VkPipelineBindPoint GetPipelineBindPoint() {
            switch (m_type) {
                case GFX:
                    return VK_PIPELINE_BIND_POINT_GRAPHICS;
                case Compute:
                    return VK_PIPELINE_BIND_POINT_COMPUTE;
                case RT:
                    return VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
                default:
                    return VK_PIPELINE_BIND_POINT_GRAPHICS;
            }
        }

    protected:
        friend VulkanDevice;
        VkPipeline       m_pipeline;
        VkPipelineLayout m_pipeline_layout;
        // descriptor sets
        Moer::Render::VulkanDescriptorSetsLayout* m_descriptor_sets_layout;
        // resource cache
        VulkanPipelineResourceCache* m_pipeline_state_cache;

        EType m_type;
    };

    class VulkanRHIGraphicsPipelineState final : public RHIGfxPso, public VulkanPipelineState {
    public:
        VulkanRHIGraphicsPipelineState(VulkanDevice* _device)
            : RHIGfxPso(),
              VulkanPipelineState(_device) {}

        virtual ~VulkanRHIGraphicsPipelineState();

        static Moer::Array<VkPipelineShaderStageCreateInfo> METoVKShaderStageCreateInfo(const RHIShaderBoundStateInput& _shader_bound_state);
        static VkPipelineVertexInputStateCreateInfo         METoVKVertexInputStateCreateInfo(const RHIVertexInputInfo& _vertex_input_state);
        static Moer::Array<const Shader*>                   GetShaderInfoList(const RHIShaderBoundStateInput& _shader_bound_state);

        void CreateGraphicsPipeline(const VkGraphicsPipelineCreateInfo& _info);
    };

    class VulkanRHIComputePipelineState final : public RHIComputePso, public VulkanPipelineState {
    public:
        VulkanRHIComputePipelineState(VulkanDevice* _device)
            : RHIComputePso(),
              VulkanPipelineState(_device) {}

        void CreateComputePipeline(const VkComputePipelineCreateInfo& _info);
    };

    class VulkanRHIRayTracingPipelineState final : public RHIRTPso, public VulkanPipelineState {
        friend VulkanRHIImpl;

    public:
        VulkanRHIRayTracingPipelineState(VulkanDevice* _device)
            : RHIRTPso(),
              VulkanPipelineState(_device) {}

        const VkStridedDeviceAddressRegionKHR* GetRayGenSBT() { return &m_raygen_sbt; }
        const VkStridedDeviceAddressRegionKHR* GetRayMissSBT() { return &m_miss_sbt; }
        const VkStridedDeviceAddressRegionKHR* GetRayHitSBT() { return &m_hit_sbt; }
        const VkStridedDeviceAddressRegionKHR* GetRayCallableSBT() { return &m_callable_sbt; }

        void CreateRayTracingPipeline(const VkRayTracingPipelineCreateInfoKHR& _info);

    private:
        //SBT
        RHIBufferRef                    m_sbt_buffer;// MARK: should use RHIBufferRef instead of VkBuffer?
        VkStridedDeviceAddressRegionKHR m_raygen_sbt;
        VkStridedDeviceAddressRegionKHR m_miss_sbt;
        VkStridedDeviceAddressRegionKHR m_hit_sbt;
        VkStridedDeviceAddressRegionKHR m_callable_sbt;
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

    class VulkanBuffer : public Buffer, VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        struct BufferAlloc {
            VkBuffer      buffer;
            VmaAllocation alloc;
        };
        VulkanBuffer() = delete;
        virtual ~VulkanBuffer();
        VulkanBuffer(const BufferInfo& _info, VulkanDevice& _device);
        VulkanBuffer(const BufferInfo& _info, VulkanDevice& _device, VkBuffer _handle, VmaAllocation _alloc, bool _defer_destroy);
        inline const VmaAllocation GetAllocation() const {
            return m_alloc.alloc;
        }

        inline VkBuffer GetHandle() const {
            return m_alloc.buffer;
        }

        static VkIndexType        METoVKIndexType(EIndexElementType _type);
        static VkBufferUsageFlags METoVKBufferUsageFlags(VulkanDevice* _device, EBufferUsageFlags _me_flags);

    private:
        friend class TempBufferAllocator;
        BufferAlloc m_alloc;
    };

    class VulkanTexture final : public Texture, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        VulkanTexture() = delete;
        ~VulkanTexture();
        //for inner usage only
        explicit VulkanTexture(const TextureInfo& _info, VulkanDevice* _device, VkImage _image = VK_NULL_HANDLE);

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

        VkImageView GetView(uint _mip_level = 0, uint _mip_cnt = 1);
        bool        IsGeneralRead(uint _mip_level = 0) const;

    private:
        struct TextureAlloc {
            VkImage       image;
            VmaAllocation alloc;
        } m_alloc;
        UnorderedMap<uint, VkImageView>                                           m_views;
        Moer::UnorderedMap<Moer::uint, std::tuple<ETextureStateFlags, EPassType>> mip_usages;
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

    #define RESOURCE_CAST(RHIType, VkType) \
    inline VkType* ResourceCast(RHIType* _val) { return static_cast<VkType*>(_val); }

    class VulkanFence final : public Fence, VulkanDeviceObject {
    public:
        struct BinaryFence {
            VkSemaphore binary;
        };
        struct TimelineFence {
            VkSemaphore timeline;
        };

    public:
        VulkanFence(EFenceUsageFlags _usage, VulkanDevice&);
        virtual ~VulkanFence();

        uint64_t GetValue() const override;

        void                    Wait(uint64_t _value) override;
        auto&                   GetFence() { return m_fence; }
        VkSemaphore GetUnderlyingHandle() { return std::holds_alternative<BinaryFence>(m_fence) ? std::get<BinaryFence>(m_fence).binary : std::get<TimelineFence>(m_fence).timeline; }
        VkSemaphore GetBinaryHandle() { return std::get<BinaryFence>(m_fence).binary; }
        inline EFenceUsageFlags GetUsage() { return std::holds_alternative<BinaryFence>(m_fence) ? EFenceUsageFlags::BINARY : EFenceUsageFlags::TIMELINE; }
    private:
        std::variant<BinaryFence, TimelineFence> m_fence;
    };

#pragma endregion

#pragma region [ resource cast ]
    RESOURCE_CAST(Buffer, VulkanBuffer)
    RESOURCE_CAST(Texture, VulkanTexture)
    RESOURCE_CAST(Fence, VulkanFence)
    RESOURCE_CAST(Swapchain, VkSwapchain)
#pragma endregion

#pragma region viewable resources view definitions
    class VulkanRHIViewport;
    class VulkanRHITextureUAV final : public RHIUAV, public VulkanDeviceObject {
        friend VulkanRHIImpl;
        friend Moer::Render::VulkanRHIViewport;

    public:
        virtual ~VulkanRHITextureUAV();
        explicit VulkanRHITextureUAV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIUAV(_resource, _viewInfo), VulkanDeviceObject(_device) {}

        inline VkImageView GetView() const { return m_view; }

    private:
        VkImageView m_view;
    };

    class VulkanRHICBV final : public RHICBV, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        virtual ~VulkanRHICBV();
        explicit VulkanRHICBV(
            VulkanDevice*      _device,
            RHIBuffer*         _resource,
            const RHIViewInfo& _viewInfo) : RHICBV(_resource, _viewInfo), VulkanDeviceObject(_device) {}
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

    class VulkanRHIAccelerationStructureSRV final : public RHISRV, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        virtual ~VulkanRHIAccelerationStructureSRV();
        explicit VulkanRHIAccelerationStructureSRV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewinfo) : RHISRV(_resource, _viewinfo), VulkanDeviceObject(_device){};
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

    class VulkanRHIViewport final : public RHIViewport {
        friend class VkCommandQueue;
    public:
        VulkanRHIViewport(class VulkanSwapChain* _swapchain, uint32_t _max_frame_in_flight);
        ~VulkanRHIViewport();
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
        VulkanTexture* GetSwapchainImage(uint32_t _index);
        VulkanFence*  GetBinaryFence(uint32_t _index);
        void Present(VulkanFence::BinaryFence _fence);
        VulkanRHITextureUAV* InnerCreateVulkanUAV(VulkanDevice* _device, VulkanRHITexture* texture, const RHIViewInfo& _view_info);

        class VulkanSwapChain* swapchain;

        Moer::Array<VulkanRHIFence*> image_aquire_fences;

        Moer::Array<VulkanRHITextureUAV*> swapchain_image_uavs;

        Moer::Array<VulkanRHITexture*> swapchain_images;
        //new resources
        Moer::Array<VulkanTexture*> swapchain_textures;
        Moer::Array<VulkanFence*>   frame_fences;

        uint32_t frame_offset = 0;

        // uint32_t max_frame_in_flight = 3;
    };

    class VulkanViewport final : public Viewport, VulkanDeviceObject {
    public:
        VulkanViewport(RHIViewportInitializer _init_info, VulkanDevice& _device);
        // ~VulkanViewport();
        void           Resize(Extent2D _size) override;
        void           Present(FenceRef) override;
        // BackBufferInfo GetBackBuffer() override;
        void*          GetNativeWindow() override;

    private:
        VulkanSwapChain m_swap_chain;
        void            CreateResources();

        Array<VulkanTexture*> m_back_buffers;
        uint32_t              frame_offset = 0;
        Array<VulkanFence*>   m_frame_fences;
    };
#pragma endregion

#pragma region acceleration structure definitions
    class VulkanRHIRayTracingAccelerationStructure {
    public:
        static VkGeometryTypeKHR                    METoVKGeometryTypeKHR(ERayTracingGeometryType _type);
        static VkGeometryFlagsKHR                   METoGeometryFlagsKHR(ERayTracingGeometryFlags _flag);
        static VkBuildAccelerationStructureFlagsKHR METoVKBuildAccelerationStructureFlagsKHR(ERayTracingAccelerationStructureBuildFlags _me_flags);
        static VkGeometryInstanceFlagsKHR           METoVKGeometryInstanceFlagsKHR(ERayTracingInstanceFlags _me_flags);
    };

    class VulkanRHIRayTracingBLAS final : public RHIRayTracingBLAS, public VulkanRHIRayTracingAccelerationStructure {
        friend VulkanRHIImpl;

    public:
        VulkanRHIRayTracingBLAS(const RHIRayTracingBLASInitializer& _init) : RHIRayTracingBLAS(_init) {
        }

    protected:
        VkAccelerationStructureKHR m_blas;
        RHIBufferRef               m_buffer;
    };
    class VulkanRHIRayTracingTLAS final : public RHIRayTracingTLAS, public VulkanRHIRayTracingAccelerationStructure {
        friend VulkanRHIImpl;
        friend VulkanPipelineResourceCache;

    public:
        VulkanRHIRayTracingTLAS(const RHIRayTracingTLASInitializer& _init) : RHIRayTracingTLAS(_init) {
        }

    protected:
        VkAccelerationStructureKHR m_tlas;
        RHIBufferRef               m_buffer;
    };
}// namespace Moer::Render
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
