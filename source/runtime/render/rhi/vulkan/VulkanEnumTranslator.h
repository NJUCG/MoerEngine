#pragma once

#include "VulkanCommon.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {

struct VulkanPixelFormatInfo {
    uint     stride; // Bytes per texel for linear formats, per block for compressed formats.
    VkFormat format;
};

class VulkanEnumTranslator final {
public:
    static const VulkanPixelFormatInfo& GetPixelFormatInfo(EPixelFormat _format);
    static VkIndexType                  METoVKIndexType(EIndexElementType _type);
    static VkFormat                     METoVKFormat(EPixelFormat _format);
    static VkImageCreateFlags           METoVKImageCreateFlags(ETextureDimension _dim);
    static VkImageType                  METoVKImageType(ETextureDimension _dim);
    static VkImageUsageFlags            METoVKImageUsageFlags(ETextureUsageFlags _me_flags);
    static EPixelFormat                 VKToMEFormat(VkFormat _format);
    static VkBufferUsageFlags           METoVKBufferUsageFlags(EBufferUsageFlags _me_flags);
    static VkDescriptorType             METoVkBufferDescriptorType(EBufferUsageFlags _type);
    static VkSampleCountFlagBits        METoVKSampleCountFlagBits(uint32_t _me_count);
    static VkImageAspectFlags           METoVKImageAspectFlags(ETextureAspectFlags _flags);
    static VkImageViewType              METoVKImageViewType(ETextureDimension _dim);
    static VkImageLayout                METoVKImageLayout(ETextureLayout _layout);
    static VkAttachmentLoadOp           METoVKAttachmentLoadOp(EAttachmentLoadOp _load_op);
    static VkAttachmentStoreOp          METoVKAttachmentStoreOp(EAttachmentStoreOp _store_op);
    static VkFilter                     METoVKImageFilter(ESamplerFilter _filter);

    static VkPipelineStageFlags2 METoVkPipelineStageFlags2(ERHIPipelineStageFlags _flags);
    static VkAccessFlags2        METoVkAccessFlags2(ERHIAccessFlags _flags);

    static VkCullModeFlags     METoVKCullModeFlags(ERasterizerCullMode _cull_mode);
    static VkPrimitiveTopology METoVKPrimitiveTopology(EPrimitiveTopology _primitive_type);
    static VkPolygonMode       METoVKPolygonMode(ERasterizerFillMode _fill_mode);

    static VkDescriptorType
    METoVKDescriptorType(EShaderParameterType _type, EShaderCodeResourceBindingType _binding_type);
    static VkShaderStageFlags METoVKShaderStageFlags(EShaderType _type);

    static VkFilter             METoVKMinMagFilterMode(ESamplerFilter _filter);
    static VkSamplerMipmapMode  METoVKMipmapMode(ESamplerFilter _filter);
    static VkSamplerAddressMode METoVKWrapMode(ESamplerAddressMode _address_mode);

    static VkCompareOp METoVKCompareOp(ECompareOption _compare_op);
    static VkStencilOp METoVKStencilOp(EStencilOp _stencil_op);

    static VkBlendOp     METoVKBlendOp(EBlendOperation _blend_op);
    static VkBlendFactor METoVKBlendFactor(EBlendFactor _blend_factor);

    static VkVertexInputRate METoVKVertexInputRate(EVertexInputRate _me_rate);

    //Raytracing
    static VkGeometryTypeKHR  METoVKGeometryType(ERayTracingGeometryType _type);
    static VkGeometryFlagsKHR METoVKGeometryFlags(ERayTracingGeometryFlags _flags);
    static VkBuildAccelerationStructureFlagsKHR
    METoVKAccelerationStructureBuildType(ERayTracingAccelerationStructureBuildFlags _type);
    static VkBuildAccelerationStructureModeKHR
    METoVKBuildAccelerationStructureMode(ERaytracingBuildMode _mode);
};

} // namespace Moer::Render
