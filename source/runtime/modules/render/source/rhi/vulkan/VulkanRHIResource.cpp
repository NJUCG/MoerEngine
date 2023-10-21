//
// Created by 74535 on 2023/10/12.
//

#include "VulkanRHIResource.h"

#include "VulkanDevice.h"

#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "log/LogSystem.h"

#pragma region utils definition

VmaMemoryUsage VulkanMemoryManager::MEGenerateVmaMemoryUsage() {
    return VMA_MEMORY_USAGE_AUTO;
}

VkSampleCountFlagBits VulkanEnumTranslator::METoVKSampleCountFlagBits(uint32_t _me_count) {
    switch (_me_count) {
        case 1:
            return VK_SAMPLE_COUNT_1_BIT;
        case 2:
            return VK_SAMPLE_COUNT_2_BIT;
        case 4:
            return VK_SAMPLE_COUNT_4_BIT;
        case 8:
            return VK_SAMPLE_COUNT_8_BIT;
        case 16:
            return VK_SAMPLE_COUNT_16_BIT;
        case 32:
            return VK_SAMPLE_COUNT_32_BIT;
        case 64:
            return VK_SAMPLE_COUNT_64_BIT;
        default:
            LOG_CRITICAL("Unsupported multisample count: {}", static_cast<uint32_t>(_me_count));
            return VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM;
    }
}

VkImageViewType VulkanEnumTranslator::METoVKImageViewType(ETextureDimension _dim) {
    switch (_dim) {
        case ETextureDimension::TEX_2D:
            return VK_IMAGE_VIEW_TYPE_2D;
        case ETextureDimension::TEX_2D_ARRAY:
            return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case ETextureDimension::TEX_3D:
            return VK_IMAGE_VIEW_TYPE_3D;
        case ETextureDimension::TEX_CUBE:
            return VK_IMAGE_VIEW_TYPE_CUBE;
        case ETextureDimension::TEX_CUBE_ARRAY:
            return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        default:
            LOG_CRITICAL("Unsupported SRV dimension type: {}", static_cast<uint32_t>(_dim));
            return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    }
}

#pragma endregion

void VulkanRHISampler::GenerateSamplerFromInitializer(const VulkanDevice* _device, const RHISamplerInitializer& _initializer) {
    VkSamplerCreateInfo sampler_create_info{};

    sampler_create_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_create_info.flags        = 0;
    sampler_create_info.magFilter    = METoVKMinMagFilterMode(_initializer.filter);
    sampler_create_info.minFilter    = METoVKMinMagFilterMode(_initializer.filter);
    sampler_create_info.mipmapMode   = METoVKMipmapMode(_initializer.filter);
    sampler_create_info.addressModeU = METoVKWrapMode(_initializer.address_mode_u);
    sampler_create_info.addressModeV = METoVKWrapMode(_initializer.address_mode_v);
    sampler_create_info.addressModeW = METoVKWrapMode(_initializer.address_mode_w);
    sampler_create_info.mipLodBias   = _initializer.mip_lod_bias;

    sampler_create_info.maxAnisotropy = 1.0f;
    if (_initializer.filter == SF_ANISOTROPIC_NEAREST || _initializer.filter == SF_ANISOTROPIC_LINEAR) {
        sampler_create_info.maxAnisotropy = std::clamp(static_cast<float>(_initializer.max_anisotropy), 1.0f, _device->GetProperties().limits.maxSamplerAnisotropy);
    }
    sampler_create_info.anisotropyEnable = sampler_create_info.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;

    sampler_create_info.compareEnable = _initializer.compare_op != SCF_NEVER ? VK_TRUE : VK_FALSE;
    sampler_create_info.compareOp     = METoVKCompareOp(_initializer.compare_op);
    sampler_create_info.minLod        = _initializer.min_mip_level;
    sampler_create_info.maxLod        = _initializer.max_mip_level;
    sampler_create_info.borderColor   = _initializer.border_color == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

    VK_CHECK_RESULT(vkCreateSampler(*_device, &sampler_create_info, nullptr, &m_sampler));
}

VkFilter VulkanRHISampler::METoVKMinMagFilterMode(ESamplerFilter _filter) {
    switch (_filter) {
        case SF_NEAREST:
            return VK_FILTER_NEAREST;
        case SF_LINEAR:
        case SF_CUBIC:
            return VK_FILTER_LINEAR;
        case SF_ANISOTROPIC_NEAREST:
        case SF_ANISOTROPIC_LINEAR:
            return VK_FILTER_LINEAR;
        default:
            LOG_CRITICAL("Unknown ESamplerFilter {:d}", static_cast<uint8_t>(_filter));
            return VK_FILTER_MAX_ENUM;
    }
}

VkSamplerMipmapMode VulkanRHISampler::METoVKMipmapMode(ESamplerFilter _filter) {
    switch (_filter) {
        case SF_NEAREST:
        case SF_LINEAR:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case SF_CUBIC:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        case SF_ANISOTROPIC_NEAREST:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case SF_ANISOTROPIC_LINEAR:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        default:
            LOG_CRITICAL("Unknown Mipmap ESamplerFilter {:d}", static_cast<uint8_t>(_filter));
            return VK_SAMPLER_MIPMAP_MODE_MAX_ENUM;
    }
}

VkSamplerAddressMode VulkanRHISampler::METoVKWrapMode(ESamplerAddressMode _address_mode) {
    switch (_address_mode) {
        case SAM_REPEAT:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SAM_MIRRORED_REPEAT:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SAM_CLAMP_TO_EDGE:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SAM_CLAMP_TO_BORDER:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
            LOG_CRITICAL("Unknown ESamplerAddressMode {:d}", static_cast<uint8_t>(_address_mode));
            return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
    }
}

VkCompareOp VulkanRHISampler::METoVKCompareOp(ESamplerCompareFunction _compare_op) {
    switch (_compare_op) {
        case SCF_NEVER:
            return VK_COMPARE_OP_NEVER;
        case SCF_LESS:
            return VK_COMPARE_OP_LESS;
        case SCF_EQUAL:
            return VK_COMPARE_OP_EQUAL;
        case SCF_LESS_OR_EQUAL:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case SCF_GREATER:
            return VK_COMPARE_OP_GREATER;
        case SCF_NOT_EQUAL:
            return VK_COMPARE_OP_NOT_EQUAL;
        case SCF_GREATER_OR_EQUAL:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case SCF_ALWAYS:
            return VK_COMPARE_OP_ALWAYS;
        default:
            LOG_CRITICAL("Unknown ESamplerCompareFunction {:d}", static_cast<uint8_t>(_compare_op));
            return VK_COMPARE_OP_MAX_ENUM;
    }
}

void VulkanRHIVertexInputState::GenerateVertexInputStateFromInitializer(const VertexInputStateInitializerList& _init) {
    for (uint32_t i = 0; i < MAX_VERTEX_ELEMENT_COUNT; ++i) {
        if (_init[i].type == EVertexElementType::VET_None) {
            break;
        }
        m_bindings[i].binding    = _init[i].binding_index;
        m_bindings[i].stride     = _init[i].stride;
        m_bindings[i].inputRate  = METoVKVertexInputRate(_init[i].input_rate);
        m_attributes[i].location = _init[i].attribute_index;
        m_attributes[i].binding  = _init[i].binding_index;
        m_attributes[i].format   = METoVKFormat(_init[i].type);
        m_attributes[i].offset   = _init[i].offset;

        m_binding_count = _init[i].binding_index;
        ++m_attribute_count;
    }

    m_input_state_create_info.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    m_input_state_create_info.pNext                           = nullptr;
    m_input_state_create_info.flags                           = 0;
    m_input_state_create_info.vertexBindingDescriptionCount   = m_binding_count;
    m_input_state_create_info.pVertexBindingDescriptions      = m_bindings.data();
    m_input_state_create_info.vertexAttributeDescriptionCount = m_attribute_count;
    m_input_state_create_info.pVertexAttributeDescriptions    = m_attributes.data();
}

VkVertexInputRate VulkanRHIVertexInputState::METoVKVertexInputRate(EVertexInputRate _me_rate) {
    switch (_me_rate) {
        case EVertexInputRate::VIR_VERTEX:
            return VK_VERTEX_INPUT_RATE_VERTEX;
        case EVertexInputRate::VIR_INSTANCE:
            return VK_VERTEX_INPUT_RATE_INSTANCE;
        default:
            LOG_CRITICAL("Unsupported vertex input rate: {}", static_cast<uint32_t>(_me_rate));
            return VK_VERTEX_INPUT_RATE_MAX_ENUM;
    }
}

VkFormat VulkanRHIVertexInputState::METoVKFormat(EVertexElementType _me_format) {
    switch (_me_format) {
        case EVertexElementType::VET_None:
            return VK_FORMAT_UNDEFINED;
        case EVertexElementType::VET_Float1:
            return VK_FORMAT_R32_SFLOAT;
        case EVertexElementType::VET_Float2:
            return VK_FORMAT_R32G32_SFLOAT;
        case EVertexElementType::VET_Float3:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case EVertexElementType::VET_Float4:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case EVertexElementType::VET_PackedNormal:
            return VK_FORMAT_R8G8B8A8_SNORM;
        case EVertexElementType::VET_UByte4:
            return VK_FORMAT_R8G8B8A8_UINT;
        case EVertexElementType::VET_UByte4N:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case EVertexElementType::VET_Color:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case EVertexElementType::VET_Short2:
            return VK_FORMAT_R16G16_SINT;
        case EVertexElementType::VET_Short4:
            return VK_FORMAT_R16G16B16A16_SINT;
        case EVertexElementType::VET_Short2N:
            return VK_FORMAT_R16G16_SNORM;
        case EVertexElementType::VET_Half2:
            return VK_FORMAT_R16G16_SFLOAT;
        case EVertexElementType::VET_Half4:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case EVertexElementType::VET_Short4N:// 4 X 16 bit word: normalized
            return VK_FORMAT_R16G16B16A16_SNORM;
        case EVertexElementType::VET_UShort2:
            return VK_FORMAT_R16G16_UINT;
        case EVertexElementType::VET_UShort4:
            return VK_FORMAT_R16G16B16A16_UINT;
        case EVertexElementType::VET_UShort2N:// 16 bit word normalized to (value/65535.0:value/65535.0:0:0:1)
            return VK_FORMAT_R16G16_UNORM;
        case EVertexElementType::VET_UShort4N:// 4 X 16 bit word unsigned: normalized
            return VK_FORMAT_R16G16B16A16_UNORM;
        case EVertexElementType::VET_URGB10A2N:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case EVertexElementType::VET_UInt:
            return VK_FORMAT_R32_UINT;
        default:
            LOG_CRITICAL("Unsupported vertex element type: {}", static_cast<uint32_t>(_me_format));
            return VK_FORMAT_MAX_ENUM;
    }
}

void VulkanRHIRasterizationState::GenerateRasterizationStateFromInitializer(const RHIRasterizationStateInitializer& _init) {
    m_rasterization_state_create_info.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    m_rasterization_state_create_info.pNext                   = nullptr;
    m_rasterization_state_create_info.flags                   = 0;
    m_rasterization_state_create_info.depthClampEnable        = _init.b_depth_clamp_enable ? VK_TRUE : VK_FALSE;
    m_rasterization_state_create_info.rasterizerDiscardEnable = VK_FALSE;// MARK...
    m_rasterization_state_create_info.polygonMode             = METoVKPolygonMode(_init.fill_mode);
    m_rasterization_state_create_info.cullMode                = METoVKCullModeFlags(_init.cull_mode);
    m_rasterization_state_create_info.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;// MARK...
    m_rasterization_state_create_info.depthBiasEnable         = _init.b_depth_bias ? VK_TRUE : VK_FALSE;
    m_rasterization_state_create_info.depthBiasConstantFactor = _init.depth_bias;
    m_rasterization_state_create_info.depthBiasClamp          = _init.depth_bias_clamp;
    m_rasterization_state_create_info.depthBiasSlopeFactor    = _init.depth_bias_slop_factor;
    m_rasterization_state_create_info.lineWidth               = 1.0f;
}

VkPolygonMode VulkanRHIRasterizationState::METoVKPolygonMode(ERasterizerFillMode _fill_mode) {
    switch (_fill_mode) {
        case ERasterizerFillMode::FM_FILL:
            return VK_POLYGON_MODE_FILL;
        case ERasterizerFillMode::FM_LINE:
            return VK_POLYGON_MODE_LINE;
        case ERasterizerFillMode::FM_POINT:
            return VK_POLYGON_MODE_POINT;
        case ERasterizerFillMode::FM_FILL_RECTANGLE_NV:
            return VK_POLYGON_MODE_FILL_RECTANGLE_NV;
        default:
            LOG_CRITICAL("Unsupported rasterizer fill mode: {}", static_cast<uint32_t>(_fill_mode));
            return VK_POLYGON_MODE_MAX_ENUM;
    }
}

VkCullModeFlags VulkanRHIRasterizationState::METoVKCullModeFlags(ERasterizerCullMode _cull_mode) {
    switch (_cull_mode) {
        case ERasterizerCullMode::CM_UNDEFINED:
            return VK_CULL_MODE_NONE;
        case ERasterizerCullMode::CM_FRONT:
            return VK_CULL_MODE_FRONT_BIT;
        case ERasterizerCullMode::CM_BACK:
            return VK_CULL_MODE_BACK_BIT;
        case ERasterizerCullMode::CM_FRONT_AND_BACK:
            return VK_CULL_MODE_FRONT_AND_BACK;
        default:
            LOG_CRITICAL("Unsupported rasterizer cull mode: {}", static_cast<uint32_t>(_cull_mode));
            return VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
    }
}

void VulkanRHIDepthStencilState::GenerateDepthStencilStateFromInitializer(const RHIDepthStencilStateInitializer& _init) {
    m_depth_stencil_state_create_info.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    m_depth_stencil_state_create_info.pNext                 = nullptr;
    m_depth_stencil_state_create_info.flags                 = 0;
    m_depth_stencil_state_create_info.depthTestEnable       = (_init.b_enable_depth_write || _init.depth_test_op != ECompareOption::CO_ALWAYS) ? VK_TRUE : VK_FALSE;
    m_depth_stencil_state_create_info.depthWriteEnable      = _init.b_enable_depth_write;
    m_depth_stencil_state_create_info.depthCompareOp        = VulkanRHIDepthStencilState::METoVKCompareOp(_init.depth_test_op);
    m_depth_stencil_state_create_info.depthBoundsTestEnable = VK_FALSE;// MARK...
    m_depth_stencil_state_create_info.minDepthBounds        = 0.0f;
    m_depth_stencil_state_create_info.maxDepthBounds        = 1.0f;

    m_depth_stencil_state_create_info.stencilTestEnable = (_init.b_enable_front_face_stencil || _init.b_enable_back_face_stencil) ? VK_TRUE : VK_FALSE;
    m_depth_stencil_state_create_info.front.failOp      = METoVKStencilOp(_init.front_face_stencil_fail_stencilOp);
    m_depth_stencil_state_create_info.front.passOp      = METoVKStencilOp(_init.front_face_pass_stencil_op);
    m_depth_stencil_state_create_info.front.depthFailOp = METoVKStencilOp(_init.front_face_depth_fail_stencilOp);
    m_depth_stencil_state_create_info.front.compareOp   = METoVKCompareOp(_init.front_face_stencil_test);
    m_depth_stencil_state_create_info.front.compareMask = _init.stencil_readmask;
    m_depth_stencil_state_create_info.front.writeMask   = _init.stencil_writemask;
    m_depth_stencil_state_create_info.front.reference   = 0;

    if (_init.b_enable_back_face_stencil) {
        m_depth_stencil_state_create_info.back.failOp      = METoVKStencilOp(_init.back_face_stencil_fail_stencil_op);
        m_depth_stencil_state_create_info.back.passOp      = METoVKStencilOp(_init.back_face_pass_stencil_op);
        m_depth_stencil_state_create_info.back.depthFailOp = METoVKStencilOp(_init.back_face_depth_fail_stencil_op);
        m_depth_stencil_state_create_info.back.compareOp   = METoVKCompareOp(_init.back_face_stencil_test);
        m_depth_stencil_state_create_info.back.compareMask = _init.stencil_readmask;
        m_depth_stencil_state_create_info.back.writeMask   = _init.stencil_writemask;
        m_depth_stencil_state_create_info.back.reference   = 0;
    } else {
        m_depth_stencil_state_create_info.front = m_depth_stencil_state_create_info.back;
    }
}

VkCompareOp VulkanRHIDepthStencilState::METoVKCompareOp(ECompareOption _compare_op) {
    switch (_compare_op) {
        case ECompareOption::CO_NEVER:
            return VK_COMPARE_OP_NEVER;
        case ECompareOption::CO_LESS:
            return VK_COMPARE_OP_LESS;
        case ECompareOption::CO_EQUAL:
            return VK_COMPARE_OP_EQUAL;
        case ECompareOption::CO_LESS_OR_EQUAL:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case ECompareOption::CO_GREATER:
            return VK_COMPARE_OP_GREATER;
        case ECompareOption::CO_NOT_EQUAL:
            return VK_COMPARE_OP_NOT_EQUAL;
        case ECompareOption::CO_GREATER_OR_EQUAL:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case ECompareOption::CO_ALWAYS:
            return VK_COMPARE_OP_ALWAYS;
        default:
            LOG_CRITICAL("Unsupported depth stencil compare option: {}", static_cast<uint32_t>(_compare_op));
            return VK_COMPARE_OP_MAX_ENUM;
    }
}

VkStencilOp VulkanRHIDepthStencilState::METoVKStencilOp(EStencilOp _stencil_op) {
    switch (_stencil_op) {
        case SO_KEEP:
            return VK_STENCIL_OP_KEEP;
        case SO_ZERO:
            return VK_STENCIL_OP_ZERO;
        case SO_REPLACE:
            return VK_STENCIL_OP_REPLACE;
        case SO_INCREMENT_AND_CLAMP:
            return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case SO_DECREMENT_AND_CLAMP:
            return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case SO_INVERT:
            return VK_STENCIL_OP_INVERT;
        case SO_INCREMENT_AND_WRAP:
            return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case SO_DECREMENT_AND_WRAP:
            return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default:
            LOG_CRITICAL("Unsupported depth stencil operation: {}", static_cast<uint32_t>(_stencil_op));
            return VK_STENCIL_OP_MAX_ENUM;
    };
}

void VulkanRHIMultisampleState::GenerateMultisampleStateFromInitializer(const RHIMultisampleStateInitializer& _init) {
    m_multisample_state_create_info.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    m_multisample_state_create_info.pNext                 = nullptr;
    m_multisample_state_create_info.flags                 = 0;
    m_multisample_state_create_info.rasterizationSamples  = VulkanEnumTranslator::METoVKSampleCountFlagBits(_init.sample_count);
    m_multisample_state_create_info.sampleShadingEnable   = _init.b_sample_shading;
    m_multisample_state_create_info.minSampleShading      = _init.min_sample_shading;
    m_multisample_state_create_info.pSampleMask           = nullptr;// MARK...
    m_multisample_state_create_info.alphaToCoverageEnable = _init.b_alpha_to_converge;
    m_multisample_state_create_info.alphaToOneEnable      = _init.b_alpha_to_one;
}

void VulkanRHIBlendState::GenerateBlendStateFromInitializer(const RHIBlendStateInitializer& _init) {
    const auto n = 1;// MARK...

    std::vector<VkPipelineColorBlendAttachmentState> attachments(n);
    for (size_t i = 0; i < n; ++i) {
        auto& attachment_init = _init.attachments[i];
        auto& attachment      = attachments[i];

        attachment.blendEnable =
            (attachment_init.color_blend_op != BO_ADD || attachment_init.color_dst_blend_factor != BF_ZERO || attachment_init.color_src_blend_factor != BF_ONE ||
             attachment_init.alpha_blend_op != BO_ADD || attachment_init.alpha_dst_blend_factor != BF_ZERO || attachment_init.alpha_src_blend_factor != BF_ONE) ?
                VK_TRUE :
                VK_FALSE;
        attachment.srcColorBlendFactor = VulkanRHIBlendState::METoVKBlendFactor(attachment_init.color_src_blend_factor);
        attachment.dstColorBlendFactor = VulkanRHIBlendState::METoVKBlendFactor(attachment_init.color_dst_blend_factor);
        attachment.colorBlendOp        = VulkanRHIBlendState::METoVKBlendOp(attachment_init.color_blend_op);
        attachment.srcAlphaBlendFactor = VulkanRHIBlendState::METoVKBlendFactor(attachment_init.alpha_src_blend_factor);
        attachment.dstAlphaBlendFactor = VulkanRHIBlendState::METoVKBlendFactor(attachment_init.alpha_dst_blend_factor);
        attachment.alphaBlendOp        = VulkanRHIBlendState::METoVKBlendOp(attachment_init.alpha_blend_op);
        attachment.colorWriteMask      = (attachment_init.color_write_mask & CW_RED) ? VK_COLOR_COMPONENT_R_BIT : 0;
        attachment.colorWriteMask |= (attachment_init.color_write_mask & CW_GREEN) ? VK_COLOR_COMPONENT_G_BIT : 0;
        attachment.colorWriteMask |= (attachment_init.color_write_mask & CW_BLUE) ? VK_COLOR_COMPONENT_B_BIT : 0;
        attachment.colorWriteMask |= (attachment_init.color_write_mask & CW_ALPHA) ? VK_COLOR_COMPONENT_A_BIT : 0;
    }

    VkPipelineColorBlendStateCreateInfo blend_state_create_info{};
    blend_state_create_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state_create_info.pNext           = nullptr;
    blend_state_create_info.flags           = 0;
    blend_state_create_info.logicOpEnable   = VK_FALSE;
    blend_state_create_info.logicOp         = VK_LOGIC_OP_COPY;
    blend_state_create_info.attachmentCount = n;
    blend_state_create_info.pAttachments    = attachments.data();
}

VkBlendOp VulkanRHIBlendState::METoVKBlendOp(EBlendOperation _blend_op) {
    switch (_blend_op) {
        case BO_ADD:
            return VK_BLEND_OP_ADD;
        case BO_SUBTRACT:
            return VK_BLEND_OP_SUBTRACT;
        case BO_REVERSE_SUBTRACT:
            return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BO_MIN:
            return VK_BLEND_OP_MIN;
        case BO_MAX:
            return VK_BLEND_OP_MAX;
        default:
            LOG_CRITICAL("Unsupported color blend operation: {}", static_cast<uint32_t>(_blend_op));
            return VK_BLEND_OP_MAX_ENUM;
    }
}

VkBlendFactor VulkanRHIBlendState::METoVKBlendFactor(EBlendFactor _blend_factor) {
    switch (_blend_factor) {
        case BF_ZERO:
            return VK_BLEND_FACTOR_ZERO;
        case BF_ONE:
            return VK_BLEND_FACTOR_ONE;
        case BF_SRC_COLOR:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case BF_ONE_MINUS_SRC_COLOR:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BF_DST_COLOR:
            return VK_BLEND_FACTOR_DST_COLOR;
        case BF_ONE_MINUS_DST_COLOR:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BF_SRC_ALPHA:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case BF_ONE_MINUS_SRC_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BF_DST_ALPHA:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case BF_ONE_MINUS_DST_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BF_CONSTANT_ALPHA:
            return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case BF_ONE_MINUS_CONSTANT_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case BF_SRC1_COLOR:
            return VK_BLEND_FACTOR_SRC1_COLOR;
        case BF_ONE_MINUS_SRC1_COLOR:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case BF_SRC1_ALPHA:
            return VK_BLEND_FACTOR_SRC1_ALPHA;
        case BF_ONE_MINUS_SRC1_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        default:
            LOG_CRITICAL("Unsupported color blend factor: {}", static_cast<uint32_t>(_blend_factor));
            return VK_BLEND_FACTOR_MAX_ENUM;
    }
}

#pragma region shader definitions
#pragma endregion

#pragma region pipeline states definitions
#pragma endregion

#pragma region global buffer definitions
#pragma endregion

#pragma region viewable resources definitions

VkBufferUsageFlags VulkanRHIBuffer::METoVKBufferUsageFlags(VulkanDevice* _device, EBufferUsageFlags _me_flags) {
    // Always include TRANSFER_SRC since hardware vendors confirmed it wouldn't have any performance cost and we need it for some debug functionalities.
    VkBufferUsageFlags vk_flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    auto TranslateFlag = [&vk_flags, &_me_flags](EBufferUsageFlags _search_me_flags, VkBufferUsageFlags _added_if_found, VkBufferUsageFlags _added_if_not_found = 0) {
        const bool has_flag = (_me_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    TranslateFlag(EBufferUsageFlags::VERTEX_BUFFER, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::INDEX_BUFFER, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::STRUCTURED_BUFFER, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

#if VULKAN_RHI_RAYTRACING
    TranslateFlag(EBufferUsageFlags::ACCELERATION_STRUCTURE, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
#endif

    TranslateFlag(EBufferUsageFlags::UNORDERED_ACCESS, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::INDIRECT_BUFFER, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::CPU_VISIBLE, (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    TranslateFlag(EBufferUsageFlags::SHADER_RESOURCE, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);

    TranslateFlag(EBufferUsageFlags::LIFE_CYCLE_ONE_FRAME, 0, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

#if VULKAN_RHI_RAYTRACING
    if (_device->GetGpuExtensions().HasRaytracingExtensions()) {
        vk_flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        TranslateFlag(EBufferUsageFlags::ACCELERATION_STRUCTURE, 0, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    }
#endif
    // For descriptors buffers
    // if (_device->GetOptionalExtensions().HasBufferDeviceAddress) {
    //     OutVkUsage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    // }

    return vk_flags;
}

VkImageType VulkanRHITexture::METoVKImageType(ETextureDimension _dim) {
    switch (_dim) {
        case ETextureDimension::TEX_2D:
        case ETextureDimension::TEX_2D_ARRAY:
        case ETextureDimension::TEX_CUBE:
        case ETextureDimension::TEX_CUBE_ARRAY:
            return VK_IMAGE_TYPE_2D;
        case ETextureDimension::TEX_3D:
            return VK_IMAGE_TYPE_3D;
        default:
            LOG_CRITICAL("Unsupported texture dimension: {}", static_cast<uint32_t>(_dim));
            return VK_IMAGE_TYPE_MAX_ENUM;
    }
}

VkImageUsageFlags VulkanRHITexture::METoVKImageUsageFlags(ETextureUsageFlags _me_flags) {
    // Always include TRANSFER_SRC since hardware vendors confirmed it wouldn't have any performance cost and we need it for some debug functionalities.
    VkImageUsageFlags vk_flags = 0;

    auto TranslateFlag = [&vk_flags, &_me_flags](ETextureUsageFlags _search_me_flags, VkImageUsageFlags _added_if_found, VkImageUsageFlags _added_if_not_found = 0) {
        const bool has_flag = (_me_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    TranslateFlag(ETextureUsageFlags::TRANSFER_SRC, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    TranslateFlag(ETextureUsageFlags::TRANSFER_DST, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    TranslateFlag(ETextureUsageFlags::SAMPLED, VK_IMAGE_USAGE_SAMPLED_BIT);
    TranslateFlag(ETextureUsageFlags::SHADER_RESOURCE, VK_IMAGE_USAGE_STORAGE_BIT);
    TranslateFlag(ETextureUsageFlags::COLOR_ATTACHMENT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    TranslateFlag(ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    TranslateFlag(ETextureUsageFlags::TRANSIENT_ATTACHMENT, VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
    TranslateFlag(ETextureUsageFlags::INPUT_ATTACHMENT, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    TranslateFlag(ETextureUsageFlags::VIDEO_DECODE, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR);
#endif
    TranslateFlag(ETextureUsageFlags::FRAGMENT_DENSITY_MAP, VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT);
    TranslateFlag(ETextureUsageFlags::FRAGMENT_SHADING_RATE_ATTACHMENT, VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
#ifdef VK_ENABLE_BETA_EXTENSIONS
    TranslateFlag(ETextureUsageFlags::VIDEO_ENCODE, VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR);
#endif
    TranslateFlag(ETextureUsageFlags::ATTACHMENT_FEEDBACK_LOOP, VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);

    return vk_flags;
}

VkImageLayout VulkanRHITexture::METoVKImageLayout(ETextureLayout _layout) {
    switch (_layout) {
        case ETextureLayout::TEXTURE_LAYOUT_UNDEFINED:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case ETextureLayout::TEXTURE_LAYOUT_COMMON:
            return VK_IMAGE_LAYOUT_GENERAL;
        case ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_READ:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_PRE_INITIALIZED:
            return VK_IMAGE_LAYOUT_PREINITIALIZED;
        case ETextureLayout::TEXTURE_LAYOUT_DEPTH_READ_STENCIL_WRITE:
            return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_DEPTH_WRITE_STENCIL_READ:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_DEPTH_WRITE:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_DEPTH_READ:
            return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_STENCIL_WRITE:
            return VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_STENCIL_READ:
            return VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
#ifdef VK_ENABLE_BETA_EXTENSIONS
        case ETextureLayout::TEXTURE_LAYOUT_VIDEO_ENCODE:
            return VK_IMAGE_LAYOUT_VIDEO_ENCODE_DST_KHR | VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR | VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR;
        case ETextureLayout::TEXTURE_LAYOUT_VIDEO_DECODE:
            return VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR | VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR | VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
#endif
        case ETextureLayout::TEXTURE_LAYOUT_READ:
            return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_WRITE:
            return VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        case ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case ETextureLayout::TEXTURE_LAYOUT_SHARED_PRESENT:
            return VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR;
        case ETextureLayout::TEXTURE_LAYOUT_FRAGMENT_DENSITY_MAP:
            return VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
        case ETextureLayout::TEXTURE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT:
            return VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        case ETextureLayout::TEXTURE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL:
            return VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
            //        case ETextureLayout::TEXTURE_LAYOUT_QUEUE_TYPE_GRAPHICS: // MARK...
            //            return VK_IMAGE_LAYOUT_GENERAL;
            //        case ETextureLayout::TEXTURE_LAYOUT_QUEUE_TYPE_COMPUTE:
            //            return VK_IMAGE_LAYOUT_GENERAL;
        default:
            LOG_CRITICAL("Unsupported texture layout: {}", static_cast<uint32_t>(_layout));
            return VK_IMAGE_LAYOUT_MAX_ENUM;
    }
}

#pragma endregion

#pragma region shader param
#pragma endregion

#pragma region synchronization

VulkanRHIFence::VulkanRHIFence(const std::string& _name, VulkanDevice* _device) : RHIFence(_name), m_device(_device) {
    VkFenceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    create_info.pNext = nullptr;
    create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VK_CHECK_RESULT(vkCreateFence(*m_device, &create_info, nullptr, &m_fence));
}

bool VulkanRHIFence::Signaled() const {
    return vkGetFenceStatus(*m_device, m_fence) == VK_SUCCESS;
}

#pragma endregion

#pragma region viewable resources view definitions

#pragma endregion

void* VulkanRHIStagingBuffer::GetSuballocationFromBuffer(uint32_t _size) {
    assert(reinterpret_cast<uint8_t*>(m_cur_ptr) + _size <= m_tail_ptr);
    return reinterpret_cast<uint8_t*>(m_cur_ptr) + _size;
}

#pragma region graphic pipeline definitions
#pragma endregion

#pragma region raytracing
#pragma endregion

#pragma region render query
#pragma endregion

#pragma region RDG resource creater
#pragma endregion
