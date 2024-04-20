//
// Created by 74535 on 2023/10/12.
//

#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanSwapChain.h"
#include "VulkanDescriptor.h"
#include "VulkanRHIResource.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanCommand.h"

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "log/LogSystem.h"

#include <vulkan/vulkan_core.h>

#pragma region utils definition

VmaAllocationCreateFlags VulkanMemoryManager::MEGenerateVmaMemoryFlags(EBufferUsageFlags _flags) {
    if ((_flags & EBufferUsageFlags::CPU_VISIBLE) == EBufferUsageFlags::CPU_VISIBLE) return VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    return 0;
}

VmaMemoryUsage VulkanMemoryManager::MEGenerateVmaMemoryUsage() {
    return VMA_MEMORY_USAGE_AUTO;
}

VkIndexType VulkanEnumTranslator::METoVKIndexType(EIndexElementType _type) {
    switch (_type) {
        case IET_NONE:
            return VK_INDEX_TYPE_NONE_KHR;
        case IET_UINT8:
            return VK_INDEX_TYPE_UINT8_EXT;
        case IET_UINT16:
            return VK_INDEX_TYPE_UINT16;
        case IET_UINT32:
            return VK_INDEX_TYPE_UINT32;
        default:
            LOG_CRITICAL("Unsupported index element type: {}", static_cast<uint32_t>(_type));
            return VK_INDEX_TYPE_MAX_ENUM;
    }
}

VkFormat VulkanEnumTranslator::METoVKFormat(EPixelFormat _format) {
    if (_format > 184) {
        LOG_CRITICAL("Unsupported pixel format: {}", static_cast<uint32_t>(_format));
        return VK_FORMAT_MAX_ENUM;
    }
    return VkFormat(_format);// MARK...
}

EPixelFormat VulkanEnumTranslator::VKToMEFormat(VkFormat _format) {

    //translate format to pixel format
    switch (_format) {
        case VK_FORMAT_UNDEFINED:
            return PF_UNDEFINED;
        case VK_FORMAT_R4G4_UNORM_PACK8:
            return PF_R4G4_UNORM_PACK8;
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
            return PF_R4G4B4A4_UNORM_PACK16;
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
            return PF_B4G4R4A4_UNORM_PACK16;
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
            return PF_R5G6B5_UNORM_PACK16;
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
            return PF_B5G6R5_UNORM_PACK16;
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
            return PF_R5G5B5A1_UNORM_PACK16;
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
            return PF_B5G5R5A1_UNORM_PACK16;
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
            return PF_A1R5G5B5_UNORM_PACK16;
        case VK_FORMAT_R8_UNORM:
            return PF_R8_UNORM;
        case VK_FORMAT_R8_SNORM:
            return PF_R8_SNORM;
        case VK_FORMAT_R8_USCALED:
            return PF_R8_USCALED;
        case VK_FORMAT_R8_SSCALED:
            return PF_R8_SSCALED;
        case VK_FORMAT_R8_UINT:
            return PF_R8_UINT;
        case VK_FORMAT_R8_SINT:
            return PF_R8_SINT;
        case VK_FORMAT_R8_SRGB:
            return PF_R8_SRGB;
        case VK_FORMAT_R8G8_UNORM:
            return PF_R8G8_UNORM;
        case VK_FORMAT_R8G8_SNORM:
            return PF_R8G8_SNORM;
        case VK_FORMAT_R8G8_USCALED:
            return PF_R8G8_USCALED;
        case VK_FORMAT_R8G8_SSCALED:
            return PF_R8G8_SSCALED;
        case VK_FORMAT_R8G8_UINT:
            return PF_R8G8_UINT;
        case VK_FORMAT_R8G8_SINT:
            return PF_R8G8_SINT;
        case VK_FORMAT_R8G8_SRGB:// MARK...
            return PF_R8G8_SRGB;
        case VK_FORMAT_R8G8B8_UNORM:
            return PF_R8G8B8_UNORM;
        case VK_FORMAT_R8G8B8_SNORM:
            return PF_R8G8B8_SNORM;
        case VK_FORMAT_R8G8B8_USCALED:
            return PF_R8G8B8_USCALED;
        case VK_FORMAT_R8G8B8_SSCALED:
            return PF_R8G8B8_SSCALED;
        case VK_FORMAT_R8G8B8_UINT:
            return PF_R8G8B8_UINT;
        case VK_FORMAT_R8G8B8_SINT:
            return PF_R8G8B8_SINT;
        case VK_FORMAT_R8G8B8_SRGB:
            return PF_R8G8B8_SRGB;
        case VK_FORMAT_B8G8R8_UNORM:
            return PF_B8G8R8_UNORM;
        case VK_FORMAT_B8G8R8_SNORM:
            return PF_B8G8R8_SNORM;
        case VK_FORMAT_B8G8R8_USCALED:
            return PF_B8G8R8_USCALED;
        case VK_FORMAT_B8G8R8_SSCALED:
            return PF_B8G8R8_SSCALED;
        case VK_FORMAT_B8G8R8_UINT:
            return PF_B8G8R8_UINT;
        case VK_FORMAT_B8G8R8_SINT:
            return PF_B8G8R8_SINT;
        case VK_FORMAT_B8G8R8_SRGB:
            return PF_B8G8R8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return PF_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SNORM:
            return PF_R8G8B8A8_SNORM;
        case VK_FORMAT_R8G8B8A8_USCALED:
            return PF_R8G8B8A8_USCALED;
        case VK_FORMAT_R8G8B8A8_SSCALED:
            return PF_R8G8B8A8_SSCALED;
        case VK_FORMAT_R8G8B8A8_UINT:
            return PF_R8G8B8A8_UINT;
        case VK_FORMAT_R8G8B8A8_SINT:
            return PF_R8G8B8A8_SINT;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return PF_R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return PF_B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SNORM:
            return PF_B8G8R8A8_SNORM;
        case VK_FORMAT_B8G8R8A8_USCALED:
            return PF_B8G8R8A8_USCALED;
        case VK_FORMAT_B8G8R8A8_SSCALED:
            return PF_B8G8R8A8_SSCALED;
        case VK_FORMAT_B8G8R8A8_UINT:
            return PF_B8G8R8A8_UINT;
        case VK_FORMAT_B8G8R8A8_SINT:
            return PF_B8G8R8A8_SINT;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return PF_B8G8R8A8_SRGB;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
            return PF_A8B8G8R8_UNORM_PACK32;
        case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
            return PF_A8B8G8R8_SNORM_PACK32;
        case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
            return PF_A8B8G8R8_USCALED_PACK32;
        case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
            return PF_A8B8G8R8_SSCALED_PACK32;
        case VK_FORMAT_A8B8G8R8_UINT_PACK32:
            return PF_A8B8G8R8_UINT_PACK32;
        case VK_FORMAT_A8B8G8R8_SINT_PACK32:
            return PF_A8B8G8R8_SINT_PACK32;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return PF_A8B8G8R8_SRGB_PACK32;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return PF_A2R10G10B10_UNORM_PACK32;
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
            return PF_A2R10G10B10_SNORM_PACK32;
        case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
            return PF_A2R10G10B10_USCALED_PACK32;
        case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
            return PF_A2R10G10B10_SSCALED_PACK32;
        case VK_FORMAT_A2R10G10B10_UINT_PACK32:
            return PF_A2R10G10B10_UINT_PACK32;
        case VK_FORMAT_A2R10G10B10_SINT_PACK32:
            return PF_A2R10G10B10_SINT_PACK32;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return PF_A2B10G10R10_UNORM_PACK32;
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
            return PF_A2B10G10R10_SNORM_PACK32;
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
            return PF_A2B10G10R10_USCALED_PACK32;
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
            return PF_A2B10G10R10_SSCALED_PACK32;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return PF_A2B10G10R10_UINT_PACK32;
        case VK_FORMAT_A2B10G10R10_SINT_PACK32:
            return PF_A2B10G10R10_SINT_PACK32;
        case VK_FORMAT_R16_UNORM:
            return PF_R16_UNORM;
        case VK_FORMAT_R16_SNORM:
            return PF_R16_SNORM;
        case VK_FORMAT_R16_USCALED:
            return PF_R16_USCALED;
        case VK_FORMAT_R16_SSCALED:
            return PF_R16_SSCALED;
        case VK_FORMAT_R16_UINT:
            return PF_R16_UINT;
        case VK_FORMAT_R16_SINT:
            return PF_R16_SINT;
        case VK_FORMAT_R16_SFLOAT:
            return PF_R16_SFLOAT;
        case VK_FORMAT_R16G16_UNORM:
            return PF_R16G16_UNORM;
        case VK_FORMAT_R16G16_SNORM:
            return PF_R16G16_SNORM;
        case VK_FORMAT_R16G16_USCALED:
            return PF_R16G16_USCALED;
        case VK_FORMAT_R16G16_SSCALED:
            return PF_R16G16_SSCALED;
        case VK_FORMAT_R16G16_UINT:
            return PF_R16G16_UINT;
        case VK_FORMAT_R16G16_SINT:
            return PF_R16G16_SINT;
        case VK_FORMAT_R16G16_SFLOAT:
            return PF_R16G16_SFLOAT;
        case VK_FORMAT_R16G16B16_UNORM:
            return PF_R16G16B16_UNORM;
        case VK_FORMAT_R16G16B16_SNORM:
            return PF_R16G16B16_SNORM;
        case VK_FORMAT_R16G16B16_USCALED:
            return PF_R16G16B16_USCALED;
        case VK_FORMAT_R16G16B16_SSCALED:
            return PF_R16G16B16_SSCALED;
        case VK_FORMAT_R16G16B16_UINT:
            return PF_R16G16B16_UINT;
        case VK_FORMAT_R16G16B16_SINT:
            return PF_R16G16B16_SINT;
        case VK_FORMAT_R16G16B16_SFLOAT:
            return PF_R16G16B16_SFLOAT;
        case VK_FORMAT_R16G16B16A16_UNORM:
            return PF_R16G16B16A16_UNORM;
        case VK_FORMAT_R16G16B16A16_SNORM:
            return PF_R16G16B16A16_SNORM;
        case VK_FORMAT_R16G16B16A16_USCALED:
            return PF_R16G16B16A16_USCALED;
        case VK_FORMAT_R16G16B16A16_SSCALED:
            return PF_R16G16B16A16_SSCALED;
        case VK_FORMAT_R16G16B16A16_UINT:
            return PF_R16G16B16A16_UINT;
        case VK_FORMAT_R16G16B16A16_SINT:
            return PF_R16G16B16A16_SINT;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return PF_R16G16B16A16_SFLOAT;
        case VK_FORMAT_R32_UINT:
            return PF_R32_UINT;
        case VK_FORMAT_R32_SINT:
            return PF_R32_SINT;
        case VK_FORMAT_R32_SFLOAT:
            return PF_R32_SFLOAT;
        case VK_FORMAT_R32G32_UINT:
            return PF_R32G32_UINT;
        case VK_FORMAT_R32G32_SINT:
            return PF_R32G32_SINT;
        case VK_FORMAT_R32G32_SFLOAT:
            return PF_R32G32_SFLOAT;
        case VK_FORMAT_R32G32B32_UINT:
            return PF_R32G32B32_UINT;
        case VK_FORMAT_R32G32B32_SINT:
            return PF_R32G32B32_SINT;
        case VK_FORMAT_R32G32B32_SFLOAT:
            return PF_R32G32B32_SFLOAT;
        case VK_FORMAT_R32G32B32A32_UINT:
            return PF_R32G32B32A32_UINT;
        case VK_FORMAT_R32G32B32A32_SINT:
            return PF_R32G32B32A32_SINT;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return PF_R32G32B32A32_SFLOAT;
        case VK_FORMAT_R64_UINT:
            return PF_R64_UINT;
        case VK_FORMAT_R64_SINT:
            return PF_R64_SINT;
        case VK_FORMAT_R64_SFLOAT:

            return PF_R64_SFLOAT;
        case VK_FORMAT_R64G64_UINT:
            return PF_R64G64_UINT;
        case VK_FORMAT_R64G64_SINT:
            return PF_R64G64_SINT;
        case VK_FORMAT_R64G64_SFLOAT:
            return PF_R64G64_SFLOAT;
        case VK_FORMAT_R64G64B64_UINT:
            return PF_R64G64B64_UINT;
        case VK_FORMAT_R64G64B64_SINT:
            return PF_R64G64B64_SINT;
        case VK_FORMAT_R64G64B64_SFLOAT:
            return PF_R64G64B64_SFLOAT;
        case VK_FORMAT_R64G64B64A64_UINT:
            return PF_R64G64B64A64_UINT;
        case VK_FORMAT_R64G64B64A64_SINT:
            return PF_R64G64B64A64_SINT;
        case VK_FORMAT_R64G64B64A64_SFLOAT:
            return PF_R64G64B64A64_SFLOAT;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return PF_B10G11R11_UFLOAT_PACK32;
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
            return PF_E5B9G9R9_UFLOAT_PACK32;
        case VK_FORMAT_D16_UNORM:
            return PF_D16_UNORM;
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            return PF_X8_D24_UNORM_PACK32;
        case VK_FORMAT_D32_SFLOAT:
            return PF_D32_SFLOAT;
        case VK_FORMAT_S8_UINT:
            return PF_S8_UINT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
            return PF_D16_UNORM_S8_UINT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return PF_D24_UNORM_S8_UINT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return PF_D32_SFLOAT_S8_UINT;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            return PF_BC1_RGB_UNORM_BLOCK;
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            return PF_BC1_RGB_SRGB_BLOCK;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            return PF_BC1_RGBA_UNORM_BLOCK;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            return PF_BC1_RGBA_SRGB_BLOCK;
        case VK_FORMAT_BC2_UNORM_BLOCK:
            return PF_BC2_UNORM_BLOCK;
        case VK_FORMAT_BC2_SRGB_BLOCK:
            return PF_BC2_SRGB_BLOCK;
        case VK_FORMAT_BC3_UNORM_BLOCK:
            return PF_BC3_UNORM_BLOCK;
        case VK_FORMAT_BC3_SRGB_BLOCK:
            return PF_BC3_SRGB_BLOCK;
        case VK_FORMAT_BC4_UNORM_BLOCK:
            return PF_BC4_UNORM_BLOCK;
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return PF_BC4_SNORM_BLOCK;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            return PF_BC5_UNORM_BLOCK;
        case VK_FORMAT_BC5_SNORM_BLOCK:
            return PF_BC5_SNORM_BLOCK;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            return PF_BC6H_UFLOAT_BLOCK;
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            return PF_BC6H_SFLOAT_BLOCK;
        case VK_FORMAT_BC7_UNORM_BLOCK:
            return PF_BC7_UNORM_BLOCK;
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return PF_BC7_SRGB_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
            return PF_ETC2_R8G8B8_UNORM_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
            return PF_ETC2_R8G8B8_SRGB_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
            return PF_ETC2_R8G8B8A1_UNORM_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
            return PF_ETC2_R8G8B8A1_SRGB_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
            return PF_ETC2_R8G8B8A8_UNORM_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
            return PF_ETC2_R8G8B8A8_SRGB_BLOCK;
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
            return PF_EAC_R11_UNORM_BLOCK;
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
            return PF_EAC_R11_SNORM_BLOCK;
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
            return PF_EAC_R11G11_UNORM_BLOCK;
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            return PF_EAC_R11G11_SNORM_BLOCK;
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
            return PF_ASTC_4x4_UNORM_BLOCK;
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            return PF_ASTC_4x4_SRGB_BLOCK;
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
            return PF_ASTC_5x4_UNORM_BLOCK;
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
            return PF_ASTC_5x4_SRGB_BLOCK;
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
            return PF_ASTC_5x5_UNORM_BLOCK;
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
            return PF_ASTC_5x5_SRGB_BLOCK;
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
            return PF_ASTC_6x5_UNORM_BLOCK;
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:

            return PF_ASTC_6x5_SRGB_BLOCK;
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
            return PF_ASTC_6x6_UNORM_BLOCK;
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
            return PF_ASTC_6x6_SRGB_BLOCK;
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
            return PF_ASTC_8x5_UNORM_BLOCK;
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
            return PF_ASTC_8x5_SRGB_BLOCK;
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
            return PF_ASTC_8x6_UNORM_BLOCK;
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
            return PF_ASTC_8x6_SRGB_BLOCK;
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
            return PF_ASTC_8x8_UNORM_BLOCK;
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
            return PF_ASTC_8x8_SRGB_BLOCK;
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
            return PF_ASTC_10x5_UNORM_BLOCK;
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
            return PF_ASTC_10x5_SRGB_BLOCK;
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
            return PF_ASTC_10x6_UNORM_BLOCK;
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
            return PF_ASTC_10x6_SRGB_BLOCK;
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
            return PF_ASTC_10x8_UNORM_BLOCK;
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
            return PF_ASTC_10x8_SRGB_BLOCK;
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
            return PF_ASTC_10x10_UNORM_BLOCK;
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
            return PF_ASTC_10x10_SRGB_BLOCK;
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
            return PF_ASTC_12x10_UNORM_BLOCK;
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
            return PF_ASTC_12x10_SRGB_BLOCK;
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
            return PF_ASTC_12x12_UNORM_BLOCK;
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return PF_ASTC_12x12_SRGB_BLOCK;
        case VK_FORMAT_G8B8G8R8_422_UNORM:
            return PF_G8B8G8R8_422_UNORM;
        case VK_FORMAT_B8G8R8G8_422_UNORM:
            return PF_B8G8R8G8_422_UNORM;
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
            return PF_G8_B8_R8_3PLANE_420_UNORM;
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
            return PF_G8_B8R8_2PLANE_420_UNORM;
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
            return PF_G8_B8_R8_3PLANE_422_UNORM;
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
            return PF_G8_B8R8_2PLANE_422_UNORM;
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
            return PF_G8_B8_R8_3PLANE_444_UNORM;
        case VK_FORMAT_R10X6_UNORM_PACK16:
            return PF_R10X6_UNORM_PACK16;
        case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
            return PF_R10X6G10X6_UNORM_2PACK16;
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16:
            return PF_R10X6G10X6B10X6A10X6_UNORM_4PACK16;
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
            return PF_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16;
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
            return PF_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16;
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
            return PF_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16;
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
            return PF_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
            return PF_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16;
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
            return PF_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
            return PF_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16;
        case VK_FORMAT_R12X4_UNORM_PACK16:
            return PF_R12X4_UNORM_PACK16;
        case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
            return PF_R12X4G12X4_UNORM_2PACK16;
        case VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16:
            return PF_R12X4G12X4B12X4A12X4_UNORM_4PACK16;
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
            return PF_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16;
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
            return PF_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16;
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
            return PF_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16;
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
            return PF_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
            return PF_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16;
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
            return PF_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16;
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
            return PF_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16;
        case VK_FORMAT_G16B16G16R16_422_UNORM:
            return PF_G16B16G16R16_422_UNORM;
        case VK_FORMAT_B16G16R16G16_422_UNORM:
            return PF_B16G16R16G16_422_UNORM;
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
            return PF_G16_B16_R16_3PLANE_420_UNORM;
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
            return PF_G16_B16R16_2PLANE_420_UNORM;
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
            return PF_G16_B16_R16_3PLANE_422_UNORM;
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
            return PF_G16_B16R16_2PLANE_422_UNORM;
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
            return PF_G16_B16_R16_3PLANE_444_UNORM;
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
            return PF_PVRTC1_2BPP_UNORM_BLOCK_IMG;
        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
            return PF_PVRTC1_4BPP_UNORM_BLOCK_IMG;
        case VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG:
            return PF_PVRTC2_2BPP_UNORM_BLOCK_IMG;
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
            return PF_PVRTC2_4BPP_UNORM_BLOCK_IMG;
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
            return PF_PVRTC1_2BPP_SRGB_BLOCK_IMG;
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
            return PF_PVRTC1_4BPP_SRGB_BLOCK_IMG;
        case VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG:
            return PF_PVRTC2_2BPP_SRGB_BLOCK_IMG;
        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
            return PF_PVRTC2_4BPP_SRGB_BLOCK_IMG;
        case VK_FORMAT_R16G16_S10_5_NV:
            return PF_R16G16_S10_5_NV;
        default: return PF_UNDEFINED;
    }
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

VkImageAspectFlags VulkanEnumTranslator::METoVKImageAspectFlags(ETextureAspectFlags _flags) {
    return VkImageAspectFlags(_flags);
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

VkImageLayout VulkanEnumTranslator::METoVKImageLayout(ETextureLayout _layout) {
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

VkAttachmentLoadOp VulkanEnumTranslator::METoVKAttachmentLoadOp(EAttachmentLoadOp _load_op) {
    switch (_load_op) {
        case EAttachmentLoadOp::LOAD:
            return VK_ATTACHMENT_LOAD_OP_LOAD;
        case EAttachmentLoadOp::CLEAR:
            return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case EAttachmentLoadOp::NONE:
            return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        default:
            LOG_CRITICAL("Unsupported EAttachmentLoadOp: {}", static_cast<uint32_t>(_load_op));
            return VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    }
}

VkAttachmentStoreOp VulkanEnumTranslator::METoVKAttachmentStoreOp(EAttachmentStoreOp _store_op) {
    switch (_store_op) {
        case EAttachmentStoreOp::STORE:
            return VK_ATTACHMENT_STORE_OP_STORE;
        case EAttachmentStoreOp::NONE:
            return VK_ATTACHMENT_STORE_OP_DONT_CARE;
        case EAttachmentStoreOp::MULTISAMPLE_RESOLVE:
            return VK_ATTACHMENT_STORE_OP_STORE;
        default:
            LOG_CRITICAL("Unsupported EAttachmentStoreOp: {}", static_cast<uint32_t>(_store_op));
            return VK_ATTACHMENT_STORE_OP_MAX_ENUM;
    }
}

VkFilter VulkanEnumTranslator::METoVKImageFilter(ESamplerFilter _filter) {
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
            LOG_CRITICAL("Unsupported ESamplerFilter {}", static_cast<uint8_t>(_filter));
            return VK_FILTER_MAX_ENUM;
    }
}

VkPipelineStageFlags2 VulkanEnumTranslator::METoVkPipelineStageFlags2(ERHIPipelineStageFlags _flags) {
    VkPipelineStageFlags2 vk_flags = VK_PIPELINE_STAGE_2_NONE;

    auto translate_flag = [&vk_flags, &_flags](ERHIPipelineStageFlags _search_me_flags, VkPipelineStageFlags2 _added_if_found, VkPipelineStageFlags2 _added_if_not_found = 0) {
        const bool has_flag = (_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    translate_flag(PS_TOP_OF_PIPE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    translate_flag(PS_DRAW_INDIRECT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
    translate_flag(PS_VERTEX_INPUT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
    translate_flag(PS_VERTEX_SHADER, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    translate_flag(PS_TESSELLATION_CONTROL_SHADER, VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT);
    translate_flag(PS_TESSELLATION_EVALUATION_SHADER, VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT);
    translate_flag(PS_GEOMETRY_SHADER, VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT);
    translate_flag(PS_FRAGMENT_SHADER, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    translate_flag(PS_EARLY_FRAGMENT_TESTS, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT);
    translate_flag(PS_LATE_FRAGMENT_TESTS, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    translate_flag(PS_COLOR_ATTACHMENT_OUTPUT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    translate_flag(PS_COMPUTE_SHADER, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    translate_flag(PS_TRANSFER, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    translate_flag(PS_BOTTOM_OF_PIPE, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    translate_flag(PS_HOST, VK_PIPELINE_STAGE_2_HOST_BIT);
    translate_flag(PS_ALL_GRAPHICS, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
    translate_flag(PS_ALL_COMMANDS, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    translate_flag(PS_CONDITIONAL_RENDERING, VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT);
    translate_flag(PS_ACCELERATION_STRUCTURE_BUILD, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    translate_flag(PS_RAY_TRACING_SHADER, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    translate_flag(PS_FRAGMENT_DENSITY_PROCESS, VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT);
    translate_flag(PS_TASK_SHADER, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_NV);
    translate_flag(PS_MESH_SHADER, VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_NV);
    translate_flag(PS_COMMAND_PREPROCESS_BIT_NV, VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_NV);
    return vk_flags;
}

VkAccessFlags2 VulkanEnumTranslator::METoVkAccessFlags2(ERHIAccessFlags _flags) {

    VkAccessFlags2 vk_flags       = VK_ACCESS_2_NONE;
    auto           translate_flag = [&vk_flags, &_flags](ERHIAccessFlags _search_me_flags, VkAccessFlags2 _added_if_found, VkAccessFlags2 _added_if_not_found = 0) {
        const bool has_flag = (_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };
    // clang-format off
    translate_flag(ERHIAccessFlags::INDIRECT_COMMAND_READ, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    translate_flag(ERHIAccessFlags::INDEX_READ, VK_ACCESS_2_INDEX_READ_BIT);
    translate_flag(ERHIAccessFlags::VERTEX_ATTRIBUTE_READ, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    translate_flag(ERHIAccessFlags::UNIFORM_READ, VK_ACCESS_2_UNIFORM_READ_BIT);
    translate_flag(ERHIAccessFlags::INPUT_ATTACHMENT_READ, VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT);
    translate_flag(ERHIAccessFlags::SHADER_READ, VK_ACCESS_2_SHADER_READ_BIT);
    translate_flag(ERHIAccessFlags::SHADER_WRITE, VK_ACCESS_2_SHADER_WRITE_BIT);
    translate_flag(ERHIAccessFlags::COLOR_ATTACHMENT_READ, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    translate_flag(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    translate_flag(ERHIAccessFlags::DEPTH_STENCIL_READ, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    translate_flag(ERHIAccessFlags::DEPTH_STENCIL_WRITE, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    translate_flag(ERHIAccessFlags::TRANSFER_READ, VK_ACCESS_2_TRANSFER_READ_BIT);
    translate_flag(ERHIAccessFlags::TRANSFER_WRITE, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    translate_flag(ERHIAccessFlags::CPU_READ_BIT, VK_ACCESS_2_HOST_READ_BIT);
    translate_flag(ERHIAccessFlags::CPU_WRITE_BIT, VK_ACCESS_2_HOST_WRITE_BIT);
    translate_flag(ERHIAccessFlags::MEMORY_READ, VK_ACCESS_2_MEMORY_READ_BIT);
    translate_flag(ERHIAccessFlags::MEMORY_WRITE, VK_ACCESS_2_MEMORY_WRITE_BIT);
    translate_flag(ERHIAccessFlags::COMMAND_PREPROCESS_READ_BIT_NV, VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_NV);
    translate_flag(ERHIAccessFlags::COMMAND_PREPROCESS_WRITE_BIT_NV, VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_NV);
    translate_flag(ERHIAccessFlags::FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT, VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR);
    translate_flag(ERHIAccessFlags::FRAGMENT_DENSITY_MAP_READ_BIT_EXT, VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT);
    translate_flag(ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    translate_flag(ERHIAccessFlags::ACCELERATION_STRUCTURE_WRITE_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    translate_flag(ERHIAccessFlags::SHADER_SAMPLED_READ, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    translate_flag(ERHIAccessFlags::SHADER_RESOURCE_VIEW, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    translate_flag(ERHIAccessFlags::UNORDERED_ACCESS_VIEW, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    translate_flag(ERHIAccessFlags::TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT, VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT);
    translate_flag(ERHIAccessFlags::TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT, VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT);
    translate_flag(ERHIAccessFlags::TRANSFORM_FEEDBACK_WRITE_BIT_EXT,   VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT);
    translate_flag(ERHIAccessFlags::CONDITIONAL_RENDERING_READ_BIT_EXT, VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT);
    return vk_flags;
}

VkCullModeFlags VulkanEnumTranslator::METoVKCullModeFlags(ERasterizerCullMode _cull_mode) {
    switch (_cull_mode) {
        case ERasterizerCullMode::RCM_NONE:
            return VK_CULL_MODE_NONE;
        case ERasterizerCullMode::RCM_FRONT:
            return VK_CULL_MODE_FRONT_BIT;
        case ERasterizerCullMode::RCM_BACK:
            return VK_CULL_MODE_BACK_BIT;
        case ERasterizerCullMode::RCM_FRONT_AND_BACK:
            return VK_CULL_MODE_FRONT_AND_BACK;
        default:
            LOG_CRITICAL("Unsupported rasterizer cull mode: {}", static_cast<uint32_t>(_cull_mode));
            return VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
    }
}

VkPrimitiveTopology VulkanEnumTranslator::METoVKPrimitiveTopology(EPrimitiveTopology _primitive_type) {
    switch (_primitive_type) {
        case EPrimitiveTopology::POINT_LIST:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case EPrimitiveTopology::LINE_LIST:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case EPrimitiveTopology::LINE_STRIP:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case EPrimitiveTopology::TRIANGLE_LIST:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case EPrimitiveTopology::TRIANGLE_STRIP:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case EPrimitiveTopology::TRIANGLE_LIST_WITH_ADJACENCY:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case EPrimitiveTopology::TRIANGLE_STRIP_WITH_ADJACENCY:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case EPrimitiveTopology::PATCH_LIST:
            return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default:
            LOG_CRITICAL("Unsupported primitive topology: {}", static_cast<uint32_t>(_primitive_type));
            return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
}

VkPolygonMode VulkanEnumTranslator::METoVKPolygonMode(ERasterizerFillMode _fill_mode) {
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

VkBlendOp VulkanEnumTranslator::METoVKBlendOp(EBlendOperation _blend_op) {
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
VkBlendFactor VulkanEnumTranslator::METoVKBlendFactor(EBlendFactor _blend_factor) {
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

VkDescriptorType VulkanEnumTranslator::METoVKDescriptorType(EShaderParameterType _type, EShaderCodeResourceBindingType _binding_type) {

    auto is_texture = [](EShaderCodeResourceBindingType type) {
        return type == EShaderCodeResourceBindingType::TEXTURE_2D ||
               type == EShaderCodeResourceBindingType::TEXTURE_2D_ARRAY ||
               type == EShaderCodeResourceBindingType::TEXTURE_CUBE ||
               type == EShaderCodeResourceBindingType::TEXTURE_CUBE_ARRAY ||
               type == EShaderCodeResourceBindingType::TEXTURE_3D ||
               type == EShaderCodeResourceBindingType::RW_TEXTURE_2D ||
               type == EShaderCodeResourceBindingType::RW_TEXTURE_2D_ARRAY ||
               type == EShaderCodeResourceBindingType::RW_TEXTURE_3D;
    };
    auto is_buffer = [](EShaderCodeResourceBindingType type) {
        return type == EShaderCodeResourceBindingType::CONSTANT_BUFFER ||
               type == EShaderCodeResourceBindingType::STRUCTURED_BUFFER ||
               type == EShaderCodeResourceBindingType::BYTE_ADDRESS_BUFFER ||
               type == EShaderCodeResourceBindingType::RW_BYTE_ADDRESSED_BUFFER ||
               type == EShaderCodeResourceBindingType::RW_STRUCTURED_BUFFER;
    };
    switch (_type) {
        case EShaderParameterType::CBV:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case EShaderParameterType::SAMPLER:
        case EShaderParameterType::BINDLESS_SAMPLER_INDEX:// MARK...
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case EShaderParameterType::SRV:
            if (is_texture(_binding_type)) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            if (is_buffer(_binding_type)) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            LOG_ERROR("Unsupported SRV type: {}", ToString(_binding_type));
        case EShaderParameterType::UAV:
            if (is_texture(_binding_type)) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (is_buffer(_binding_type)) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case EShaderParameterType::BINDLESS_RESOURCE_INDEX:// MARK...
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        default:
            LOG_CRITICAL("Unsupported EShaderParameterType: {}", static_cast<uint32_t>(_type));
            return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

VkShaderStageFlags VulkanEnumTranslator::METoVKShaderStageFlags(EShaderType _type) {
    switch (_type) {
        case EShaderType::ST_VERTEX:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case EShaderType::ST_FRAGMENT:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case EShaderType::ST_GEOMETRY:
            return VK_SHADER_STAGE_GEOMETRY_BIT;
        case EShaderType::ST_COMPUTE:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        case EShaderType::ST_MESH:
            return VK_SHADER_STAGE_MESH_BIT_NV;
        case EShaderType::ST_AMPLIFICATION:
            return VK_SHADER_STAGE_TASK_BIT_NV;
        case EShaderType::ST_RAY_GEN:
            return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        case EShaderType::ST_RAY_MISS:
            return VK_SHADER_STAGE_MISS_BIT_KHR;
        case EShaderType::ST_RAY_CLOSESTHIT:
            return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        case EShaderType::ST_RAY_CALLABLE:
            return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
        case EShaderType::ST_RAY_INTERSECTION:
            return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
        case EShaderType::ST_RAY_ANYHIT:
            return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        default:
            LOG_CRITICAL("Unsupported EShaderType: {}", static_cast<uint32_t>(_type));
            return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    }
}

uint32_t VulkanEnumTranslator::METoVkQueueFamilyIndex(ECommandQueueType _type, const VulkanDevice* _device) {
    switch (_type) {
        case ECommandQueueType::GRAPHICS:
            return _device->GetQueueFamilyIndices().graphics.value();
        case ECommandQueueType::COMPUTE:
            return _device->GetQueueFamilyIndices().compute.value();
        case ECommandQueueType::COPY:
            return _device->GetQueueFamilyIndices().transfer.value();
        case ECommandQueueType::RAYTRACING:
            return _device->GetQueueFamilyIndices().raytracing.value();
        default:
            return VK_QUEUE_FAMILY_IGNORED;
    }
}

uint32_t VulkanEnumTranslator::METoVkQueueFamilyIndex(ECommandListType _type, const VulkanDevice* _device) {
    switch (_type) {
        case ECommandListType::GRAPHICS:
            return _device->GetQueueFamilyIndices().graphics.value();
        case ECommandListType::COMPUTE:
            return _device->GetQueueFamilyIndices().compute.value();
        case ECommandListType::COPY:
            return _device->GetQueueFamilyIndices().transfer.value();
        case ECommandListType::RAY_TRACING:
            return _device->GetQueueFamilyIndices().raytracing.value();
        default:
            return _device->GetQueueFamilyIndices().graphics.value();
    }
}

#pragma endregion

void VulkanRHISampler::GenerateSamplerFromInitializer(const VulkanDevice* _device, const RHISamplerCreateInfo& _initializer) {
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
        sampler_create_info.maxAnisotropy = std::clamp(static_cast<float>(_initializer.max_anisotropy), 1.0f, _device->GetCoreProperties().core_1_0.limits.maxSamplerAnisotropy);
    }
    sampler_create_info.anisotropyEnable = sampler_create_info.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;

    sampler_create_info.compareEnable = _initializer.compare_op != SCF_NEVER ? VK_TRUE : VK_FALSE;
    sampler_create_info.compareOp     = METoVKCompareOp(_initializer.compare_op);
    sampler_create_info.minLod        = _initializer.min_mip_level;
    sampler_create_info.maxLod        = _initializer.max_mip_level;
    sampler_create_info.borderColor   = _initializer.border_color == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

    VK_CHECK_RESULT(vkCreateSampler(*_device, &sampler_create_info, nullptr, &m_sampler));
    m_image_layout = VulkanEnumTranslator::METoVKImageLayout(_initializer.texture_layout);
}

VkFilter VulkanRHISampler::METoVKMinMagFilterMode(ESamplerFilter _filter) {
    return VulkanEnumTranslator::METoVKImageFilter(_filter);
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

VkVertexInputRate VulkanEnumTranslator::METoVKVertexInputRate(EVertexInputRate _me_rate) {
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

VkCompareOp VulkanEnumTranslator::METoVKCompareOp(ECompareOption _compare_op) {
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

VkStencilOp VulkanEnumTranslator::METoVKStencilOp(EStencilOp _stencil_op) {
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

#pragma region shader definitions

#pragma endregion

#pragma region pipeline states definitions

VulkanPipelineState::~VulkanPipelineState() {
    if (m_pipeline_layout) {
        vkDestroyPipelineLayout(m_device->GetDevice(), m_pipeline_layout, nullptr);
        m_pipeline_layout = VK_NULL_HANDLE;
    }
    if (m_pipeline) {
        vkDestroyPipeline(m_device->GetDevice(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    CHECK_AND_DELETE(m_descriptor_sets_layout);
    CHECK_AND_DELETE(m_pipeline_state_cache);
}

void VulkanPipelineState::InitDescriptorSetLayouts(Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings) {
    if (_descriptor_bindings.empty()) {
        return;
    }

    m_descriptor_sets_layout = MoerNew(VulkanDescriptorSetsLayout)(m_device, _descriptor_bindings);
}

void VulkanPipelineState::InitPipelineResourceCache(const Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings) {
    if (_descriptor_bindings.empty()) {
        return;
    }

    m_pipeline_state_cache = MoerNew(VulkanPipelineResourceCache)(m_descriptor_sets_layout, _descriptor_bindings);
}

void VulkanPipelineState::CreatePipelineLayout(const VkPipelineLayoutCreateInfo& _pipeline_layout_ci) {
    VK_CHECK_RESULT(vkCreatePipelineLayout(m_device->GetDevice(), &_pipeline_layout_ci, nullptr, &m_pipeline_layout));
}

VulkanRHIGraphicsPipelineState::~VulkanRHIGraphicsPipelineState() {
    // if (m_pipeline_state_cache) {
    //     delete m_pipeline_state_cache;
    //     m_pipeline_state_cache = nullptr;
    // }
    // if (m_descriptor_sets_layout) {
    //     delete m_descriptor_sets_layout;
    //     m_descriptor_sets_layout = nullptr;
    // }
}

Moer::Array<VkPipelineShaderStageCreateInfo> VulkanRHIGraphicsPipelineState::METoVKShaderStageCreateInfo(const RHIShaderBoundStateInput& _shader_bound_state) {
    Moer::Array<VkPipelineShaderStageCreateInfo> shader_stage_create_infos;
    // vert-frag pipeline
    VkPipelineShaderStageCreateInfo shader_stage_create_info{};
    shader_stage_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_create_info.pNext = nullptr;
    shader_stage_create_info.flags = 0;
    if (_shader_bound_state.p_vertex_shader) {
        auto* vk_vert_shader                         = static_cast<VulkanRHIVertexShader*>(_shader_bound_state.p_vertex_shader);
        shader_stage_create_info.stage               = VK_SHADER_STAGE_VERTEX_BIT;
        shader_stage_create_info.module              = vk_vert_shader->GetHandle();
        shader_stage_create_info.pName               = "main";
        shader_stage_create_info.pSpecializationInfo = nullptr;
        shader_stage_create_infos.push_back(shader_stage_create_info);
    }
    if (_shader_bound_state.p_geometry_shader) {
        auto* vk_geom_shader                         = static_cast<VulkanRHIGeometryShader*>(_shader_bound_state.p_geometry_shader);
        shader_stage_create_info.stage               = VK_SHADER_STAGE_GEOMETRY_BIT;
        shader_stage_create_info.module              = vk_geom_shader->GetHandle();
        shader_stage_create_info.pName               = "main";
        shader_stage_create_info.pSpecializationInfo = nullptr;
        shader_stage_create_infos.push_back(shader_stage_create_info);
    }
    // mesh-frag pipeline
    if (_shader_bound_state.p_mesh_shader) {
        auto* vk_mesh_shader                         = static_cast<VulkanRHIMeshShader*>(_shader_bound_state.p_mesh_shader);
        shader_stage_create_info.stage               = VK_SHADER_STAGE_MESH_BIT_NV;
        shader_stage_create_info.module              = vk_mesh_shader->GetHandle();
        shader_stage_create_info.pName               = "main";
        shader_stage_create_info.pSpecializationInfo = nullptr;
        shader_stage_create_infos.push_back(shader_stage_create_info);
    }
    if (_shader_bound_state.p_amplification_shader) {
        auto* vk_amp_shader                          = static_cast<VulkanRHIAmplificationShader*>(_shader_bound_state.p_amplification_shader);
        shader_stage_create_info.stage               = VK_SHADER_STAGE_TASK_BIT_NV;
        shader_stage_create_info.module              = vk_amp_shader->GetHandle();
        shader_stage_create_info.pName               = "main";
        shader_stage_create_info.pSpecializationInfo = nullptr;
        shader_stage_create_infos.push_back(shader_stage_create_info);
    }
    if (_shader_bound_state.p_fragment_shader) {
        auto* vk_frag_shader                         = static_cast<VulkanRHIFragmentShader*>(_shader_bound_state.p_fragment_shader);
        shader_stage_create_info.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
        shader_stage_create_info.module              = vk_frag_shader->GetHandle();
        shader_stage_create_info.pName               = "main";
        shader_stage_create_info.pSpecializationInfo = nullptr;
        shader_stage_create_infos.push_back(shader_stage_create_info);
    }

    return shader_stage_create_infos;
}

Moer::Array<const Shader*> VulkanRHIGraphicsPipelineState::GetShaderInfoList(const RHIShaderBoundStateInput& _shader_bound_state) {
    Moer::Array<const Shader*> shader_list;
    if (_shader_bound_state.p_vertex_shader->shader_type == EShaderType::ST_VERTEX) {
        shader_list.push_back(_shader_bound_state.p_vertex_shader->GetMetaShader());
    }
    if (_shader_bound_state.p_geometry_shader != nullptr) {
        shader_list.push_back(_shader_bound_state.p_geometry_shader->GetMetaShader());
    }
    // mesh-frag pipeline
    if (_shader_bound_state.p_mesh_shader != nullptr) {
        shader_list.push_back(_shader_bound_state.p_mesh_shader->GetMetaShader());
    }
    if (_shader_bound_state.p_amplification_shader != nullptr) {
        shader_list.push_back(_shader_bound_state.p_amplification_shader->GetMetaShader());
    }
    if (_shader_bound_state.p_fragment_shader != nullptr) {
        shader_list.push_back(_shader_bound_state.p_fragment_shader->GetMetaShader());
    }
    return shader_list;
}

void VulkanRHIGraphicsPipelineState::CreateGraphicsPipeline(const VkGraphicsPipelineCreateInfo& _info) {
    VK_CHECK_RESULT(vkCreateGraphicsPipelines(m_device->GetDevice(), VK_NULL_HANDLE, 1, &_info, nullptr, &m_pipeline));
}

void VulkanRHIComputePipelineState::CreateComputePipeline(const VkComputePipelineCreateInfo& _info) {
    VK_CHECK_RESULT(vkCreateComputePipelines(m_device->GetDevice(), VK_NULL_HANDLE, 1, &_info, nullptr, &m_pipeline));
}

void VulkanRHIRayTracingPipelineState::CreateRayTracingPipeline(const VkRayTracingPipelineCreateInfoKHR& _info) {
    // NOLINTNEXTLINE
    static auto vkCreateRayTracingPipelinesKHR       = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(m_device->GetDevice(), "vkCreateRayTracingPipelinesKHR"));
    
    VK_CHECK_RESULT(vkCreateRayTracingPipelinesKHR(m_device->GetDevice(), VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &_info, nullptr, &m_pipeline));
}

#pragma endregion

VulkanDeviceObject::VulkanDeviceObject(VulkanDevice* _device) : m_device(_device) {
}

#pragma region global buffer definitions
#pragma endregion

#pragma region viewable resources definitions

VkIndexType VulkanRHIBuffer::METoVKIndexType(EIndexElementType _type) {
    switch (_type) {
        case EIndexElementType::IET_NONE:
            return VK_INDEX_TYPE_NONE_KHR;
        case EIndexElementType::IET_UINT8:
            return VK_INDEX_TYPE_UINT8_EXT;
        case EIndexElementType::IET_UINT16:
            return VK_INDEX_TYPE_UINT16;
        case EIndexElementType::IET_UINT32:
            return VK_INDEX_TYPE_UINT32;
        default:
            LOG_CRITICAL("Unsupported index element type: {}", static_cast<uint32_t>(_type));
            return VK_INDEX_TYPE_MAX_ENUM;
    }
}

VkBufferUsageFlags VulkanRHIBuffer::METoVKBufferUsageFlags(VulkanDevice* _device, EBufferUsageFlags _me_flags) {
    // Always include TRANSFER_SRC since hardware vendors confirmed it wouldn't have any performance cost and we need it for some debug functionalities.
    VkBufferUsageFlags vk_flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    // NOLINTNEXTLINE
    auto TranslateFlag = [&vk_flags, &_me_flags](EBufferUsageFlags _search_me_flags, VkBufferUsageFlags _added_if_found, VkBufferUsageFlags _added_if_not_found = 0) {
        const bool has_flag = (_me_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    TranslateFlag(EBufferUsageFlags::VERTEX_BUFFER, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::INDEX_BUFFER, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::STORAGE_BUFFER, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::UNIFORM_BUFFER, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    TranslateFlag(EBufferUsageFlags::ACCELERATION_STRUCTURE, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
    TranslateFlag(EBufferUsageFlags::SHADER_BINDING_TABLE, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR);

    TranslateFlag(EBufferUsageFlags::UNORDERED_ACCESS, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::INDIRECT_BUFFER, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    TranslateFlag(EBufferUsageFlags::CPU_VISIBLE, (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    TranslateFlag(EBufferUsageFlags::SHADER_RESOURCE, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);

    TranslateFlag(EBufferUsageFlags::UNIFORM_BUFFER, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    TranslateFlag(EBufferUsageFlags::LIFE_CYCLE_ONE_FRAME, 0, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    TranslateFlag(EBufferUsageFlags::ACCELERATION_STRUCTURE_SCRATCH, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    TranslateFlag(EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

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

VulkanRHITexture::VulkanRHITexture(const RHITextureCreateInfo& _info, VulkanDevice* _device)
    : RHITexture(_info), VulkanDeviceObject(_device) {}

VulkanRHITexture::VulkanRHITexture(const RHITextureCreateInfo& _info, VkImage _image, VulkanDevice* _device)
    : RHITexture(_info),
      VulkanDeviceObject(_device),
      m_alloc{_image, VK_NULL_HANDLE} {}

VulkanRHITexture::~VulkanRHITexture() {
    if (m_alloc.alloc && m_alloc.image != VK_NULL_HANDLE) {
        //todo:
        // vmaDestroyImage(device->Get, m_alloc.image, m_alloc.alloc);
    }
}

VkImageUsageFlags VulkanRHITexture::METoVKImageUsageFlags(ETextureUsageFlags _me_flags) {
    // Always include TRANSFER_SRC since hardware vendors confirmed it wouldn't have any performance cost and we need it for some debug functionalities.
    VkImageUsageFlags vk_flags = 0;

    // NOLINTNEXTLINE
    auto TranslateFlag = [&vk_flags, &_me_flags](ETextureUsageFlags _search_me_flags, VkImageUsageFlags _added_if_found, VkImageUsageFlags _added_if_not_found = 0) {
        const bool has_flag = (_me_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    TranslateFlag(ETextureUsageFlags::TRANSFER_SRC, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    TranslateFlag(ETextureUsageFlags::TRANSFER_DST, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    TranslateFlag(ETextureUsageFlags::SAMPLED, VK_IMAGE_USAGE_SAMPLED_BIT);
    TranslateFlag(ETextureUsageFlags::UNORDERED_ACCESS, VK_IMAGE_USAGE_STORAGE_BIT);
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
    // TranslateFlag(ETextureUsageFlags::ATTACHMENT_FEEDBACK_LOOP, VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);

    return vk_flags;
}

#pragma endregion

#pragma region shader param
#pragma endregion

#pragma region synchronization

VulkanRHIFence::VulkanRHIFence(VulkanDevice* _device, EFenceUsageFlags _usage) : m_device(_device), m_binary(VK_NULL_HANDLE), m_timeline(VK_NULL_HANDLE), usage(_usage) {
    VkSemaphoreCreateInfo create_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (EnumHasAnyFlag(usage, EFenceUsageFlags::PRESENT)) {
        VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &m_binary));

        VkSemaphoreTypeCreateInfo timeline_semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline_semaphore_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timeline_semaphore_info.initialValue  = 0;

        create_info.pNext = &timeline_semaphore_info;
        VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &m_timeline));
        return;
    }
    if (EnumHasAnyFlag(usage, EFenceUsageFlags::BINARY)) {
        VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &m_binary));
    }
    if (EnumHasAnyFlag(usage, EFenceUsageFlags::TIMELINE)) {
        VkSemaphoreTypeCreateInfo timeline_semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline_semaphore_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timeline_semaphore_info.initialValue  = 0;

        create_info.pNext = &timeline_semaphore_info;
        VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &m_timeline));
    }
}

VulkanRHIFence::~VulkanRHIFence() {

    VkSemaphoreWaitInfo wait_delete_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait_delete_info.pNext          = nullptr;
    wait_delete_info.flags          = 0;
    wait_delete_info.semaphoreCount = 1;
    if (m_binary != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device->GetDevice(), m_binary, VK_NULL_HANDLE);
    }
    if (m_timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device->GetDevice(), m_timeline, VK_NULL_HANDLE);
    }
}

uint64_t VulkanRHIFence::GetValue() const {
    uint64_t value;
    vkGetSemaphoreCounterValue(m_device->GetDevice(), m_timeline, &value);
    return value;
}

void VulkanRHIFence::Wait(uint64_t value) {
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.pSemaphores    = &m_timeline;
    info.semaphoreCount = 1;
    info.pValues        = &value;
    vkWaitSemaphores(m_device->GetDevice(), &info, UINT64_MAX);
}

#pragma endregion

#pragma region viewable resources view definitions
VulkanRHICBV::~VulkanRHICBV() {
 
}


VulkanRHITextureUAV::~VulkanRHITextureUAV() {
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device->GetDevice(), m_view, VK_NULL_HANDLE);
    }
}

VulkanRHITextureSRV::~VulkanRHITextureSRV() {
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device->GetDevice(), m_view, VK_NULL_HANDLE);
    }
}

VulkanRHIBufferSRV::~VulkanRHIBufferSRV() {
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyBufferView(m_device->GetDevice(), m_view, VK_NULL_HANDLE);
    }
}

VulkanRHIBufferUAV::~VulkanRHIBufferUAV() {
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyBufferView(m_device->GetDevice(), m_view, VK_NULL_HANDLE);
    }
}

#pragma endregion

#pragma region viewport
VulkanViewport::VulkanViewport(VulkanSwapChain* _swapchain, uint32_t _max_frame_in_flight) : RHIViewport() {
    swapchain                = _swapchain;
    info.max_frame_in_flight = _max_frame_in_flight;
    InnerCreateResources();
}

VulkanViewport::~VulkanViewport() {
    InnerDestroyResources();

    MoerDelete(swapchain);
    swapchain = nullptr;
}

VulkanRHITextureUAV* VulkanViewport::InnerCreateVulkanUAV(VulkanDevice* _device, VulkanRHITexture* texture, const RHIViewInfo& _view_info) {
    auto* view = MoerNew(VulkanRHITextureUAV)(_device, texture, _view_info);

    VkImageViewCreateInfo image_view_create_info{};
    image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_create_info.pNext = nullptr;
    image_view_create_info.flags = 0;

    auto& uav_info = std::get<v_type_texture_uav>(_view_info.info);

    image_view_create_info.image                           = texture->GetHandle();
    image_view_create_info.viewType                        = VulkanEnumTranslator::METoVKImageViewType(uav_info.dimension);
    image_view_create_info.format                          =  VulkanEnumTranslator::METoVKFormat(texture->GetFormat());
    image_view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    image_view_create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;// MARK...
    image_view_create_info.subresourceRange.baseMipLevel   = uav_info.mip;
    image_view_create_info.subresourceRange.levelCount     = 1;
    image_view_create_info.subresourceRange.baseArrayLayer = uav_info.array_min ;
    image_view_create_info.subresourceRange.layerCount     = uav_info.array_num;

    VK_CHECK_RESULT(vkCreateImageView(_device->GetDevice(), &image_view_create_info, nullptr, &view->m_view));

    return view;
}

void VulkanViewport::InnerCreateResources() {
    uint32_t back_buffer_size = swapchain->m_swap_chain_images.size();

    info.max_frame_in_flight = 3;
    swapchain_image_uavs.resize(back_buffer_size);
    image_aquire_fences.resize(info.max_frame_in_flight);
    swapchain_images.resize(back_buffer_size);

    EPixelFormat swapchain_format = VulkanEnumTranslator::VKToMEFormat(swapchain->image_format);
    for (uint32_t index = 0; index < swapchain_image_uavs.size(); index++) {
        swapchain_images[index] = MoerNew(VulkanRHITexture)(RHITextureCreateInfo::Create2D("swapchain")
                                                           .SetExtent(swapchain->extent.width, swapchain->extent.height)
                                                           .SetFormat(swapchain_format)
                                                           .SetUAVFormat(swapchain_format),
                                                       swapchain->m_swap_chain_images[index],
                                                       swapchain->m_device);
        swapchain_images[index]->AddRef();
        RHITextureUAVInfo uav_info;
        uav_info.dimension = ETextureDimension::TEX_2D;
        uav_info.format    = swapchain_format;
        uav_info.array_min = 0;
        uav_info.array_num = 1;
        uav_info.mip       = 0;


        RHIViewInfo view_info(std::move(uav_info));
        swapchain_image_uavs[index] = InnerCreateVulkanUAV(
            swapchain->m_device,
            swapchain_images[index],
            std::move(view_info));
    }
    for (uint32_t index = 0; index < image_aquire_fences.size(); index++) {
        image_aquire_fences[index] = MoerNew( VulkanRHIFence)(swapchain->m_device, EFenceUsageFlags::BINARY);
    }
    frame_offset = 0;

    //init information

    info.backbuffer_format = swapchain_format;
}
void VulkanViewport::InnerDestroyResources() {

    for (uint32_t index = 0; index < swapchain_image_uavs.size(); index++) {
        MoerDelete( swapchain_image_uavs[index]);
    }
    for (uint32_t index = 0; index < image_aquire_fences.size(); index++) {
        VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        VkSemaphore         temp[1] = {image_aquire_fences[index]->GetBinaryHandle()};
        wait_info.semaphoreCount    = 1;
        wait_info.pSemaphores       = temp;
        wait_info.pValues           = 0;
        // vkWaitSemaphores(swapchain->m_device->GetDevice(), &wait_info, UINT64_MAX);
        MoerDelete( image_aquire_fences[index]);
    }
}

void VulkanViewport::ResetResources() {
    assert(swapchain != nullptr);

    uint32_t back_buffer_size = swapchain->m_swap_chain_images.size();

    swapchain_image_uavs.resize(back_buffer_size);
    image_aquire_fences.resize(info.max_frame_in_flight);
    swapchain_images.resize(back_buffer_size);

    for (uint32_t index = 0; index < swapchain_image_uavs.size(); index++) {
        swapchain_images[index]->SetAttachedImageInner(swapchain->m_swap_chain_images[index]);
        EPixelFormat swapchain_format = VulkanEnumTranslator::VKToMEFormat(swapchain->image_format);

        MoerDelete( swapchain_image_uavs[index]);

        RHITextureUAVInfo uav_info;
        uav_info.dimension = ETextureDimension::TEX_2D;
        uav_info.format    = swapchain_format;
        uav_info.array_min = 0;
        uav_info.array_num = 1;
        uav_info.mip       = 0;


        RHIViewInfo view_info(std::move(uav_info));
        swapchain_image_uavs[index] = InnerCreateVulkanUAV(
            swapchain->m_device,
            swapchain_images[index],
             std::move(view_info));
    }
    for (uint32_t index = 0; index < image_aquire_fences.size(); index++) {
        MoerDelete( image_aquire_fences[index]);
        image_aquire_fences[index] = MoerNew( VulkanRHIFence)(swapchain->m_device, EFenceUsageFlags::BINARY);
    }
    info.backbuffer_format = VulkanEnumTranslator::VKToMEFormat(swapchain->image_format);
    frame_offset           = 0;
}
void VulkanViewport::OnResize(Extent2D _size) {

    swapchain->Recreate();
    ResetResources();
}

VulkanRHIFence* VulkanViewport::GetAcquireNextImageFence() {
    return image_aquire_fences[frame_offset = (frame_offset - 1) % info.max_frame_in_flight];
}
RHIViewportNextBackBufferInfo VulkanViewport::GetNextFrameBackBufferInfo() {
    uint32_t index = swapchain->AcquireNextImage(image_aquire_fences[frame_offset]->GetBinaryHandle());
    if (index != UINT32_MAX) {
        auto current_frame = frame_offset;
        frame_offset       = (frame_offset + 1) % info.max_frame_in_flight;
        return {.backbuffer_index = index, .backbuffer_ready_fence = image_aquire_fences[current_frame]};
    }
    //recreate
    swapchain->Recreate();
    ResetResources();
    return {.backbuffer_index = UINT32_MAX, .backbuffer_ready_fence = nullptr};
}
VulkanRHITextureUAV* VulkanViewport::GetCurrentBackBuffer(uint32_t index) {
    if (index != UINT32_MAX) {
        return swapchain_image_uavs[index];
    }
    return nullptr;
}
void VulkanViewport::Present(RHIFence* _render_finished) {

    assert(_render_finished && "Fence Empty");
    VulkanRHIFence* vk_fence = (VulkanRHIFence*)_render_finished;
    VulkanDevice*   device   = swapchain->m_device;
    assert(device != nullptr && "Swapchain not valid");

    VkResult result = swapchain->Present(device->GetPresentQueue(), vk_fence->GetBinaryHandle());
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        swapchain->Recreate();
        ResetResources();
    }
    // swapchain->Present();
}

void VulkanViewport::WaitForQueueComplete(RHICommandQueue* _command_queue, RHIFence* _optional_fence) {
    if (!_command_queue) return;
    VulkanRHICommandQueue* vk_queue = dynamic_cast<VulkanRHICommandQueue*>(_command_queue);
    vkQueueWaitIdle(vk_queue->GetHandle());
}
ViewPort VulkanViewport::GetViewportExtent() const {
    return ViewPort{0, 0, (float)swapchain->extent.width, (float)swapchain->extent.height, 0.f, 1.f};
}
#pragma endregion

#pragma region graphic pipeline definitions

#pragma endregion

#pragma region    raytracing
VkGeometryTypeKHR VulkanRHIRayTracingAccelerationStructure::METoVKGeometryTypeKHR(ERayTracingGeometryType _type) {
    switch (_type) {
        case RTGT_TRIANGLES:
            return VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        case RTGT_AABBS:
            LOG_CRITICAL("GeometryType AABB is not supported now");
            return VK_GEOMETRY_TYPE_AABBS_KHR;
        default:
            LOG_CRITICAL("Unsupported ERayTracingGeometryType: {}", static_cast<uint32_t>(_type));
            return VK_GEOMETRY_TYPE_MAX_ENUM_KHR;
    }
}
VkGeometryFlagsKHR VulkanRHIRayTracingAccelerationStructure::METoGeometryFlagsKHR(ERayTracingGeometryFlags _me_flags) {
    VkGeometryFlagsKHR vk_flags = 0;

    auto TranslateFlag = [&vk_flags, &_me_flags](ERayTracingGeometryFlags _search_me_flags, VkGeometryFlagsKHR _added_if_found, VkGeometryFlagsKHR _added_if_not_found = 0) {
        const bool has_flag = (_me_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };
    TranslateFlag(ERayTracingGeometryFlags::GEOMETRY_OPAQUE, VK_GEOMETRY_OPAQUE_BIT_KHR);
    TranslateFlag(ERayTracingGeometryFlags::NO_DUPLICATE_ANY_HIT_INVOCATION, VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR);
    return vk_flags;
}
VkBuildAccelerationStructureFlagsKHR VulkanRHIRayTracingAccelerationStructure::METoVKBuildAccelerationStructureFlagsKHR(ERayTracingAccelerationStructureBuildFlags _me_flags) {
    VkBuildAccelerationStructureFlagsKHR vk_flags = 0;

    auto TranslateFlag = [&vk_flags, &_me_flags](ERayTracingAccelerationStructureBuildFlags _search_me_flags, VkBuildAccelerationStructureFlagsKHR _added_if_found, VkBuildAccelerationStructureFlagsKHR _added_if_not_found = 0) {
        const bool has_flag = (_me_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    TranslateFlag(ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
    TranslateFlag(ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_BUILD, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR);
    TranslateFlag(ERayTracingAccelerationStructureBuildFlags::ALLOW_COMPACTION, VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
    TranslateFlag(ERayTracingAccelerationStructureBuildFlags::ALLOW_UPDATE, VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR);
    TranslateFlag(ERayTracingAccelerationStructureBuildFlags::MINIMIZE_MEMORY, VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR);

    return vk_flags;
}
VkGeometryInstanceFlagsKHR VulkanRHIRayTracingAccelerationStructure::METoVKGeometryInstanceFlagsKHR(ERayTracingInstanceFlags _me_flags) {
    VkGeometryInstanceFlagsKHR vk_flags = 0;

    auto TranslateFlag = [&vk_flags, &_me_flags](ERayTracingInstanceFlags _search_me_flags, VkGeometryInstanceFlagsKHR _added_if_found, VkGeometryInstanceFlagsKHR _added_if_not_found = 0) {
        const bool has_flag = (_me_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    TranslateFlag(ERayTracingInstanceFlags::FORCE_OPAQUE, VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    TranslateFlag(ERayTracingInstanceFlags::FORCE_NO_OPAQUE, VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR, VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    TranslateFlag(ERayTracingInstanceFlags::TRIANGLE_CULL_DISABLE, VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
    TranslateFlag(ERayTracingInstanceFlags::TRIANGLE_FRONT_COUNTERCLOCKWISE, VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR);

    return vk_flags;
}
#pragma endregion

#pragma region render query
#pragma endregion

#pragma region RDG resource creater
#pragma endregion
