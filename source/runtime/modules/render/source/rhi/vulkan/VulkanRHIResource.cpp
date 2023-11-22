//
// Created by 74535 on 2023/10/12.
//

#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanSwapChain.h"
#include "VulkanDescriptor.h"
#include "VulkanRHIResource.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanCommandQueue.h"

#include "rhi/RHI.h"
#include "rhi/RHICommandQueue.h"
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
    // translate flags
    switch (_flags) {
        case PS_NONE:
            return VK_PIPELINE_STAGE_2_NONE;
        case PS_TOP_OF_PIPE:
            return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        case PS_DRAW_INDIRECT:
            return VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        case PS_VERTEX_INPUT:
            return VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        case PS_VERTEX_SHADER:
            return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        case PS_TESSELLATION_CONTROL_SHADER:
            return VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
        case PS_TESSELLATION_EVALUATION_SHADER:
            return VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
        case PS_GEOMETRY_SHADER:
            return VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
        case PS_FRAGMENT_SHADER:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case PS_EARLY_FRAGMENT_TESTS:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        case PS_LATE_FRAGMENT_TESTS:
            return VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case PS_COLOR_ATTACHMENT_OUTPUT:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case PS_COMPUTE_SHADER:
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case PS_TRANSFER:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case PS_BOTTOM_OF_PIPE:
            return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        case PS_HOST:
            return VK_PIPELINE_STAGE_2_HOST_BIT;
        case PS_ALL_GRAPHICS:
            return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        case PS_ALL_COMMANDS:
            return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        case PS_CONDITIONAL_RENDERING:
            return VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
        case PS_ACCELERATION_STRUCTURE_BUILD:
            return VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        case PS_RAY_TRACING_SHADER:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case PS_FRAGMENT_DENSITY_PROCESS:
            return VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT;
        case PS_TASK_SHADER:
            return VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_NV;
        case PS_MESH_SHADER:
            return VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_NV;
        case PS_COMMAND_PREPROCESS_BIT_NV:
            return VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_NV;
        default:
            return VkPipelineStageFlags2(_flags);// MARK...
    }
}

VkAccessFlags2 VulkanEnumTranslator::METoVkAccessFlags2(ERHIAccessFlags _flags) {
    // clang-format off
    switch (_flags) {
        case ERHIAccessFlags::UNDEFINED:                                    return VK_ACCESS_2_NONE;
        case ERHIAccessFlags::INDIRECT_COMMAND_READ:                        return VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        case ERHIAccessFlags::INDEX_READ:                                   return VK_ACCESS_2_INDEX_READ_BIT;
        case ERHIAccessFlags::VERTEX_ATTRIBUTE_READ:                        return VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        case ERHIAccessFlags::UNIFORM_READ:                                 return VK_ACCESS_2_UNIFORM_READ_BIT;
        case ERHIAccessFlags::INPUT_ATTACHMENT_READ:                        return VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
        case ERHIAccessFlags::SHADER_READ:                                  return VK_ACCESS_2_SHADER_READ_BIT;
        case ERHIAccessFlags::SHADER_WRITE:                                 return VK_ACCESS_2_SHADER_WRITE_BIT;
        case ERHIAccessFlags::COLOR_ATTACHMENT_READ:                        return VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        case ERHIAccessFlags::COLOR_ATTACHMENT_WRITE:                       return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case ERHIAccessFlags::DEPTH_STENCIL_READ:                           return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        case ERHIAccessFlags::DEPTH_STENCIL_WRITE:                          return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case ERHIAccessFlags::TRANSFER_READ:                                return VK_ACCESS_2_TRANSFER_READ_BIT;
        case ERHIAccessFlags::TRANSFER_WRITE:                               return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        case ERHIAccessFlags::CPU_READ_BIT:                                 return VK_ACCESS_2_HOST_READ_BIT;
        case ERHIAccessFlags::CPU_WRITE_BIT:                                return VK_ACCESS_2_HOST_WRITE_BIT;
        case ERHIAccessFlags::MEMORY_READ:                                  return VK_ACCESS_2_MEMORY_READ_BIT;
        case ERHIAccessFlags::MEMORY_WRITE:                                 return VK_ACCESS_2_MEMORY_WRITE_BIT;
        case ERHIAccessFlags::SHADER_SAMPLED_READ:                          return VK_ACCESS_2_SHADER_READ_BIT;
        case ERHIAccessFlags::SHADER_RESOURCE_VIEW:                         return VK_ACCESS_2_SHADER_READ_BIT;
        case ERHIAccessFlags::UNORDERED_ACCESS_VIEW:                        return VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        case ERHIAccessFlags::TRANSFORM_FEEDBACK_WRITE_BIT_EXT:             return VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
        case ERHIAccessFlags::TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT:      return VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT;
        case ERHIAccessFlags::TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT:     return VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
        case ERHIAccessFlags::CONDITIONAL_RENDERING_READ_BIT_EXT:           return VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
        case ERHIAccessFlags::COMMAND_PREPROCESS_READ_BIT_NV:               return VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_NV;
        case ERHIAccessFlags::COMMAND_PREPROCESS_WRITE_BIT_NV:              return VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_NV;
        case ERHIAccessFlags::FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT:    return VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
        case ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT:              return VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        case ERHIAccessFlags::ACCELERATION_STRUCTURE_WRITE_BIT:             return VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        case ERHIAccessFlags::FRAGMENT_DENSITY_MAP_READ_BIT_EXT:            return VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
        default:
            LOG_CRITICAL("Unsupported ERHIAccessFlags: {}", static_cast<uint32_t>(_flags));
            return VK_ACCESS_FLAG_BITS_MAX_ENUM;
    }
    // clang-format on
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

VkDescriptorType VulkanEnumTranslator::METoVKDescriptorType(EShaderParameterType _type) {
    switch (_type) {
        case EShaderParameterType::CBV:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case EShaderParameterType::SAMPLER:
        case EShaderParameterType::BINDLESS_SAMPLER_INDEX:// MARK...
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case EShaderParameterType::SRV:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case EShaderParameterType::UAV:
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
        case EShaderType::ST_RAY_HIT:
            return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        case EShaderType::ST_RAY_CALLABLE:
            return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
        default:
            LOG_CRITICAL("Unsupported EShaderType: {}", static_cast<uint32_t>(_type));
            return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
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
        sampler_create_info.maxAnisotropy = std::clamp(static_cast<float>(_initializer.max_anisotropy), 1.0f, _device->GetProperties().properties.limits.maxSamplerAnisotropy);
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

void VulkanRHIVertexInputState::GenerateVertexInputStateFromInitializer(const VertexInputStateInitializerList& _init) {
    for (uint32_t i = 0; i < MAX_VERTEX_ELEMENT_COUNT; ++i) {
        if (_init[i].format == EPixelFormat::PF_UNDEFINED) {
            break;
        }
        m_bindings[i].binding    = _init[i].binding_index;
        m_bindings[i].stride     = _init[i].stride;
        m_bindings[i].inputRate  = METoVKVertexInputRate(_init[i].input_rate);
        m_attributes[i].location = _init[i].attribute_index;
        m_attributes[i].binding  = _init[i].binding_index;
        m_attributes[i].format   = VulkanEnumTranslator::METoVKFormat(_init[i].format);
        m_attributes[i].offset   = _init[i].offset;

        // fallback
        m_binding_count = std::max(m_binding_count, static_cast<uint32_t>(_init[i].binding_index));
        ++m_attribute_count;
    }
    // count = max_index + 1
    ++m_binding_count;

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
    m_depth_stencil_state_create_info.front.failOp      = METoVKStencilOp(_init.front_face_stencil_fail_stencil_op);
    m_depth_stencil_state_create_info.front.passOp      = METoVKStencilOp(_init.front_face_pass_stencil_op);
    m_depth_stencil_state_create_info.front.depthFailOp = METoVKStencilOp(_init.front_face_depth_fail_stencil_op);
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
    // const auto n = 1;// MARK...

    // attachments.resize(MAX_PASS_ATTACHMENT_COUNT);
    for (size_t i = 0; i < MAX_PASS_ATTACHMENT_COUNT; ++i) {
        auto& attachment_init = _init.attachments[i];

        m_attachments[i].blendEnable =
            (attachment_init.color_blend_op != BO_ADD || attachment_init.color_dst_blend_factor != BF_ZERO || attachment_init.color_src_blend_factor != BF_ONE ||
             attachment_init.alpha_blend_op != BO_ADD || attachment_init.alpha_dst_blend_factor != BF_ZERO || attachment_init.alpha_src_blend_factor != BF_ONE) ?
                VK_TRUE :
                VK_FALSE;
        m_attachments[i].srcColorBlendFactor = VulkanRHIBlendState::METoVKBlendFactor(attachment_init.color_src_blend_factor);
        m_attachments[i].dstColorBlendFactor = VulkanRHIBlendState::METoVKBlendFactor(attachment_init.color_dst_blend_factor);
        m_attachments[i].colorBlendOp        = VulkanRHIBlendState::METoVKBlendOp(attachment_init.color_blend_op);
        m_attachments[i].srcAlphaBlendFactor = VulkanRHIBlendState::METoVKBlendFactor(attachment_init.alpha_src_blend_factor);
        m_attachments[i].dstAlphaBlendFactor = VulkanRHIBlendState::METoVKBlendFactor(attachment_init.alpha_dst_blend_factor);
        m_attachments[i].alphaBlendOp        = VulkanRHIBlendState::METoVKBlendOp(attachment_init.alpha_blend_op);
        m_attachments[i].colorWriteMask      = (attachment_init.color_write_mask & CW_RED) ? VK_COLOR_COMPONENT_R_BIT : 0;
        m_attachments[i].colorWriteMask |= (attachment_init.color_write_mask & CW_GREEN) ? VK_COLOR_COMPONENT_G_BIT : 0;
        m_attachments[i].colorWriteMask |= (attachment_init.color_write_mask & CW_BLUE) ? VK_COLOR_COMPONENT_B_BIT : 0;
        m_attachments[i].colorWriteMask |= (attachment_init.color_write_mask & CW_ALPHA) ? VK_COLOR_COMPONENT_A_BIT : 0;
    }

    // // VkPipelineColorBlendStateCreateInfo blend_state_create_info{};
    // m_blend_state_create_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    // m_blend_state_create_info.pNext           = nullptr;
    // m_blend_state_create_info.flags           = 0;
    // m_blend_state_create_info.logicOpEnable   = VK_FALSE;
    // m_blend_state_create_info.logicOp         = VK_LOGIC_OP_COPY;
    // m_blend_state_create_info.attachmentCount = n;
    // m_blend_state_create_info.pAttachments    = attachments.data();
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

void VulkanRHIGraphicsPipelineState::GenerateDescriptorSetLayouts(const VulkanDevice* _device, std::vector<TDescriptorSetLayout>& _layout_mappings) {
    // create descriptor set layouts
    for (auto& layout : _layout_mappings) {
        VkDescriptorSetLayoutCreateInfo layout_create_info{};
        layout_create_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_create_info.pNext        = nullptr;
        layout_create_info.flags        = 0;
        layout_create_info.bindingCount = layout.second.size();
        layout_create_info.pBindings    = layout.second.empty() ? nullptr : layout.second.data();

        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(*_device, &layout_create_info, nullptr, &layout.first));
    }

    // extract descriptor set layouts
    m_descriptor_sets_layout = new VulkanDescriptorSetsLayout();
    m_descriptor_sets_layout->Init(_layout_mappings, m_pipeline_state_cache);
}

void VulkanRHIGraphicsPipelineState::CreateResourceCache() {
    m_pipeline_state_cache = new VulkanPipelineResourceCache();
}

std::vector<VkPipelineShaderStageCreateInfo> VulkanRHIGraphicsPipelineState::METoVKShaderStageCreateInfo(const RHIShaderBoundStateInput& _shader_bound_state) {
    std::vector<VkPipelineShaderStageCreateInfo> shader_stage_create_infos;
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

VkPipelineVertexInputStateCreateInfo VulkanRHIGraphicsPipelineState::METoVKVertexInputStateCreateInfo(const RHIVertexInputState* _vertex_input_state) {
    auto* vk_vertex_input_state = static_cast<const VulkanRHIVertexInputState*>(_vertex_input_state);
    VK_CHECK_NULLPTR(vk_vertex_input_state, "RHICreateGraphicsPipelineState: initializer's vertex input state is nullptr!", return {});

    VkPipelineVertexInputStateCreateInfo vertex_input_state_create_info{};
    vertex_input_state_create_info.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state_create_info.pNext                           = nullptr;
    vertex_input_state_create_info.flags                           = 0;
    vertex_input_state_create_info.vertexBindingDescriptionCount   = vk_vertex_input_state->GetBindingCount();
    vertex_input_state_create_info.pVertexBindingDescriptions      = vk_vertex_input_state->GetBindings();
    vertex_input_state_create_info.vertexAttributeDescriptionCount = vk_vertex_input_state->GetAttributeCount();
    vertex_input_state_create_info.pVertexAttributeDescriptions    = vk_vertex_input_state->GetAttributes();

    return vertex_input_state_create_info;
}

std::vector<const Shader*> VulkanRHIGraphicsPipelineState::GetShaderInfoList(const RHIShaderBoundStateInput& _shader_bound_state) {
    std::vector<const Shader*> shader_list;
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

VulkanRHITexture::VulkanRHITexture(const RHITextureCreateInfo& _info, VulkanDevice* _device)
    : RHITexture(_info), VulkanDeviceObject(_device) {}

VulkanRHITexture::VulkanRHITexture(const RHITextureCreateInfo& _info, VkImage _image, VulkanDevice* _device)
    : RHITexture(_info),
      VulkanDeviceObject(_device),
      m_alloc(_image, VK_NULL_HANDLE) {}

VulkanRHITexture::~VulkanRHITexture() {
    if (m_alloc.alloc && m_alloc.image != VK_NULL_HANDLE) {
        //todo:
        // vmaDestroyImage(device->Get, m_alloc.image, m_alloc.alloc);
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

#pragma endregion

#pragma region shader param
#pragma endregion

#pragma region synchronization

VulkanRHIFence::VulkanRHIFence(VulkanDevice* _device, EFenceUsage _usage) : m_device(_device), m_binary(VK_NULL_HANDLE), m_semaphore(VK_NULL_HANDLE), usage(_usage) {
    VkSemaphoreCreateInfo create_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (_usage != EFenceUsage::TIMELINE) {
        //do nothing
        VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &m_binary));
    }

    VkSemaphoreTypeCreateInfo timeline_semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_semaphore_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_semaphore_info.initialValue  = 0;

    create_info.pNext = &timeline_semaphore_info;
    VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &m_semaphore));
}

VulkanRHIFence::~VulkanRHIFence() {

    VkSemaphoreWaitInfo wait_delete_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait_delete_info.pNext          = nullptr;
    wait_delete_info.flags          = 0;
    wait_delete_info.semaphoreCount = 1;
    if (m_binary != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device->GetDevice(), m_binary, VK_NULL_HANDLE);
    }
    if (m_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device->GetDevice(), m_semaphore, VK_NULL_HANDLE);
    }
}

uint64_t VulkanRHIFence::GetValue() const {
    uint64_t value;
    vkGetSemaphoreCounterValue(m_device->GetDevice(), m_semaphore, &value);
    return value;
}

void VulkanRHIFence::Wait(uint64_t value) {
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.pSemaphores    = &m_semaphore;
    info.semaphoreCount = 1;
    info.pValues        = &value;
    vkWaitSemaphores(m_device->GetDevice(), &info, UINT64_MAX);
}

#pragma endregion

#pragma region viewable resources view definitions
VulkanRHIUnorderedAccessView::~VulkanRHIUnorderedAccessView() {
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device->GetDevice(), m_view, VK_NULL_HANDLE);
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

    delete swapchain;
    swapchain = nullptr;
}

VulkanRHIUnorderedAccessView* VulkanViewport::InnerCreateVulkanUnorderedAccessView(VulkanDevice* _device, VulkanRHITexture* texture, const RHIViewInfo& _view_info) {
    auto* view = new VulkanRHIUnorderedAccessView(_device, texture, _view_info);

    VkImageViewCreateInfo image_view_create_info{};
    image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_create_info.pNext = nullptr;
    image_view_create_info.flags = 0;

    image_view_create_info.image                           = texture->GetHandle();
    image_view_create_info.viewType                        = VulkanEnumTranslator::METoVKImageViewType(_view_info.texture.srv.dimension);
    image_view_create_info.format                          = _view_info.texture.uav.format == PF_UNDEFINED ? VulkanEnumTranslator::METoVKFormat(texture->GetUAVFormat()) : VulkanEnumTranslator::METoVKFormat(_view_info.texture.uav.format);
    image_view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    image_view_create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;// MARK...
    image_view_create_info.subresourceRange.baseMipLevel   = _view_info.texture.uav.mip_min;
    image_view_create_info.subresourceRange.levelCount     = _view_info.texture.uav.mip_num;
    image_view_create_info.subresourceRange.baseArrayLayer = _view_info.texture.uav.array_min;
    image_view_create_info.subresourceRange.layerCount     = _view_info.texture.uav.array_num;

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
        swapchain_images[index] = new VulkanRHITexture(RHITextureCreateInfo::Create2D("swapchain")
                                                           .SetExtent(swapchain->extent.width, swapchain->extent.height)
                                                           .SetFormat(swapchain_format)
                                                           .SetUAVFormat(swapchain_format)
                                                           .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED),
                                                       swapchain->m_swap_chain_images[index],
                                                       swapchain->m_device);
        swapchain_images[index]->AddRef();

        swapchain_image_uavs[index] = InnerCreateVulkanUnorderedAccessView(
            swapchain->m_device,
            swapchain_images[index],
            RHIViewInfo::CreateTextureUAVInfo()
                .SetArrayRange(0, 1)
                .SetDimension(ETextureDimension::TEX_2D)
                .SetMipLevel(0));
    }
    for (uint32_t index = 0; index < image_aquire_fences.size(); index++) {
        image_aquire_fences[index] = new VulkanRHIFence(swapchain->m_device, EFenceUsage::AQUIRE_NEXT_FRAME);
    }
    frame_offset = 0;

    //init information

    info.backbuffer_format = swapchain_format;
}
void VulkanViewport::InnerDestroyResources() {

    for (uint32_t index = 0; index < swapchain_image_uavs.size(); index++) {
        delete swapchain_image_uavs[index];
    }
    for (uint32_t index = 0; index < image_aquire_fences.size(); index++) {
        VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        VkSemaphore         temp[1] = {image_aquire_fences[index]->GetBinaryHandle()};
        wait_info.semaphoreCount    = 1;
        wait_info.pSemaphores       = temp;
        wait_info.pValues           = 0;
        // vkWaitSemaphores(swapchain->m_device->GetDevice(), &wait_info, UINT64_MAX);
        delete image_aquire_fences[index];
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

        delete swapchain_image_uavs[index];
        swapchain_image_uavs[index] = InnerCreateVulkanUnorderedAccessView(
            swapchain->m_device,
            swapchain_images[index],
            RHIViewInfo::CreateTextureUAVInfo()
                .SetArrayRange(0, 1)
                .SetDimension(ETextureDimension::TEX_2D)
                .SetMipLevel(0));
    }
    for (uint32_t index = 0; index < image_aquire_fences.size(); index++) {
        delete image_aquire_fences[index];
        image_aquire_fences[index] = new VulkanRHIFence(swapchain->m_device, EFenceUsage::AQUIRE_NEXT_FRAME);
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
VulkanRHIUnorderedAccessView* VulkanViewport::GetCurrentBackBuffer(uint32_t index) {
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

#pragma region raytracing
#pragma endregion

#pragma region render query
#pragma endregion

#pragma region RDG resource creater
#pragma endregion
