//
// Created by 74535 on 2023/10/12.
//

#include "VulkanRHIResource.h"

#include "VulkanDevice.h"

#include "log/LogSystem.h"

VmaMemoryUsage VulkanMemoryManager::MEGenerateVmaMemoryUsage(EBufferUsageFlags _flags) {
    return VMA_MEMORY_USAGE_AUTO;
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
    return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
}

VkSampleCountFlagBits VulkanRHIMultisampleState::METoVKSampleCountFlagBits(uint32_t _me_count) {
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
