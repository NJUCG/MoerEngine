//
// Created by 74535 on 2023/10/12.
//

#include "PixelFormat.h"

#include "VulkanCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanPlatform.h"
#include "VulkanRHIResource.h"
#include "VulkanSwapChain.h"
#include <type_traits>

#include "log/LogSystem.h"
#include "misc/Alignment.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "vulkan/vk_enum_string_helper.h"
#include "vulkan/vulkan_core.h"

#if WITH_CUDA
#include "platform/windows/WindowsSecurityAttributes.h"
#include <vulkan/vulkan_win32.h>
#endif

#include <memory>
#include <mutex>
#include <thread>

#pragma region utils definition
namespace Moer::Render {

static constexpr std::string_view s_tlas_underlying_buffer_name = "VkRaytracing::TLASUnderlyingBuffer";
static constexpr std::string_view s_blas_underlying_buffer_name = "VkRaytracing::BLASUnderlyingBuffer";
static constexpr std::string_view s_blas_scratch_buffer_name    = "VkRaytracing::BLASScratchBuffer";
static constexpr std::string_view s_tlas_scratch_buffer_name    = "VkRaytracing::TLASScratchBuffer";
static constexpr std::string_view s_tlas_instance_buffer_name   = "VkRaytracing::TLASInstanceBuffer";
static constexpr std::string_view s_bdls_array_name             = "VkBindless::BindlessArrayBuffer";
static constexpr std::string_view s_bdls_array_buffer_name      = "VkBindless::BindlessBuffuerBuffer";
static constexpr std::string_view s_bdls_array_image_name       = "VkBindless::BindlessImageBuffer";

VmaAllocationCreateFlags VulkanMemoryManager::MEGenerateVmaMemoryFlags(EBufferUsageFlags _flags) {
    if ((_flags & EBufferUsageFlags::CPU_VISIBLE) == EBufferUsageFlags::CPU_VISIBLE)
        return VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
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
    return VkFormat(_format); // MARK...
}

VkImageCreateFlags VulkanEnumTranslator::METoVKImageCreateFlags(ETextureDimension _dim) {
    VkImageCreateFlags flags = 0;
    if (_dim == ETextureDimension::TEX_CUBE || _dim == ETextureDimension::TEX_CUBE_ARRAY) {
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    return flags;
}

VkImageType VulkanEnumTranslator::METoVKImageType(ETextureDimension _dim) {
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

VkImageUsageFlags VulkanEnumTranslator::METoVKImageUsageFlags(ETextureUsageFlags _me_flags) {
    // Always include TRANSFER_SRC since hardware vendors confirmed it wouldn't have any performance cost and we need it for some debug functionalities.
    VkImageUsageFlags vk_flags = 0;

    // NOLINTNEXTLINE
    auto TranslateFlag = [&vk_flags, &_me_flags](
                             ETextureUsageFlags _search_me_flags,
                             VkImageUsageFlags  _added_if_found,
                             VkImageUsageFlags  _added_if_not_found = 0
                         ) {
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
    TranslateFlag(
        ETextureUsageFlags::VIDEO_DECODE,
        VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR |
            VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR
    );
#endif
    TranslateFlag(ETextureUsageFlags::FRAGMENT_DENSITY_MAP, VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT);
    TranslateFlag(
        ETextureUsageFlags::FRAGMENT_SHADING_RATE_ATTACHMENT,
        VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
    );
#ifdef VK_ENABLE_BETA_EXTENSIONS
    TranslateFlag(
        ETextureUsageFlags::VIDEO_ENCODE,
        VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
            VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR
    );
#endif
    // TranslateFlag(ETextureUsageFlags::ATTACHMENT_FEEDBACK_LOOP, VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);

    return vk_flags;
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
        case VK_FORMAT_R8G8_SRGB: // MARK...
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
        default:
            return PF_UNDEFINED;
    }
}

VkBufferUsageFlags VulkanEnumTranslator::METoVKBufferUsageFlags(EBufferUsageFlags _me_flags) {
    // Always include TRANSFER_SRC since hardware vendors confirmed it wouldn't have any performance cost and we need it for some debug functionalities.
    VkBufferUsageFlags vk_flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    // NOLINTNEXTLINE
    auto TranslateFlag = [&vk_flags, &_me_flags](
                             EBufferUsageFlags  _search_me_flags,
                             VkBufferUsageFlags _added_if_found,
                             VkBufferUsageFlags _added_if_not_found = 0
                         ) {
        const bool has_flag = (_me_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    TranslateFlag(
        EBufferUsageFlags::VERTEX_BUFFER,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
    );
    TranslateFlag(
        EBufferUsageFlags::INDEX_BUFFER,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
    );
    TranslateFlag(EBufferUsageFlags::CONSTANT_BUFFER, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    TranslateFlag(
        EBufferUsageFlags::ACCELERATION_STRUCTURE, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
    );
    TranslateFlag(EBufferUsageFlags::SHADER_BINDING_TABLE, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR);

    TranslateFlag(
        EBufferUsageFlags::UNORDERED_ACCESS,
        VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    );
    TranslateFlag(EBufferUsageFlags::INDIRECT_BUFFER, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    TranslateFlag(
        EBufferUsageFlags::CPU_VISIBLE, (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
    );
    TranslateFlag(EBufferUsageFlags::TRANSFER_DST, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    TranslateFlag(EBufferUsageFlags::TEXTURE_BUFFER, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);

    TranslateFlag(EBufferUsageFlags::ACCELERATION_STRUCTURE_SCRATCH, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    TranslateFlag(
        EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    );

    return vk_flags;
}

VkDescriptorType VulkanEnumTranslator::METoVkBufferDescriptorType(EBufferUsageFlags _type) {
    auto has_flag = [&_type](EBufferUsageFlags _search_me_flags) {
        return (_type & _search_me_flags) == _search_me_flags;
    };
    if (has_flag(EBufferUsageFlags::CONSTANT_BUFFER))
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (has_flag(EBufferUsageFlags::UNORDERED_ACCESS))
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (has_flag(EBufferUsageFlags::TEXTURE_BUFFER))
        return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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
            return VK_IMAGE_LAYOUT_VIDEO_ENCODE_DST_KHR | VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR |
                   VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR;
        case ETextureLayout::TEXTURE_LAYOUT_VIDEO_DECODE:
            return VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR | VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR |
                   VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
#endif
        case ETextureLayout::TEXTURE_LAYOUT_READ:
            return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
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

    auto translate_flag = [&vk_flags, &_flags](
                              ERHIPipelineStageFlags _search_me_flags,
                              VkPipelineStageFlags2  _added_if_found,
                              VkPipelineStageFlags2  _added_if_not_found = 0
                          ) {
        const bool has_flag = (_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    using enum ERHIPipelineStageFlags;
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
    auto           translate_flag = [&vk_flags, &_flags](
                              ERHIAccessFlags _search_me_flags,
                              VkAccessFlags2  _added_if_found,
                              VkAccessFlags2  _added_if_not_found = 0
                          ) {
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
        translate_flag(ERHIAccessFlags::TRANSFORM_FEEDBACK_WRITE_BIT_EXT, VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT);
        translate_flag(ERHIAccessFlags::CONDITIONAL_RENDERING_READ_BIT_EXT, VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT);
        return vk_flags;
    }

    VkCullModeFlags VulkanEnumTranslator::METoVKCullModeFlags(ERasterizerCullMode _cull_mode) {
        switch (_cull_mode) {
            case ERasterizerCullMode::RCM_NONE: return VK_CULL_MODE_NONE;
            case ERasterizerCullMode::RCM_FRONT: return VK_CULL_MODE_FRONT_BIT;
            case ERasterizerCullMode::RCM_BACK: return VK_CULL_MODE_BACK_BIT;
            case ERasterizerCullMode::RCM_FRONT_AND_BACK: return VK_CULL_MODE_FRONT_AND_BACK;
            default:
                LOG_CRITICAL("Unsupported rasterizer cull mode: {}", static_cast<uint32_t>(_cull_mode));
                return VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
        }
    }

    VkPrimitiveTopology VulkanEnumTranslator::METoVKPrimitiveTopology(EPrimitiveTopology _primitive_type) {
        switch (_primitive_type) {
            case EPrimitiveTopology::POINT_LIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case EPrimitiveTopology::LINE_LIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case EPrimitiveTopology::LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case EPrimitiveTopology::TRIANGLE_LIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case EPrimitiveTopology::TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case EPrimitiveTopology::TRIANGLE_LIST_WITH_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
            case EPrimitiveTopology::TRIANGLE_STRIP_WITH_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
            case EPrimitiveTopology::PATCH_LIST: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
            default:
                LOG_CRITICAL("Unsupported primitive topology: {}", static_cast<uint32_t>(_primitive_type));
                return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        }
    }

    VkPolygonMode VulkanEnumTranslator::METoVKPolygonMode(ERasterizerFillMode _fill_mode) {
        switch (_fill_mode) {
            case ERasterizerFillMode::FM_FILL: return VK_POLYGON_MODE_FILL;
            case ERasterizerFillMode::FM_LINE: return VK_POLYGON_MODE_LINE;
            case ERasterizerFillMode::FM_POINT: return VK_POLYGON_MODE_POINT;
            case ERasterizerFillMode::FM_FILL_RECTANGLE_NV: return VK_POLYGON_MODE_FILL_RECTANGLE_NV;
            default:
                LOG_CRITICAL("Unsupported rasterizer fill mode: {}", static_cast<uint32_t>(_fill_mode));
                return VK_POLYGON_MODE_MAX_ENUM;
        }
    }

    VkBlendOp VulkanEnumTranslator::METoVKBlendOp(EBlendOperation _blend_op) {
        switch (_blend_op) {
            case BO_ADD: return VK_BLEND_OP_ADD;
            case BO_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
            case BO_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case BO_MIN: return VK_BLEND_OP_MIN;
            case BO_MAX: return VK_BLEND_OP_MAX;
            default:
                LOG_CRITICAL("Unsupported color blend operation: {}", static_cast<uint32_t>(_blend_op));
                return VK_BLEND_OP_MAX_ENUM;
        }
    }

    VkBlendFactor VulkanEnumTranslator::METoVKBlendFactor(EBlendFactor _blend_factor) {
        switch (_blend_factor) {
            case BF_ZERO: return VK_BLEND_FACTOR_ZERO;
            case BF_ONE: return VK_BLEND_FACTOR_ONE;
            case BF_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
            case BF_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case BF_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
            case BF_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case BF_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
            case BF_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case BF_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
            case BF_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case BF_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case BF_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            case BF_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
            case BF_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
            case BF_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
            case BF_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
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
        auto is_acceleration_structure = [](EShaderCodeResourceBindingType type) { return type == EShaderCodeResourceBindingType::RAYTRACING_ACCELERATION_STRUCTURE; };
        switch (_type) {
            case EShaderParameterType::CBV: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case EShaderParameterType::SAMPLER:
            case EShaderParameterType::BINDLESS_SAMPLER_INDEX: // MARK...
                return VK_DESCRIPTOR_TYPE_SAMPLER;
            case EShaderParameterType::SRV: if (is_texture(_binding_type)) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                if (is_buffer(_binding_type)) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                if (is_acceleration_structure(_binding_type)) return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                LOG_ERROR("Unsupported SRV type: {}", ToString(_binding_type));
            case EShaderParameterType::UAV: if (is_texture(_binding_type)) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (is_buffer(_binding_type)) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case EShaderParameterType::BINDLESS_RESOURCE_INDEX: // MARK...
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            default:
                LOG_CRITICAL("Unsupported EShaderParameterType: {}", static_cast<uint32_t>(_type));
                return VK_DESCRIPTOR_TYPE_MAX_ENUM;
        }
    }

    VkShaderStageFlags VulkanEnumTranslator::METoVKShaderStageFlags(EShaderType _type) {
        switch (_type) {
            case EShaderType::ST_VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
            case EShaderType::ST_FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
            case EShaderType::ST_GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
            case EShaderType::ST_COMPUTE: return VK_SHADER_STAGE_COMPUTE_BIT;
            case EShaderType::ST_MESH: return VK_SHADER_STAGE_MESH_BIT_NV;
            case EShaderType::ST_AMPLIFICATION: return VK_SHADER_STAGE_TASK_BIT_NV;
            case EShaderType::ST_RAY_GEN: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            case EShaderType::ST_RAY_MISS: return VK_SHADER_STAGE_MISS_BIT_KHR;
            case EShaderType::ST_RAY_CLOSESTHIT: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            case EShaderType::ST_RAY_CALLABLE: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
            case EShaderType::ST_RAY_INTERSECTION: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            case EShaderType::ST_RAY_ANYHIT: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            default:
                LOG_CRITICAL("Unsupported EShaderType: {}", static_cast<uint32_t>(_type));
                return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
        }
    }

    uint32_t VulkanEnumTranslator::METoVkQueueFamilyIndex(ECommandQueueType _type, const VulkanDevice* _device) {
        switch (_type) {
            case ECommandQueueType::GRAPHICS: return _device->GetQueueFamilyIndices().graphics.value();
            case ECommandQueueType::COMPUTE: return _device->GetQueueFamilyIndices().compute.value();
            case ECommandQueueType::COPY: return _device->GetQueueFamilyIndices().transfer.value();
            case ECommandQueueType::RAYTRACING: return _device->GetQueueFamilyIndices().raytracing.value();
            default: return VK_QUEUE_FAMILY_IGNORED;
        }
    }

    uint32_t VulkanEnumTranslator::METoVkQueueFamilyIndex(ECommandListType _type, const VulkanDevice* _device) {
        switch (_type) {
            case ECommandListType::GRAPHICS: return _device->GetQueueFamilyIndices().graphics.value();
            case ECommandListType::COMPUTE: return _device->GetQueueFamilyIndices().compute.value();
            case ECommandListType::COPY: return _device->GetQueueFamilyIndices().transfer.value();
            case ECommandListType::RAY_TRACING: return _device->GetQueueFamilyIndices().raytracing.value();
            default: return _device->GetQueueFamilyIndices().graphics.value();
        }
    }

    VkFilter VulkanEnumTranslator::METoVKMinMagFilterMode(ESamplerFilter _filter) { return VulkanEnumTranslator::METoVKImageFilter(_filter); }

    VkSamplerMipmapMode VulkanEnumTranslator::METoVKMipmapMode(ESamplerFilter _filter) {
        switch (_filter) {
            case SF_NEAREST:
            case SF_LINEAR: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
            case SF_CUBIC: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
            case SF_ANISOTROPIC_NEAREST: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
            case SF_ANISOTROPIC_LINEAR: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
            default:
                LOG_CRITICAL("Unknown Mipmap ESamplerFilter {:d}", static_cast<uint8_t>(_filter));
                return VK_SAMPLER_MIPMAP_MODE_MAX_ENUM;
        }
    }

    VkSamplerAddressMode VulkanEnumTranslator::METoVKWrapMode(ESamplerAddressMode _address_mode) {
        switch (_address_mode) {
            case SAM_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case SAM_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case SAM_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case SAM_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            default:
                LOG_CRITICAL("Unknown ESamplerAddressMode {:d}", static_cast<uint8_t>(_address_mode));
                return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
        }
    }

#pragma endregion

    VkVertexInputRate VulkanEnumTranslator::METoVKVertexInputRate(EVertexInputRate _me_rate) {
        switch (_me_rate) {
            case EVertexInputRate::VIR_VERTEX: return VK_VERTEX_INPUT_RATE_VERTEX;
            case EVertexInputRate::VIR_INSTANCE: return VK_VERTEX_INPUT_RATE_INSTANCE;
            default:
                LOG_CRITICAL("Unsupported vertex input rate: {}", static_cast<uint32_t>(_me_rate));
                return VK_VERTEX_INPUT_RATE_MAX_ENUM;
        }
    }

    VkQueueFlagBits VulkanEnumTranslator::METoVKQueueFlagBits(EQueueType _queue_type) {
        switch (_queue_type) {
            case EQueueType::Graphics: return VK_QUEUE_GRAPHICS_BIT;
            case EQueueType::Compute: return VK_QUEUE_COMPUTE_BIT;
            case EQueueType::Copy: return VK_QUEUE_TRANSFER_BIT;
            default:
                LOG_CRITICAL("Unsupported queue type: {}", static_cast<uint32_t>(_queue_type));
                return VK_QUEUE_FLAG_BITS_MAX_ENUM;
        }
    }

    VkCompareOp VulkanEnumTranslator::METoVKCompareOp(ECompareOption _compare_op) {
        switch (_compare_op) {
            case ECompareOption::CO_NEVER: return VK_COMPARE_OP_NEVER;
            case ECompareOption::CO_LESS: return VK_COMPARE_OP_LESS;
            case ECompareOption::CO_EQUAL: return VK_COMPARE_OP_EQUAL;
            case ECompareOption::CO_LESS_OR_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case ECompareOption::CO_GREATER: return VK_COMPARE_OP_GREATER;
            case ECompareOption::CO_NOT_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
            case ECompareOption::CO_GREATER_OR_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case ECompareOption::CO_ALWAYS: return VK_COMPARE_OP_ALWAYS;
            default:
                LOG_CRITICAL("Unsupported depth stencil compare option: {}", static_cast<uint32_t>(_compare_op));
                return VK_COMPARE_OP_MAX_ENUM;
        }
    }

    VkStencilOp VulkanEnumTranslator::METoVKStencilOp(EStencilOp _stencil_op) {
        switch (_stencil_op) {
            case SO_KEEP: return VK_STENCIL_OP_KEEP;
            case SO_ZERO: return VK_STENCIL_OP_ZERO;
            case SO_REPLACE: return VK_STENCIL_OP_REPLACE;
            case SO_INCREMENT_AND_CLAMP: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case SO_DECREMENT_AND_CLAMP: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case SO_INVERT: return VK_STENCIL_OP_INVERT;
            case SO_INCREMENT_AND_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case SO_DECREMENT_AND_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            default:
                LOG_CRITICAL("Unsupported depth stencil operation: {}", static_cast<uint32_t>(_stencil_op));
                return VK_STENCIL_OP_MAX_ENUM;
        };
    }

    VkGeometryFlagsKHR VulkanEnumTranslator::METoVKGeometryFlags(ERayTracingGeometryFlags _flags) {
        VkGeometryFlagsKHR vk_flags = 0;

        auto translate_flag = [&vk_flags, &_flags](ERayTracingGeometryFlags _search_me_flags, VkGeometryFlagsKHR _added_if_found, VkGeometryFlagsKHR _added_if_not_found = 0) {
            const bool has_flag = (_flags & _search_me_flags) == _search_me_flags;
            vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
        };

        translate_flag(ERayTracingGeometryFlags::GEOMETRY_OPAQUE, VK_GEOMETRY_OPAQUE_BIT_KHR);
        translate_flag(ERayTracingGeometryFlags::NO_DUPLICATE_ANY_HIT_INVOCATION, VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR);
        return vk_flags;
    }

    VkGeometryTypeKHR VulkanEnumTranslator::METoVKGeometryType(ERayTracingGeometryType _type) {
        switch (_type) {
            case RTGT_TRIANGLES: return VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            case RTGT_AABBS: return VK_GEOMETRY_TYPE_AABBS_KHR;
            default:
                LOG_CRITICAL("Unsupported ray tracing geometry type: {}", static_cast<uint32_t>(_type));
                return VK_GEOMETRY_TYPE_MAX_ENUM_KHR;
        }
    }

    VkBuildAccelerationStructureFlagsKHR VulkanEnumTranslator::METoVKAccelerationStructureBuildType(ERayTracingAccelerationStructureBuildFlags _flags) {
        VkBuildAccelerationStructureFlagsKHR vk_flags = 0;

        auto translate_flag = [&vk_flags, &_flags](ERayTracingAccelerationStructureBuildFlags _search_me_flags, VkBuildAccelerationStructureFlagsKHR _added_if_found, VkBuildAccelerationStructureFlagsKHR _added_if_not_found = 0) {
            const bool has_flag = (_flags & _search_me_flags) == _search_me_flags;
            vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
        };

        translate_flag(ERayTracingAccelerationStructureBuildFlags::ALLOW_UPDATE, VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR);
        translate_flag(ERayTracingAccelerationStructureBuildFlags::ALLOW_COMPACTION, VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
        translate_flag(ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
        translate_flag(ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_BUILD, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR);
        translate_flag(ERayTracingAccelerationStructureBuildFlags::MINIMIZE_MEMORY, VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR);
        return vk_flags;
    }

    VkBuildAccelerationStructureModeKHR VulkanEnumTranslator::METoVKBuildAccelerationStructureMode(ERaytracingBuildMode _mode){
        switch (_mode) {
            case ERaytracingBuildMode::BUILD:
            return VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            case ERaytracingBuildMode::UPDATE:
            return VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        }
        assert(false && "unsupported build mode");
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
        for (auto& layout : descriptor_set_layouts) {
            if (layout && layout != m_device->GetEmptyDescriptorSetLayout()) {
                vkDestroyDescriptorSetLayout(m_device->GetDevice(), layout, nullptr);
                layout = VK_NULL_HANDLE;
            }
        }
        CHECK_AND_DELETE(m_descriptor_sets_layout);
    }

    void VulkanPipelineState::InitDescriptorSetLayouts(Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings) {
        if (_descriptor_bindings.empty()) { return; }

        m_descriptor_sets_layout = MoerNew(VulkanDescriptorSetsLayout)(m_device, _descriptor_bindings);
    }

    void VulkanPipelineState::CreatePipelineLayout(const VkPipelineLayoutCreateInfo& _pipeline_layout_ci) { VK_CHECK_RESULT(vkCreatePipelineLayout(m_device->GetDevice(), &_pipeline_layout_ci, nullptr, &m_pipeline_layout)); }

    void VulkanPipelineState::InitPipelineLayout(UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>&& _descriptor_set_layouts, std::optional<VkPushConstantRange> _push_constant_range){
        VkPipelineLayoutCreateInfo pipeline_layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

        bind_template = MakeUnique<VulkanPipelineParamBinder>();
        bind_template->set_binders.rehash(_descriptor_set_layouts.size());
        Array<VkDescriptorBufferBindingInfoEXT>& descriptor_buffers = bind_template->desc_buffers;
        Array<DescBufferOffsetInfo>& desc_buffer_offsets = bind_template->desc_buffer_offsets;

        // VkDescriptorSetLayoutCreateInfo descriptor_set_layout_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        Array<VkDescriptorSetLayoutBinding> descriptor_set_layout_bindings;
        uint total_binding_count = 0;
        uint descriptor_buffer_count = 0;
        constexpr static uint invalid_descriptor_buffer_idx = 114514;
        uint buffer_descriptor_buffer_idx = invalid_descriptor_buffer_idx;
        uint sampler_descriptor_buffer_idx = invalid_descriptor_buffer_idx;
        uint global_descriptor_buffer_idx = invalid_descriptor_buffer_idx;

        VkDescriptorSetLayout empty_layout = m_device->GetEmptyDescriptorSetLayout();

        //precompute array sizes
        for (auto& [set, layout] : _descriptor_set_layouts) {
            total_binding_count += layout.bindings.size();
            auto& binder = bind_template->set_binders[set];

            if(layout.is_bindless){
                if(!layout.bindings.empty() && layout[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){
                    if(buffer_descriptor_buffer_idx == invalid_descriptor_buffer_idx){
                        descriptor_buffer_count++;
                        buffer_descriptor_buffer_idx = descriptor_buffer_count - 1;
                    }
                    binder.emplace<VulkanBindlessSetArray>(layout.bindings.at(0).param_idx, buffer_descriptor_buffer_idx);
                }else if(!layout.bindings.empty() && (layout[0].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)){
                    if (sampler_descriptor_buffer_idx == invalid_descriptor_buffer_idx) {
                        descriptor_buffer_count++;
                        sampler_descriptor_buffer_idx = descriptor_buffer_count - 1;
                    }

                    binder.emplace<VulkanBindlessSetSampler>(layout.bindings.at(0).param_idx, sampler_descriptor_buffer_idx);
                }else if (!layout.bindings.empty() && (layout[0].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)){
                    //sampler and sampled_image use same descriptor buffer
                    if (sampler_descriptor_buffer_idx == invalid_descriptor_buffer_idx) {
                        descriptor_buffer_count++;
                        sampler_descriptor_buffer_idx = descriptor_buffer_count - 1;
                    }
                    binder.emplace<VulkanBindlessSetImage>(layout.bindings.at(0).param_idx, sampler_descriptor_buffer_idx);
                }
                else{
                    LOG_CRITICAL("Unsupported bindless descriptor type: {}", uint(layout[0].descriptorType));
                    assert(false);
                }
                continue;
            }
            //not bindless
            if(global_descriptor_buffer_idx == invalid_descriptor_buffer_idx){
                descriptor_buffer_count++;
                global_descriptor_buffer_idx = descriptor_buffer_count - 1;
            }
            VulkanDescriptorSetBinder& resource_binder = std::get<VulkanDescriptorSetBinder>(binder);
            resource_binder.bind_infos.resize(layout.bindings.size());
            resource_binder.writers.resize(layout.bindings.size());
            resource_binder.binding_infos.resize(layout.bindings.size());
        }
        descriptor_buffers.resize(descriptor_buffer_count);
        desc_buffer_offsets.reserve(descriptor_buffer_count);

        //get max set index
        uint max_set_idx = 0;
        for (const auto& [set, layout] : _descriptor_set_layouts) {
            max_set_idx = std::max(max_set_idx, set);
        }
        if (_descriptor_set_layouts.empty()) {
            max_set_idx = 0;
        }
        pipeline_layout_ci.setLayoutCount         = _descriptor_set_layouts.empty() ? 0 : max_set_idx + 1;
        descriptor_set_layouts.resize(pipeline_layout_ci.setLayoutCount, VK_NULL_HANDLE);
        descriptor_set_layout_bindings.resize(total_binding_count);
        total_binding_count = 0;

        static constexpr VkDescriptorBindingFlags bdls_flags [] = {0,
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
        };

        static constexpr VkDescriptorBindingFlags bdls_sampler_flags [] = {
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
        };


        //build descriptor set layouts
        for (const auto& [set, layout] : _descriptor_set_layouts) {
            VkDescriptorSetLayoutCreateInfo descriptor_set_layout_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

            auto& binder = bind_template->set_binders[set];
            descriptor_set_layout_ci = layout.layout_create_info;
            descriptor_set_layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
            VkDescriptorSetLayoutBindingFlagsCreateInfo bdls_buffer_ext{};
            bdls_buffer_ext.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
            bdls_buffer_ext.bindingCount = 1;
            bdls_buffer_ext.pBindingFlags = bdls_flags;

            VkDescriptorSetLayoutBindingFlagsCreateInfo bdls_texture_ext{};
            bdls_texture_ext.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
            bdls_texture_ext.bindingCount = 1;
            bdls_texture_ext.pBindingFlags = bdls_sampler_flags;
            std::visit([&](auto&& _binder){
                using T = std::decay_t<decltype(_binder)>;
                if constexpr(std::is_same_v<T, VulkanBindlessSetArray>){
                    descriptor_set_layout_ci.pNext = &bdls_buffer_ext;
                    for (const auto& [binding_idx, m_binding] : layout.bindings) {
                        const auto& binding = m_binding.binding;
                        descriptor_set_layout_bindings[total_binding_count + binding.binding] = binding;
                        if(binding_idx == 1){bdls_buffer_ext.bindingCount = 2;}
                    }
                    //need to calculate a layout
                }else if constexpr(std::is_same_v<T, VulkanBindlessSetImage>){
                    descriptor_set_layout_ci.pNext = &bdls_texture_ext;
                    for (const auto& [binding_idx, m_binding] : layout.bindings) {
                        const auto& binding = m_binding.binding;
                        descriptor_set_layout_bindings[total_binding_count + binding.binding] = binding;
                    }

                }else if constexpr(std::is_same_v<T, VulkanBindlessSetSampler>){
                        descriptor_set_layout_ci.pNext = &bdls_texture_ext;
                    for (const auto& [binding_idx, m_binding] : layout.bindings) {
                        const auto& binding = m_binding.binding;
                        descriptor_set_layout_bindings[total_binding_count + binding.binding] = binding;
                    }
                }else if constexpr(std::is_same_v<T, VulkanDescriptorSetBinder>){
                    for (const auto& [binding_idx, m_binding] : layout.bindings) {
                        const auto& binding = m_binding.binding;
                        descriptor_set_layout_bindings[total_binding_count + binding.binding] = binding;
                        VkWriteDescriptorSet& write_info = _binder.writers[binding.binding];
                        write_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        write_info.dstSet = VK_NULL_HANDLE;//ignored
                        write_info.dstBinding = binding.binding;
                        write_info.dstArrayElement = 0;
                        write_info.descriptorCount = binding.descriptorCount;
                        write_info.descriptorType = binding.descriptorType;

                        VulkanDescriptorInfo& descriptor_info = _binder.bind_infos[binding.binding];
                        descriptor_info.param_idx = m_binding.param_idx;
                        if(binding.descriptorCount < 1){
                            continue;
                        }
                        switch (binding.descriptorType){

                            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:{
                                descriptor_info.info_idx = _binder.buffer_infos.size();
                                _binder.buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                                write_info.pBufferInfo = &_binder.buffer_infos.back();
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_SAMPLER:{
                                descriptor_info.info_idx = _binder.image_infos.size();
                                _binder.image_infos.emplace_back(VK_NULL_HANDLE,
                                    VK_NULL_HANDLE,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                write_info.pImageInfo = &_binder.image_infos.back();
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:{
                                descriptor_info.info_idx = _binder.image_infos.size();
                                _binder.image_infos.emplace_back(VK_NULL_HANDLE,
                                    VK_NULL_HANDLE,
                                    binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ?
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                                        VK_IMAGE_LAYOUT_GENERAL);
                                write_info.pImageInfo = &_binder.image_infos.back();
                                break;
                            }
                            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:{
                                descriptor_info.info_idx = _binder.accel_structures.size();
                                _binder.accel_structures.emplace_back();
                                write_info.pNext = &_binder.accel_structures.back();
                                break;

                            }
                            default:
                                {
                                    LOG_CRITICAL("Unsupported descriptor type: {}", uint(binding.descriptorType));
                                    assert(false);
                                }
                        }
                        _binder.push_info.stageFlags |= binding.stageFlags;

                    }
                    _binder.bind_point = GetPipelineBindPoint();
                }

            }, binder);

            descriptor_set_layout_ci.bindingCount = layout.bindings.size();
            descriptor_set_layout_ci.pBindings = descriptor_set_layout_bindings.data() + total_binding_count;
            VK_CHECK_RESULT(vkCreateDescriptorSetLayout(m_device->GetDevice(), &descriptor_set_layout_ci, VK_NULL_HANDLE, &descriptor_set_layouts[set]));
            total_binding_count += layout.bindings.size();
            std::visit([&](auto& _binder){
                using T = std::decay_t<decltype(_binder)>;
                if constexpr(std::is_same_v<T, VulkanDescriptorSetBinder>){
                    for (const auto& [binding_idx, m_binding] : layout.bindings) {
                        const auto& binding = m_binding.binding;
                        auto& binding_info = _binder.binding_infos[binding.binding];
                        vkGetDescriptorSetLayoutBindingOffsetEXT(m_device->GetDevice(), descriptor_set_layouts[set], binding.binding, &binding_info.offset);
                        binding_info.binding = binding.binding;
                    }
                    vkGetDescriptorSetLayoutSizeEXT(m_device->GetDevice(), descriptor_set_layouts[set], &_binder.size);
                    //align to 16 bytes
                    uint64 align = m_device->GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment;
                    _binder.size = Moer::AlignUp(_binder.size, align);
                }
            }, binder);

        }
        for (auto& layout : descriptor_set_layouts) {
            if (layout == VK_NULL_HANDLE) {
                layout = empty_layout;
            }
        }
        pipeline_layout_ci.pSetLayouts = descriptor_set_layouts.data();
        pipeline_layout_ci.pushConstantRangeCount = _push_constant_range.has_value() ? 1 : 0;
        pipeline_layout_ci.pPushConstantRanges = _push_constant_range.has_value() ? &_push_constant_range.value() : nullptr;
        VK_CHECK_RESULT(vkCreatePipelineLayout(m_device->GetDevice(), &pipeline_layout_ci, nullptr, &m_pipeline_layout));

        //post build pipeline layout
        for (auto& [set, binder] : bind_template->set_binders) {
            std::visit([&](auto&& _binder){
                using T = std::decay_t<decltype(_binder)>;
                if constexpr(std::is_same_v<T, VulkanBindlessSetArray>){
                    descriptor_buffers[buffer_descriptor_buffer_idx] = {
                        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
                        VK_NULL_HANDLE,
                        0ull,
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT
                    };
                    desc_buffer_offsets.emplace_back(
                         set,
                        GetPipelineBindPoint(),
                        m_pipeline_layout,
                        buffer_descriptor_buffer_idx,
                        0);
                }else if constexpr(std::is_same_v<T, VulkanBindlessSetImage>){
                    descriptor_buffers[sampler_descriptor_buffer_idx] = {
                        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
                        VK_NULL_HANDLE,
                        0ull,
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
                    };
                    desc_buffer_offsets.emplace_back(
                        set,
                        GetPipelineBindPoint(),
                        m_pipeline_layout,
                        sampler_descriptor_buffer_idx,
                        m_device->GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize * 256);

                }else if constexpr(std::is_same_v<T, VulkanBindlessSetSampler>){
                    descriptor_buffers[sampler_descriptor_buffer_idx] = {
                        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
                        VK_NULL_HANDLE,
                        0ull,
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
                    };

                    desc_buffer_offsets.emplace_back(
                        set,
                        GetPipelineBindPoint(),
                        m_pipeline_layout,
                        sampler_descriptor_buffer_idx,
                        0);
                }else if constexpr(std::is_same_v<T, VulkanDescriptorSetBinder>){
                    descriptor_buffers[global_descriptor_buffer_idx] = {
                        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
                        VK_NULL_HANDLE,
                        m_device->GetGlobalDescriptorHeap().ring_desc_buffer->DeviceAddress(),
                        VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                    };

                    _binder.push_info.pDescriptorWrites = _binder.writers.data();
                    _binder.push_info.descriptorWriteCount = _binder.writers.size();
                    _binder.push_info.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO_KHR;
                    _binder.push_info.pNext = nullptr;
                    _binder.push_info.layout = m_pipeline_layout;
                    _binder.push_info.set = set;

                    _binder.desc_idx = global_descriptor_buffer_idx;
                    _binder.offset_idx = desc_buffer_offsets.size();

                    desc_buffer_offsets.emplace_back(
                        set,
                        GetPipelineBindPoint(),
                        m_pipeline_layout,
                        global_descriptor_buffer_idx,
                        0);
                }
            }, binder);

        }
        if(_push_constant_range.has_value()){
            bind_template->push_constants_info.sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO_KHR;
            bind_template->push_constants_info.pNext = nullptr;
            bind_template->push_constants_info.layout = m_pipeline_layout;
            bind_template->push_constants_info.stageFlags = _push_constant_range->stageFlags;
            bind_template->push_constants_info.offset = _push_constant_range->offset;
            bind_template->push_constants_info.size = _push_constant_range->size;

        }
        for (auto& layout : descriptor_set_layouts) {
            if (layout != empty_layout) {
                // vkDestroyDescriptorSetLayout(m_device->GetDevice(), layout, nullptr);
            }
        }

    }


#pragma endregion

    VulkanDeviceObject::VulkanDeviceObject(VulkanDevice* _device) : m_device(_device) {}

#pragma region global buffer definitions
#pragma endregion

#pragma region viewable resources definitions

    VulkanTexture::VkImageViewKey
    EncodeViewKey(uint _mip_level, uint _mip_cnt, uint _base_layer, uint _layer_cnt, ETextureAspectFlags _aspect) {
        return VulkanTexture::VkImageViewKey{
            uint16(_mip_level),
            uint16(_mip_cnt),
            uint16(_base_layer),
            uint16(_layer_cnt),
            _aspect
        };
    }


    VkImageTiling GetVkImageTilling(VkImageCreateInfo _create_infos, VulkanDevice& _device){
        //query device property
        VkImageFormatProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        VkPhysicalDeviceImageFormatInfo2 format_info{};
        format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        format_info.format = _create_infos.format;
        format_info.type = _create_infos.imageType;
        format_info.tiling = _create_infos.tiling;
        format_info.usage = _create_infos.usage;
        format_info.flags = _create_infos.flags;
        properties2.pNext = VK_NULL_HANDLE;
        VkResult result = vkGetPhysicalDeviceImageFormatProperties2(_device.GetGpu(), &format_info, &properties2);
        if(result != VK_SUCCESS){
            // LOG_CRITICAL("Failed to get image format properties");
            return VK_IMAGE_TILING_LINEAR;
        }
        return format_info.tiling;
    }
    VulkanTexture::VulkanTexture(const TextureInfo& _info, VulkanDevice* _device, VkImage _image)
        : Texture(_info), VulkanDeviceObject(_device) {
        b_present = (_info.usage & ETextureUsageFlags::PRESENT) == ETextureUsageFlags::PRESENT;
        bool b_prefer_sample = (_info.usage & ETextureUsageFlags::SAMPLED) == ETextureUsageFlags::SAMPLED && (_info.usage & ETextureUsageFlags::UNORDERED_ACCESS) == ETextureUsageFlags::UNDEFINED;
        m_preferred_layout = b_prefer_sample ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;

        if (_image != VK_NULL_HANDLE) {
            m_alloc.image = _image;
            m_alloc.alloc = {};
            return;
        }
        // 验证 texture 参数
        if (_info.extent.x == 0 || _info.extent.y == 0) {
            LOG_ERROR(
                "Invalid texture extent: ({}, {}). Texture name: {}",
                _info.extent.x,
                _info.extent.y,
                _info.debug_name.has_value() ? _info.debug_name->c_str() : "Unknown"
            );
            assert(false && "Texture extent cannot be zero");
        }

        VkImageCreateInfo image_create_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_create_info.flags       = VulkanEnumTranslator::METoVKImageCreateFlags(_info.dimension);
        image_create_info.imageType   = VulkanEnumTranslator::METoVKImageType(_info.dimension);
        image_create_info.format      = VulkanEnumTranslator::METoVKFormat(_info.format);
        image_create_info.extent      = {uint(_info.extent.x), uint(_info.extent.y), _info.depth};
        image_create_info.mipLevels   = _info.num_mips;
        image_create_info.arrayLayers = _info.array_size;
        image_create_info.samples     = VulkanEnumTranslator::METoVKSampleCountFlagBits(_info.num_samples);
        image_create_info.tiling      = VK_IMAGE_TILING_OPTIMAL;
        image_create_info.usage       = VulkanEnumTranslator::METoVKImageUsageFlags(_info.usage) |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_create_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

#if WITH_CUDA
        image_create_info.pNext = GetExternalMemoryImageCreateInfoPtr(nullptr);
#endif

        auto tilling = GetVkImageTilling(image_create_info, *_device);
        image_create_info.tiling = tilling;
        VmaAllocationCreateInfo alloc_create_info{};
        alloc_create_info.flags = 0;
        alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

        VmaAllocator allocator = m_device->GetVmaAllocator();
        VkResult     result    = vmaCreateImage(allocator, &image_create_info, &alloc_create_info, &m_alloc.image, &m_alloc.alloc, nullptr);
        if (result != VK_SUCCESS) {
            std::string texture_name = _info.debug_name.has_value() ? _info.debug_name->c_str() : "Unknown";
            LOG_CRITICAL(
                "Failed to create texture '{}': extent=({},{}), format={}, mips={}, layers={}, "
                "VkResult={}",
                texture_name,
                _info.extent.x,
                _info.extent.y,
                uint(_info.format),
                _info.num_mips,
                _info.array_size,
                int(result)
            );
            VK_CHECK_RESULT(result);
        }
        if(!_info.debug_name.has_value()){
            SetName("UserTexture");
        }else{
            SetName(_info.debug_name->c_str());
        }
    }

    VkImageView VulkanTexture::GetView(
        uint _mip_level,
        uint _mip_cnt,
        uint _base_layer,
        uint _layer_cnt,
        ETextureAspectFlags _aspect_override
    ) {
        ETextureAspectFlags aspect = _aspect_override == ETextureAspectFlags::NONE ? GetAspectFlags() :
                                                                                     _aspect_override;
        auto key = EncodeViewKey(_mip_level, _mip_cnt, _base_layer, _layer_cnt, aspect);
        auto it  = m_views.find(key);
        if (it != m_views.end()) {
            return it->second;
        }
        VkImageView           view;
        VkImageViewCreateInfo view_create_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_create_info.image                           = m_alloc.image;

        // 如果是 CubeMap 但只请求了 1 层，必须降级为 2D View
        if (GetDimension() == ETextureDimension::TEX_CUBE && _layer_cnt == 1) {
            view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        } else { 
            view_create_info.viewType = VulkanEnumTranslator::METoVKImageViewType(GetDimension());
        }

        view_create_info.format                          = VulkanEnumTranslator::METoVKFormat(GetFormat());
        view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        view_create_info.subresourceRange.aspectMask     = VulkanEnumTranslator::METoVKImageAspectFlags(aspect);
        view_create_info.subresourceRange.baseMipLevel   = _mip_level;
        view_create_info.subresourceRange.levelCount     = _mip_cnt;// 含义：这次我想看多少，而不是纹理总共有多少
        view_create_info.subresourceRange.baseArrayLayer = _base_layer;
        view_create_info.subresourceRange.layerCount     = _layer_cnt;// 含义：这次我想看多少，而不是纹理总共有多少
        view_create_info.pNext                           = nullptr;
        view_create_info.flags                           = 0;

        VK_CHECK_RESULT(vkCreateImageView(m_device->GetDevice(), &view_create_info, nullptr, &view));
        m_views[key] = view;
        return view;
    }

    uint VulkanTexture::GetMipByteSize(uint _mip_level) const {
        uint mip_width  = std::max(1u, GetWidth() >> _mip_level);
        uint mip_height = std::max(1u, GetHeight() >> _mip_level);
        uint mip_depth  = std::max(1u, GetDepth() >> _mip_level);
        return mip_width * mip_height * mip_depth * g_platform_pixel_formats[uint(GetFormat())].stride;
    }

    VulkanBuffer::~VulkanBuffer() {
        if (m_alloc.buffer != VK_NULL_HANDLE && m_alloc.alloc != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_device->GetVmaAllocator(), m_alloc.buffer, m_alloc.alloc);
        }
        // if (m_descriptor_idx >= 0) { m_device->GetGlobalDescriptorHeap().FreeBufferDescIdx(m_descriptor_idx); }
        // for (const auto& idx : m_descriptor_indices) { m_device->GetGlobalDescriptorHeap().FreeBufferDescIdx(idx.second); }

        for (uint i = 0; i < 4; ++i) {
            for (const auto& idx : m_descriptor_indices[i]) { m_device->GetGlobalDescriptorHeap().FreeBufferDescIdx(idx.second); }
        }
    }

    VulkanBuffer::VulkanBuffer(std::string_view _name,const BufferInfo& _info, VulkanDevice& _device): Buffer(_info), VulkanDeviceObject(&_device) {
        VkBufferCreateInfo buffer_create_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_create_info.size  = _info.size * _info.stride;
        buffer_create_info.usage = VulkanEnumTranslator::METoVKBufferUsageFlags(_info.usage) |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        buffer_create_info.flags       = 0;

#if WITH_CUDA
        buffer_create_info.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

        VmaAllocationCreateInfo alloc_create_info{};
        alloc_create_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT ;
        alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

        VmaAllocator allocator = m_device->GetVmaAllocator();
        auto result = vmaCreateBuffer(allocator, &buffer_create_info, &alloc_create_info, &m_alloc.buffer, &m_alloc.alloc, nullptr);
        if (result == VkResult::VK_ERROR_INITIALIZATION_FAILED) {
            LOG_CRITICAL("Failed to create buffer, buffer size: {} = {} * {}", buffer_create_info.size, _info.size, _info.stride);
        } else {
            VK_CHECK_RESULT(result);
        }

        //get device address
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer      = m_alloc.buffer;
        m_device_address = vkGetBufferDeviceAddress(m_device->GetDevice(), &info);
        m_descriptor_type = VulkanEnumTranslator::METoVkBufferDescriptorType(_info.usage);
        SetName(_name);
    }

    uint64 VulkanBuffer::DeviceAddress() const { return m_device_address; }

    VulkanBuffer::VulkanBuffer(std::string_view _name, const BufferInfo& _info, VulkanDevice& _device, VkBuffer _handle, VmaAllocation _alloc, bool _defer_destroy, bool _get_address): Buffer(_info), VulkanDeviceObject(&_device) {
        m_alloc.buffer    = _handle;
        m_alloc.alloc     = _alloc;
        b_deferred_delete = _defer_destroy;
        m_descriptor_type = VulkanEnumTranslator::METoVkBufferDescriptorType(_info.usage);
        if(_get_address){
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer      = m_alloc.buffer;
            m_device_address = vkGetBufferDeviceAddress(m_device->GetDevice(), &info);
        }
        SetName(_name);      
    }

    UnorderedMap<uint64, uint>& VulkanBuffer::GetDescriptorIndices(VkDescriptorType _type) { 
        switch (_type) {
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return m_descriptor_indices[0];
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return m_descriptor_indices[1];
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return m_descriptor_indices[2];
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return m_descriptor_indices[3];
            default: assert(false); 
        
        }
        return m_descriptor_indices[0]; 
    }

    VulkanBindlessArray::VulkanBindlessArray(VulkanDevice* _device, uint32 _max_size) :
    BindlessArray(),
    VulkanDeviceObject(_device),
    bindless_buffer_descs(nullptr),
    bindless_texture_descs(nullptr),
    g_heap(_device->GetGlobalDescriptorHeap()),
    texture_slot_offset(1),
    buffer_slot_offset(1),
    slot_offset(1),
    handles(_max_size),
    texture_view_infos(_max_size),
    texture_offset_in_buffer(_device->GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize * 256) {
        BufferInfo buffer_info(
            uint64(_max_size),
            uint(sizeof(uint)),
             EBufferUsageFlags::UNORDERED_ACCESS
        );
        VkBuffer           current_handle = VK_NULL_HANDLE;
        VkBufferCreateInfo buffer_ci      = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

        VmaAllocation      alloc          = VK_NULL_HANDLE;
        buffer_ci.size                    = _max_size * sizeof(uint32);
        buffer_ci.usage                   = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_ci.sharingMode             = VK_SHARING_MODE_EXCLUSIVE;
        buffer_ci.queueFamilyIndexCount   = 0;
        buffer_ci.pQueueFamilyIndices     = nullptr;
        buffer_ci.flags                   = 0;

#if WITH_CUDA
        buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

        VmaAllocationCreateInfo alloc_ci  = {};
        alloc_ci.usage                    = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags                    = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

        VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));
        bindless_array_buffer = MoerNew(VulkanBuffer)(s_bdls_array_name,buffer_info, *m_device, current_handle, alloc, false, true);
        
        buffer_ci.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        buffer_ci.size  = _max_size * m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize;
        current_handle   = VK_NULL_HANDLE;
        alloc           = VK_NULL_HANDLE;

        VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));
        buffer_info.size = _max_size;
        buffer_info.stride = m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize;

        bindless_buffer_descs = MoerNew(VulkanBuffer)(s_bdls_array_buffer_name, buffer_info, *m_device, current_handle, alloc, false, true);

        buffer_ci.usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        buffer_ci.size  = _max_size * m_device->GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize;
        current_handle   = VK_NULL_HANDLE;
        alloc           = VK_NULL_HANDLE;
        VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));

        buffer_info.size = _max_size;
        buffer_info.stride = m_device->GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize;
        bindless_texture_descs = MoerNew(VulkanBuffer)(s_bdls_array_image_name, buffer_info, *m_device, current_handle, alloc, false, true);

        CommandList cmd_list{};
        auto& gfx_queue = m_device->GetCommandQueue(EQueueType::Graphics);
        //fill sampler data to descriptor buffer
        {
            uint sampler_stride = m_device->GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize;
            // void* mapped_data;
            Array<byte> data_array(m_device->GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize * VulkanDevice::bindless_sampler_cnt);
            // vmaMapMemory(m_device->GetVmaAllocator(), bindless_texture_descs->GetAllocation(), &mapped_data);
            const auto* samplers = m_device->GetImmutableSamplers();
            // byte* mapped_data_byte = reinterpret_cast<byte*>(mapped_data);
            VkDescriptorGetInfoEXT descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            descriptor_info.type = VK_DESCRIPTOR_TYPE_SAMPLER;

            VkDescriptorDataEXT& descriptor_data = descriptor_info.data;
            for(uint i = 0; i < VulkanDevice::bindless_sampler_cnt; i++){
                descriptor_data.pSampler = i >= m_device->ImmutableSamplerCount() ? &samplers[0] : &samplers[i];
                vkGetDescriptorEXT( m_device->GetDevice(), &descriptor_info, sampler_stride, data_array.data() + i * sampler_stride);
            }
            // std::memcpy(mapped_data_byte, data_array.data(), data_array.size());
            // vmaUnmapMemory(m_device->GetVmaAllocator(), bindless_texture_descs->GetAllocation());
            // vmaFlushAllocation(m_device->GetVmaAllocator(), bindless_texture_descs->GetAllocation(), 0, VulkanDevice::bindless_sampler_cnt * sampler_stride);
            cmd_list.CopyFrom(std::span<byte>(data_array.data(), data_array.size()), bindless_texture_descs->GetView(0, data_array.size()));
            gfx_queue.Execute(cmd_list.Submit());
            gfx_queue.Sync();
        }


        const VkDescriptorBindingFlags flags[2] = {0,
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT };

        VkDescriptorSetLayoutBindingFlagsCreateInfoEXT binding_flags{};
        binding_flags.sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
        binding_flags.bindingCount   = 2;
        binding_flags.pBindingFlags  = flags;

        std::array<VkDescriptorSetLayoutBinding, 2> buffer_bindings;

        VkDescriptorSetLayoutBinding& array_binding = buffer_bindings[0];
        array_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        array_binding.binding = 0;
        array_binding.descriptorCount = 1;
        array_binding.stageFlags = VK_SHADER_STAGE_ALL;

        VkDescriptorSetLayoutBinding& buffer_binding = buffer_bindings[1];
        buffer_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        buffer_binding.binding = 1;
        buffer_binding.descriptorCount = _max_size;
        buffer_binding.stageFlags = VK_SHADER_STAGE_ALL;

        VkDescriptorSetLayoutCreateInfo buffer_desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        buffer_desc_info.bindingCount = 2;
        buffer_desc_info.pBindings = buffer_bindings.data();
        buffer_desc_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        buffer_desc_info.pNext = &binding_flags;

        VkDescriptorSetLayout buffer_desc_layout = VK_NULL_HANDLE;
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(m_device->GetDevice(), &buffer_desc_info, VK_NULL_HANDLE, &buffer_desc_layout));        uint64 buffers_offset;
        vkGetDescriptorSetLayoutBindingOffsetEXT(m_device->GetDevice(), buffer_desc_layout, 1, &buffers_offset_in_set);

        VkDescriptorGetInfoEXT descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        VkDescriptorAddressInfoEXT address_info{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        address_info.address = bindless_array_buffer->DeviceAddress();
        address_info.range = bindless_array_buffer->GetByteSize();
        descriptor_info.data.pStorageBuffer = &address_info;
        descriptor_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        {
            Array<byte> buffer_data(m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize);

            // byte* mapped_data;
            // vmaMapMemory(m_device->GetVmaAllocator(), bindless_buffer_descs->GetAllocation(), (void**)&mapped_data);
            vkGetDescriptorEXT(
                m_device->GetDevice(),
                &descriptor_info,
                m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize,
                buffer_data.data());
            // std::memcpy(mapped_data, buffer_data.data(), buffer_data.size());
            // vmaUnmapMemory(m_device->GetVmaAllocator(), bindless_buffer_descs->GetAllocation());
            // vmaFlushAllocation(m_device->GetVmaAllocator(), bindless_buffer_descs->GetAllocation(), 0, m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize);
            cmd_list.CopyFrom(std::span<byte>(buffer_data.data(), buffer_data.size()), bindless_buffer_descs->GetView(0, buffer_data.size()));
            gfx_queue.Execute(cmd_list.Submit());
            gfx_queue.Sync();
        }

        vkDestroyDescriptorSetLayout(m_device->GetDevice(), buffer_desc_layout, VK_NULL_HANDLE);

    }

    static uint SamplerToIndex(const Sampler& _samp) {
        uint idx = uint(_samp.filter) + uint(_samp.address_mode) * uint(SF_Num) + uint(_samp.compare_function) * uint(SAM_Num) * uint(SF_Num);
        return idx;
    }

    static bool RangeOverlap(uint32 _a_start, uint32 _a_count, uint32 _b_start, uint32 _b_count) {
        uint32 a_count = _a_count == 0 ? 1u : _a_count;
        uint32 b_count = _b_count == 0 ? 1u : _b_count;
        uint32 a_end   = _a_start + a_count;
        uint32 b_end   = _b_start + b_count;
        return _a_start < b_end && _b_start < a_end;
    }

    void VulkanBindlessArray::TrackTextureView(uint _array_idx, const TextureView& _view) {
        UntrackTextureView(_array_idx);
        auto*  vk_texture  = ResourceCast(_view.texture);
        uint64 texture_ptr = uint64(vk_texture);
        if (texture_ptr == 0) {
            return;
        }
        ValidateSubresourceRange(_view.texture, _view.mip_level, _view.num_mips, _view.array_layer, _view.num_array);
        TextureSubresourceKeyT<VulkanTexture> key{
            vk_texture, _view.mip_level, _view.num_mips, _view.array_layer, _view.num_array
        };
        texture_view_infos[_array_idx] = key;
        texture_view_map[texture_ptr].insert(key);
    }

    void VulkanBindlessArray::UntrackTextureView(uint _array_idx) {
        if (_array_idx >= texture_view_infos.size()) {
            return;
        }
        const auto& key = texture_view_infos[_array_idx];
        if (key.texture == nullptr) {
            return;
        }
        uint64 texture_ptr = uint64(key.texture);
        if (auto it = texture_view_map.find(texture_ptr); it != texture_view_map.end()) {
            it->second.erase(key);
            if (it->second.empty()) {
                texture_view_map.erase(it);
            }
        }
        texture_view_infos[_array_idx] = {};
    }

    UniquePtr<Command> VulkanBindlessArray::CreateUpdateCommand(){
        std::unique_lock<std::mutex> lk(mtx);

        {
            uint64 texture_handle_stride = m_device->GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize;
            uint64 buffer_handle_stride  = m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize;
            uint64 array_handle_stride   = sizeof(uint);

            //update all cpu data here instead of during execution

            //handle update data first
            VulkanDescriptorHeap& heap = m_device->GetGlobalDescriptorHeap();

            uint array_idx = 0;
            uint texture_cnt = 0;
            uint buffer_cnt = 0;

            //lock for resource_allocated_set
            for(const UpdateCmd& cmd : update_cmds){
                std::visit(
                    [&](auto&& _cmd){
                        using Tcmd = std::decay_t<decltype(_cmd)>;
                        if constexpr (std::is_same_v<Tcmd, TextureUpdateInfo>){
                            if(_cmd.free){
                                auto& handle = handles[_cmd.array_idx];
                                assert(handle.type == VulkanBindlessArray::Texture && handle.Ptr() && "Invalid texture handle");
                                free_slots.Push(_cmd.array_idx);
                                free_texture_slots.Push(_cmd.slot);
                                resource_allocated_set.erase(handle.Ptr());
                                UntrackTextureView(_cmd.array_idx);

                                handle = Handle(0ull, 0, 0, 0);
    
                            }else{
                                ++array_idx;
                                ++texture_cnt;
                            }

                        }
                        else if constexpr (std::is_same_v<Tcmd, BufferUpdateInfo>){
                            if(_cmd.free){
                                auto& handle = handles[_cmd.array_idx];
                                assert(handle.type == VulkanBindlessArray::Buffer && handle.Ptr() && "Invalid buffer handle");
                                free_slots.Push(_cmd.array_idx);
                                free_buffer_slots.Push(_cmd.slot);
                                resource_allocated_set.erase(handle.Ptr());

                                handle = Handle(0ull, 0, 0, 0);
    
                            }else{
                                ++array_idx;
                                ++buffer_cnt;
                            }
                        }
                    }, cmd
                );
            }

            array_indices_dat.resize(array_idx);
            texture_indices_dat.resize(texture_cnt);
            buffer_indices_dat.resize(buffer_cnt);

            array_dat.resize(array_idx * array_handle_stride);
            texture_dat.resize(texture_cnt * texture_handle_stride);
            buffer_dat.resize(buffer_cnt * buffer_handle_stride);

            array_idx = 0;
            texture_cnt = 0;
            buffer_cnt = 0;

            //buffer desc buffer has array_buffer for offset 0 and allocate from slot 1(0 for invalid handle)
            //array buffer is in same set with other bindless buffer descs, so we need to offset the slot by array_buffer slot(size of storage buffer handle)
            //in gpu:
            // bindless_array_buffer: [binding 0, offset 0]
            // bindless_buffer_descs: [binding 1, offset $sizeofhandle)]
            uint buffer_dst_slot_offset = buffers_offset_in_set / buffer_handle_stride;
            for(const UpdateCmd& cmd : update_cmds){
                std::visit(
                    [&](auto&& _cmd){
                        using Tcmd = std::decay_t<decltype(_cmd)>;
                        if constexpr (std::is_same_v<Tcmd, TextureUpdateInfo>){
                            if(!_cmd.free){
                                handles[_cmd.array_idx] = Handle((uint64)_cmd.texture.Get(), _cmd.slot, 1, VulkanBindlessArray::Texture);
                                VulkanTexture* vk_texture = ResourceCast(_cmd.texture.Get());
                                ETextureAspectFlags aspect = _cmd.aspect_flags;
                                if (aspect == ETextureAspectFlags::NONE) {
                                    aspect = vk_texture->GetAspectFlags();
                                }
                                TextureView    view(vk_texture, _cmd.format, aspect, _cmd.mip_level, _cmd.num_mips);
                                if (_cmd.array_count > 0) {
                                    view = view.Slice(_cmd.array_layer, _cmd.array_count);
                                }

                                uint           src_idx;
                                if (uint(vk_texture->GetAspectFlags() & ETextureAspectFlags::DEPTH_SLICE) != 0) {
                                    // view.aspect_flags = ETextureAspectFlags::COLOR;
                                    src_idx = heap.GetImageDescIdx(&view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

                                } else {
                                    src_idx = heap.GetImageDescIdx(&view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                }

                                memcpy(texture_dat.data() + texture_cnt * texture_handle_stride, &heap.image_desc_data[src_idx], texture_handle_stride);
                                uint indirect_handle = (m_device->GetSamplerIdx(_cmd.sampler) & 0xff) | (_cmd.slot & 0xffffff) << 8;
                                memcpy(array_dat.data() + array_idx * array_handle_stride, &indirect_handle, array_handle_stride);
                                array_indices_dat[array_idx] = {array_idx, _cmd.array_idx};
                                texture_indices_dat[texture_cnt] = {texture_cnt, _cmd.slot};

                                ++array_idx;
                                ++texture_cnt;
                            }

                        }
                        else if constexpr (std::is_same_v<Tcmd, BufferUpdateInfo>){
                            if(!_cmd.free){
                                handles[_cmd.array_idx] = Handle((uint64)_cmd.buffer.Get(), _cmd.slot, 0, VulkanBindlessArray::Buffer);
                                VulkanBuffer* vk_buffer = ResourceCast(_cmd.buffer.Get());
                                VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                                if(_cmd.format != PF_UNDEFINED){
                                    LOG_WARNING("Bindless buffer with format is not supported yet, current buffer: {}", _cmd.buffer->GetName());
                                }
                                uint src_idx = heap.GetBufferDescIdx(vk_buffer->GetView(_cmd.format), type);
                                
                                memcpy(buffer_dat.data() + buffer_cnt * buffer_handle_stride, &heap.buffer_desc_data[src_idx], buffer_handle_stride);
                                memcpy(array_dat.data() + array_idx * array_handle_stride, &_cmd.slot, sizeof(uint));
                                array_indices_dat[array_idx] = {array_idx, _cmd.array_idx};
                                buffer_indices_dat[buffer_cnt] = {buffer_cnt, _cmd.slot + buffer_dst_slot_offset};

                                ++array_idx;
                                ++buffer_cnt;
                            }
                        }

                    }, cmd
                );
            }

        }
        temp_slot_to_cmd.clear();
        UniquePtr<Command> cmd = MakeUnique<UpdateBindlessArrayCmd>(this, 
        std::move(update_cmds),
        std::move(array_dat),
        std::move(array_indices_dat),
        std::move(buffer_dat),
        std::move(buffer_indices_dat),
        std::move(texture_dat),
        std::move(texture_indices_dat));

        update_cmds.clear();
        return cmd;
    }

    uint VulkanBindlessArray::AllocateTexture(const TextureView& _texture, Sampler _sampler) {
        uint  slot_idx  = 0;
        uint array_idx = free_slots.Pop();
        if (array_idx == 0) {
            slot_idx = slot_offset++;
        } else { slot_idx = array_idx; }
        //allocate texture slot
        uint  texture_slot     = 0;
        uint texture_slot_ptr = free_texture_slots.Pop();
        if (texture_slot_ptr == 0) { texture_slot = texture_slot_offset++; } else { texture_slot = texture_slot_ptr; }
        std::unique_lock<std::mutex> lk(mtx);

        update_cmds.emplace_back(
            TextureUpdateInfo{
                _texture.texture,
                _sampler,
                _texture.format,
                _texture.aspect_flags,
                slot_idx,
                texture_slot,
                _texture.mip_level,
                _texture.num_mips,
                _texture.array_layer,
                _texture.num_array,
                false
            }
        );
        temp_slot_to_cmd[slot_idx] = update_cmds.size() - 1;
        // textures_allocated.emplace_back(_texture.texture, _sampler, _texture.format, slot_idx, texture_slot, _texture.mip_level, _texture.num_mips);
        resource_allocated_set.insert(uint64(_texture.texture));
        TrackTextureView(slot_idx, _texture);
        return slot_idx;
    }

    uint VulkanBindlessArray::AllocateBuffer(BufferView _buffer) {
        uint  slot_idx;
        uint array_idx = free_slots.Pop();
        if (array_idx == 0) {
            slot_idx = slot_offset++;
        } else { slot_idx = array_idx; }

        //allocate buffer index
        uint  buffer_slot;
        uint buffer_slot_ptr = free_buffer_slots.Pop();
        if (buffer_slot_ptr == 0) { buffer_slot = buffer_slot_offset++; } else { buffer_slot = buffer_slot_ptr; }
        std::unique_lock<std::mutex> lk(mtx);
        update_cmds.emplace_back(BufferUpdateInfo{_buffer.buffer, slot_idx, buffer_slot,_buffer.format, false});
        temp_slot_to_cmd[slot_idx] = update_cmds.size() - 1;
        // buffers_allocated.emplace_back(_buffer.buffer, slot_idx, buffer_slot);
        resource_allocated_set.insert(uint64(_buffer.buffer));
        return slot_idx;
    }
    static const Sampler s_spl = {ESamplerFilter::SF_LINEAR, SAM_CLAMP_TO_BORDER};
    void VulkanBindlessArray::UnbindTexture(uint _array_idx) {
        std::unique_lock<std::mutex> lk(mtx);
        if(auto iter = temp_slot_to_cmd.find(_array_idx); iter != temp_slot_to_cmd.end()){
            const UpdateCmd& src_cmd = update_cmds[iter->second];

            std::visit(
                [&](auto&& _cmd){
                    using TCmd = std::decay_t<decltype(_cmd)>;
                    if constexpr (std::is_same_v<TCmd, TextureUpdateInfo>){
                        if(_cmd.free){
                            LOG_WARNING("You are releasing a bindless texture that's pending releasing, array index: {}", _array_idx);
                            return;
                        }else{
                            //TODO: invalidate update command
                            LOG_WARNING("You are releasing a bindless texture that's pending updating, array index: {}", _array_idx);
                            resource_allocated_set.erase((uint64)_cmd.texture.Get());
                            UntrackTextureView(_array_idx);

                            update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
                        }
                } else if constexpr (std::is_same_v<TCmd, BufferUpdateInfo>){
                    if(_cmd.free){
                        LOG_WARNING("You are releasing a bindless buffer that's pending releasing, array index: {}", _array_idx);
                        return;
                    }else{
                        LOG_WARNING("You are releasing a bindless texture that's pending updating, array index: {}", _array_idx);
                        resource_allocated_set.erase((uint64)_cmd.buffer.Get());
                        UntrackTextureView(_array_idx);
                        update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
                    }
                } else if constexpr (std::is_same_v<TCmd, InvalidUpdateInfo>){
                        
                    }
                }, src_cmd
            );
            return;
        }


        const auto& handle = handles[_array_idx];
        if(handle.type == Texture){
            update_cmds.emplace_back(TextureUpdateInfo{
                nullptr,
                s_spl,
                PF_UNDEFINED,
                ETextureAspectFlags::NONE,
                _array_idx,
                handle.slot,
                0,
                0,
                0,
                0,
                true});
            UntrackTextureView(_array_idx);
        }else if (handle.type == Buffer){
            update_cmds.emplace_back(BufferUpdateInfo{nullptr, _array_idx, handle.slot, PF_UNDEFINED, true});
        }
    }

    void VulkanBindlessArray::UnbindBuffer(uint _array_idx) {
        std::unique_lock<std::mutex> lk(mtx);

        if(auto iter = temp_slot_to_cmd.find(_array_idx); iter != temp_slot_to_cmd.end()){
            const UpdateCmd& src_cmd = update_cmds[iter->second];

            std::visit(
                [&](auto&& _cmd){
                    using TCmd = std::decay_t<decltype(_cmd)>;
                    if constexpr (std::is_same_v<TCmd, TextureUpdateInfo>){
                        if(_cmd.free){
                            LOG_WARNING("You are releasing a bindless texture that's pending releasing, array index: {}", _array_idx);
                            return;
                        }else{
                            LOG_WARNING("You are releasing a bindless buffer that's pending updating, array index: {}", _array_idx);

                            resource_allocated_set.erase((uint64)_cmd.texture.Get());
                            update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
                        }
                    } else if constexpr (std::is_same_v<TCmd, BufferUpdateInfo>){
                        if(_cmd.free){
                            LOG_WARNING("You are releasing a bindless buffer that's pending releasing, array index: {}", _array_idx);
                            return;
                        }else{
                            LOG_WARNING("You are releasing a bindless buffer that's pending updating, array index: {}", _array_idx);

                            resource_allocated_set.erase((uint64)_cmd.buffer.Get());
                            update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
                        }
                    } else if constexpr (std::is_same_v<TCmd, InvalidUpdateInfo>){
                        
                    }
                }, src_cmd
            );
            return;
        }

        const auto& handle = handles[_array_idx];
        if(handle.type == Texture){
            // textures_freed.push_back(handle.slot);
            update_cmds.emplace_back(
                TextureUpdateInfo{
                    nullptr,
                    s_spl,
                    PF_UNDEFINED,
                    ETextureAspectFlags::NONE,
                    _array_idx,
                    handle.slot,
                    0,
                    0,
                    0,
                    0,
                    true}
            );
        }else if (handle.type == Buffer){
            // buffers_freed.push_back(handle.slot);
            update_cmds.emplace_back(BufferUpdateInfo{nullptr, _array_idx, handle.slot, PF_UNDEFINED, true});
        }
        // resource_allocated_set.erase((uint64)(buffers_allocated[handle.slot].buffer.Get()));
    }

    bool VulkanBindlessArray::IsResourceAllocated(uint64 _resource) const {
        return resource_allocated_set.find(_resource) != resource_allocated_set.end();
    }

    bool VulkanBindlessArray::IsTextureViewAllocated(
        uint64 _texture,
        uint8  _mip_level,
        uint8  _mip_count,
        uint8  _array_layer,
        uint8  _array_count
    ) const {
        auto it = texture_view_map.find(_texture);
        if (it == texture_view_map.end()) {
            return false;
        }
        auto* vk_texture = reinterpret_cast<VulkanTexture*>(_texture);
        uint32 mip_level   = _mip_level;
        uint32 mip_count   =
            _mip_count == kRemainingSubresource ? (vk_texture->GetNumMips() - mip_level) : _mip_count;
        uint32 array_layer = _array_layer;
        uint32 array_count =
            _array_count == kRemainingSubresource ? (vk_texture->GetNumArray() - array_layer) : _array_count;
        for (const auto& view : it->second) {
            if (RangeOverlap(view.mip_level, view.mip_count, mip_level, mip_count) &&
                RangeOverlap(view.array_layer, view.array_count, array_layer, array_count)) {
                return true;
            }
        }
        return false;
    }

    void VulkanBindlessArray::DeAllocateResource(uint64 _res){
        resource_allocated_set.erase(_res);
    }
    
    struct CopyPair{
        uint src_idx;
        uint dst_idx;
    };

    void VulkanBindlessArray::OnFree(const Array<uint>& _slots_freed, const Array<uint>& _textures_freed, const Array<uint>& _buffers_freed) {
        for (const uint& idx : _textures_freed) { free_texture_slots.Push(idx);
         }
        for (const uint& idx : _buffers_freed) { free_buffer_slots.Push(idx); }

        for (const uint& idx : _slots_freed) { free_slots.Push(idx); }
    }

    VulkanBindlessArray::~VulkanBindlessArray() {
        if (bindless_array_buffer) {
            MoerDelete(bindless_array_buffer);
            bindless_array_buffer = nullptr;
        }
        if (bindless_buffer_descs) {
            MoerDelete(bindless_buffer_descs);
            bindless_buffer_descs = nullptr;
        }
        if (bindless_texture_descs) {
            MoerDelete(bindless_texture_descs);
            bindless_texture_descs = nullptr;
        }
    }
#pragma endregion

#pragma region [ raytracing ]

    void GetBuildRaytracingGeometryInfo(
        Array<VkAccelerationStructureGeometryKHR>& _build_info,
        Array<uint>& _primitive_counts,
        Array<VkAccelerationStructureBuildRangeInfoKHR>& _build_ranges,
        const RaytracingGeometryInfo& _info){

            _build_info.reserve(_info.segments.size());
            _build_ranges.reserve(_info.segments.size());
            _primitive_counts.reserve(_info.segments.size());

            assert(_info.segments.size() > 0 && "No segment to build");

            VkGeometryTypeKHR geometry_type = VulkanEnumTranslator::METoVKGeometryType(_info.segments[0].type);
            
            for (const auto& segment : _info.segments) {
                //TODO: currently only support triangle
                assert(segment.type == RTGT_TRIANGLES && "Unsupported geometry type");

                uint64 vtx_addr =  ResourceCast(segment.vertex_buffer.Get())->DeviceAddress();
                uint64 idx_addr =  ResourceCast(segment.index_buffer.Get())->DeviceAddress();
                assert(vtx_addr != 0 && idx_addr != 0 && "Invalid buffer address");

                VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                geometry.geometryType = VulkanEnumTranslator::METoVKGeometryType(segment.type);
                geometry.flags = VulkanEnumTranslator::METoVKGeometryFlags(segment.flags);
                geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                geometry.geometry.triangles.vertexFormat = g_platform_pixel_formats[_info.vertex_format].format;
                geometry.geometry.triangles.vertexStride = segment.vertex_stride;
                geometry.geometry.triangles.vertexData.deviceAddress = vtx_addr + segment.vertex_offset;
                // 下一个变量maxVertex，定义了允许的最大顶点索引值。此处应为 first_vertex + vertex_count
                geometry.geometry.triangles.maxVertex = segment.first_vertex - segment.vertex_count;
                geometry.geometry.triangles.indexType = VulkanEnumTranslator::METoVKIndexType(_info.index_type);
                geometry.geometry.triangles.indexData.deviceAddress = idx_addr + segment.index_offset; 
                geometry.geometry.triangles.transformData.deviceAddress = 0;

                _build_info.push_back(geometry);
                _primitive_counts.push_back(segment.primitive_count);

                VkAccelerationStructureBuildRangeInfoKHR range{};
                range.firstVertex = segment.first_vertex;
                range.primitiveOffset = segment.first_primitive * 3 * (_info.index_type == EIndexElementType::IET_UINT16 ? 2 : 4);
                range.primitiveCount = segment.primitive_count;
                range.transformOffset = 0;

                _build_ranges.push_back(range);
            }
        }

    VulkanRaytracingGeometry::VulkanRaytracingGeometry(const RaytracingGeometryInfo& _info, VulkanDevice* _device): VulkanDeviceObject(_device), RaytracingGeometry(_info){

        VkAccelerationStructureBuildTypeKHR                 build_type = VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR build_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        Array<uint> primitive_counts;
        GetBuildRaytracingGeometryInfo(build_geometries,primitive_counts, build_ranges, _info);
        build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build_info.flags = VulkanEnumTranslator::METoVKAccelerationStructureBuildType(_info.build_flags);
        build_info.geometryCount = build_geometries.size();
        build_info.pGeometries = build_geometries.data();
        vkGetAccelerationStructureBuildSizesKHR(
            m_device->GetDevice(), 
            build_type, 
            &build_info, 
            primitive_counts.data(), 
            &build_sizes_info);


        {
            BufferInfo buffer_info(
            build_sizes_info.accelerationStructureSize,
            1,
            EBufferUsageFlags::ACCELERATION_STRUCTURE
            );

            VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_ci.size = buffer_info.size;
            buffer_ci.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

#if WITH_CUDA
            buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

            VmaAllocationCreateInfo alloc_ci{};
            alloc_ci.flags = 0;
            alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation alloc = VK_NULL_HANDLE;

            vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &buffer, &alloc, nullptr);

            underlying_buffer = MoerNew(VulkanBuffer)(s_blas_underlying_buffer_name,buffer_info, *m_device, buffer, alloc, false, true);

        }


        VkAccelerationStructureCreateInfoKHR create_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create_info.deviceAddress = 0;//for capture replay
        create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        create_info.createFlags = 0;
        create_info.buffer = underlying_buffer->GetHandle();
        create_info.offset = 0;
        create_info.size = underlying_buffer->GetByteSize();


        VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(m_device->GetDevice(), &create_info, nullptr, &acc));
        VkAccelerationStructureDeviceAddressInfoKHR address_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        address_info.accelerationStructure = acc;
        blas_address = vkGetAccelerationStructureDeviceAddressKHR(m_device->GetDevice(), &address_info);

    }

    VulkanAccelerationStructure::VulkanAccelerationStructure(VulkanDevice& _device, VulkanRaytracingScene& _scene):device(_device), src_scene(_scene), RaytracingTlas(){}

    VulkanRaytracingGeometry::~VulkanRaytracingGeometry(){
        if(acc != VK_NULL_HANDLE){
            vkDestroyAccelerationStructureKHR(m_device->GetDevice(), acc, VK_NULL_HANDLE);
            acc = VK_NULL_HANDLE;
        }

        if(underlying_buffer){
            underlying_buffer = nullptr;
        }
    }

    VulkanAccelerationStructure::~VulkanAccelerationStructure(){
        if(handle != VK_NULL_HANDLE){
            vkDestroyAccelerationStructureKHR(device.GetDevice(), handle, VK_NULL_HANDLE);
            handle = VK_NULL_HANDLE;
        }

        if (underlying_buffer) {
            underlying_buffer = nullptr;
        }
        if(m_descriptor_idx >= 0){
            device.GetGlobalDescriptorHeap().FreeAccelDescIdx(m_descriptor_idx);
        }
    }

    void VulkanAccelerationStructure::Destroy(){
        device.EnqueueDeferredRelease(this);
    }

    VulkanRaytracingScene::VulkanRaytracingScene(VulkanDevice* _device) : VulkanDeviceObject(_device), RaytracingScene() {


        VkBuffer          current_handle = VK_NULL_HANDLE;
        VmaAllocation     alloc          = VK_NULL_HANDLE;


        VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_ci.size = sizeof(VkAccelerationStructureInstanceKHR) * 1000;
        buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        buffer_ci.flags = 0;

#if WITH_CUDA
        buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.flags = 0;
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));

        instance_buffer = MoerNew(VulkanBuffer)(s_tlas_instance_buffer_name,BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE}, *m_device, current_handle, alloc, true, true);

    }

    VulkanRaytracingScene::~VulkanRaytracingScene(){
    }

    RaytracingInstance& VulkanRaytracingScene::AddInstance(){
        // uint idx = instances.size();
        uint idx = 0;
        if(free_instance_slots.empty()){
            idx = instance_offset++;
        }else{
            idx = free_instance_slots.back();
            free_instance_slots.pop_back();
        }
        RaytracingInstance instance{
            .array_idx = idx - 1,
            .instance_id = idx - 1
        };
        VkAccelerationStructureInstanceKHR vk_instance{

        };
        instances.emplace_back(instance);
        vk_instances.emplace_back(vk_instance);
        MarkModified(idx - 1);

        if(idx >= instance_capacity){
            //resize the buffer
        }
        return instances.back();
    }

    void VulkanRaytracingScene::FreeInstance(uint _array_idx){
        instances[_array_idx].visible_mask = RTVM_NONE;
        MarkModified(_array_idx); //Is not correct, because maybe the instance has invalid BLAS
        free_instance_slots.push_back(_array_idx + 1);
        //TODO: use default blas as place holder for invalid rtgeometry
    }

    void VulkanRaytracingScene::MarkModified(uint _array_idx){
        auto res =  temp_modified_instance_ids.emplace(_array_idx);
        if(res.second){
            temp_update_instance_ids.emplace_back(_array_idx);
        }
    }

    void VulkanRaytracingScene::RegisterGeometry(RaytracingGeometryRef _geometry){
        VulkanRaytracingGeometry* geometry = ResourceCast(_geometry.Get());
        assert(geometry && "Invalid geometry");

        auto pair = related_geometries.try_emplace(uint64(geometry), 1);
        if(!pair.second){
            related_geometries[uint64(geometry)]++;
        }
    }

    void VulkanRaytracingScene::UnregisterGeometry(RaytracingGeometryRef _geometry){
        VulkanRaytracingGeometry* geometry = ResourceCast(_geometry.Get());
        assert(geometry && "Invalid geometry");
        auto iter = related_geometries.find(uint64(geometry));
        if(iter != related_geometries.end()){
            iter->second++;
        }

    }

    static VkGeometryInstanceFlagsKHR METoRTInstanceFlags(const RaytracingSegment& _segment, const RaytracingInstance& _instance){
        VkGeometryInstanceFlagsKHR flags = 0;
        if((EnumHasAllFlag(_segment.flags, ERayTracingGeometryFlags::GEOMETRY_OPAQUE)) && _segment.b_force_opaque ){
            flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        }
        if(_segment.b_flip_face){
            flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
        }
        if(!_segment.b_cull_back_face){
            flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        }
        return flags;
    }

    UniquePtr<Command> VulkanRaytracingScene::UpdateScene(){

        related_geometries.clear();
        temp_update_instances.clear();

                //update gpu buffer
        bool b_full_refit = false;
        if (instance_capacity < instances.size()){
            RefitInstanceBuffer();
            b_full_refit = true;
        }
        bool b_need_force_build = RefitTLASAndScratchBuffer();

        // for(auto prev_idx : prev_modified_instance_ids){
        //     if(!temp_modified_instance_ids.contains(prev_idx)){
        //         temp_update_instance_ids.push_back(prev_idx);
        //     }
        // }
        if(!prev_modified_instance_ids.empty()){
            b_need_force_build = true;
        }

        prev_modified_instance_ids.clear();


        // b_current_full_refit |= b_full_refit;

        // if(b_prev_full_refit){
        //     b_full_refit = true;
        //     b_prev_full_refit = false;
        // }

        if(b_full_refit){
            temp_update_instances.reserve(instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
            std::span<byte> temp_span((byte*)vk_instances.data(), vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR));

            temp_update_instance_ids.clear();
            for(uint i = 0; i < instances.size(); i++){
                temp_update_instance_ids.push_back(i);
            }

        }
        
        {

            temp_update_instances.resize(temp_update_instance_ids.size() * sizeof(VkAccelerationStructureInstanceKHR));
            //update cpu size vk_instance datas
            for (const auto& idx : temp_update_instance_ids) {
                VulkanRaytracingGeometry* geometry = ResourceCast(instances[idx].geom);
                assert(geometry && "Invalid geometry");
                const Array<RaytracingSegment>& segments = geometry->GetInfo().segments;
                VkAccelerationStructureInstanceKHR& vk_instance = vk_instances[idx];
                const RaytracingInstance& instance = instances[idx];
                vk_instance.instanceCustomIndex = instance.custom_index;
                vk_instance.mask = instance.visible_mask;
                vk_instance.instanceShaderBindingTableRecordOffset = instance.material_ref.sbt_offset;
                vk_instance.flags = METoRTInstanceFlags(segments[instance.segment_idx], instance);
                vk_instance.accelerationStructureReference = geometry->blas_address;
                std::memcpy(&vk_instance.transform, &instance.transform, sizeof(vk_instance.transform));

                uint indice = &idx - &temp_update_instance_ids[0];
                std::memcpy(temp_update_instances.data() + indice * sizeof(VkAccelerationStructureInstanceKHR), &vk_instance, sizeof(VkAccelerationStructureInstanceKHR));

                auto pair = related_geometries.try_emplace(uint64(geometry), 1);
                if(!pair.second){
                    related_geometries[uint64(geometry)]++;
                }
            }
        }


        //maybe we should calculate relative blas here
        // Array<uint> instance_ids_to_update = temp_update_instance_ids;
        
        // RefitTLASAndScratchBuffer();
        // temp_modified_instance_ids.clear();
        return MakeUnique<UpdateRaytracingSceneCmd>(
            std::move(related_geometries),
            uint64(this),
            uint64(instance_buffer.Get()),
            uint64(scratch_buffer.Get()),
            uint64(tlas.Get()),
            std::move(temp_update_instance_ids),
            std::move(temp_update_instances),
            vk_instances.size(),
            b_full_refit || b_need_force_build);
    }

    void VulkanRaytracingScene::AdvanceFrame(){  
        
        assert(prev_modified_instance_ids.empty() && "There are still modified instances not updated, call UpdateScene() first");
        
        prev_modified_instance_ids.swap( temp_modified_instance_ids);
        b_current_full_refit = false;

        std::swap(prev_instance_capacity, instance_capacity);
        prev_size_infos.build_scratch_size = size_infos.build_scratch_size;
        prev_size_infos.update_scratch_size = size_infos.update_scratch_size;
        std::swap(prev_size_infos, size_infos);

        std::swap(prev_tlas, tlas);
    }

    RaytracingTlasRef VulkanRaytracingScene::GetTlas() const{
        return RaytracingTlasRef(tlas.Get());
    }

    RaytracingTlasRef VulkanRaytracingScene::GetPrevTlas() const{
        return RaytracingTlasRef(prev_tlas.Get());
    }

    void VulkanRaytracingScene::RefitInstanceBuffer(){
        VkBuffer          current_handle = VK_NULL_HANDLE;
        VmaAllocation     alloc          = VK_NULL_HANDLE;

        instance_capacity *= exponent;
        VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_ci.size = sizeof(VkAccelerationStructureInstanceKHR) * instance_capacity;
        buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        buffer_ci.flags = 0;

#if WITH_CUDA
        buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.flags = 0;
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));
        instance_buffer = MoerNew(VulkanBuffer)(s_tlas_instance_buffer_name,BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE}, *m_device, current_handle, alloc, true, true);
        
    }

    RaytracingSizeInfos VulkanRaytracingScene::CalculateSizeInfos(){
        VkAccelerationStructureBuildGeometryInfoKHR build_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};

        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.flags = 0;
        geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        geometry.geometry.instances.data.deviceAddress = instance_buffer->DeviceAddress();

        build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build_info.srcAccelerationStructure = VK_NULL_HANDLE;
        build_info.geometryCount = 1;
        build_info.pGeometries = &geometry;
        build_info.scratchData.deviceAddress = 0;
        build_info.dstAccelerationStructure = VK_NULL_HANDLE;


        VkAccelerationStructureBuildSizesInfoKHR build_sizes_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};

        uint instance_cnt = instances.size();
        vkGetAccelerationStructureBuildSizesKHR(
            m_device->GetDevice(),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &build_info,
            &instance_cnt,
            &build_sizes_info);
        
        // reserve more space for buffer device address alignment
        const uint64 alignment = m_device->GetOptionalProperties().acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
        build_sizes_info.buildScratchSize = Moer::AlignUp(build_sizes_info.buildScratchSize + alignment, alignment);
        build_sizes_info.updateScratchSize = Moer::AlignUp(build_sizes_info.updateScratchSize + alignment, alignment);
        return {build_sizes_info.buildScratchSize, build_sizes_info.updateScratchSize, build_sizes_info.accelerationStructureSize};
    }

    bool VulkanRaytracingScene::RefitTLASAndScratchBuffer(){
        //calculate prebuild info

        RaytracingSizeInfos build_sizes_info = CalculateSizeInfos();

        if (build_sizes_info.result_size > size_infos.result_size ||
        build_sizes_info.build_scratch_size > size_infos.build_scratch_size) {
            //recreate buffers
            VkBuffer          current_handle = VK_NULL_HANDLE;
            VmaAllocation     alloc          = VK_NULL_HANDLE;

            VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

            buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            buffer_ci.flags = 0;
                    
#if WITH_CUDA
            buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

            VmaAllocationCreateInfo alloc_ci{};
            alloc_ci.flags = 0;
            alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

            if(build_sizes_info.build_scratch_size > size_infos.build_scratch_size){
                //scratch buffer
                buffer_ci.size = build_sizes_info.build_scratch_size;
                buffer_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));
            
                scratch_buffer = MoerNew(VulkanBuffer)(s_tlas_scratch_buffer_name, BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE}, *m_device, current_handle, alloc, true, true);
                
            }
            //TLAS
            if(build_sizes_info.result_size > size_infos.result_size){
                buffer_ci.size = build_sizes_info.result_size;
                buffer_ci.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));
                
                tlas = MoerNew(VulkanAccelerationStructure)(*m_device, *this);

                tlas->underlying_buffer = MoerNew(VulkanBuffer)(s_tlas_underlying_buffer_name,BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE}, *m_device, current_handle, alloc, true, true);

                //create new TLAS
                VkAccelerationStructureCreateInfoKHR create_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                create_info.deviceAddress = 0;//for capture replay
                create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
                create_info.createFlags = VK_ACCELERATION_STRUCTURE_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT;
                create_info.buffer = tlas->underlying_buffer->GetHandle();
                create_info.offset = 0;
                create_info.size = tlas->underlying_buffer->GetByteSize();
                VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(m_device->GetDevice(), &create_info, nullptr, &tlas->handle));
                
                // //create previous tlas
                // if(prev_size_infos.result_size == 0){
                    
                //     VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));
                //     prev_tlas = MoerNew(VulkanAccelerationStructure)(*m_device, *this);
                //     prev_tlas->underlying_buffer = MoerNew(VulkanBuffer)(BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE}, *m_device, current_handle, alloc, false, true);
                //     prev_tlas->underlying_buffer->SetName("TLAS underly buffer Pong");
                //     create_info.buffer = prev_tlas->underlying_buffer->GetHandle();
                //     VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(m_device->GetDevice(), &create_info, nullptr, &prev_tlas->handle));
                //     prev_size_infos = build_sizes_info;
                // }
            }
            size_infos = build_sizes_info;
            return true;
        }
        return false;
    }

#pragma endregion

#pragma region shader param
#pragma endregion

#pragma region synchronization

    VulkanFence::VulkanFence(VulkanDevice& _device) : VulkanDeviceObject(&_device) {
        VkSemaphoreCreateInfo create_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        VkSemaphoreTypeCreateInfo timeline_semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline_semaphore_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timeline_semaphore_info.initialValue  = 0;

#if WITH_CUDA
        WindowsSecurityAttributes winSecurityAttributes;

        VkExportSemaphoreWin32HandleInfoKHR vulkanExportSemaphoreWin32HandleInfoKHR = {};
        vulkanExportSemaphoreWin32HandleInfoKHR.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
        vulkanExportSemaphoreWin32HandleInfoKHR.pNext       = NULL;
        vulkanExportSemaphoreWin32HandleInfoKHR.pAttributes = &winSecurityAttributes;
        vulkanExportSemaphoreWin32HandleInfoKHR.dwAccess    = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
        vulkanExportSemaphoreWin32HandleInfoKHR.name        = (LPCWSTR)NULL;

        VkExportSemaphoreCreateInfoKHR vulkanExportSemaphoreCreateInfo = {};
        vulkanExportSemaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO_KHR;
        vulkanExportSemaphoreCreateInfo.pNext = &vulkanExportSemaphoreWin32HandleInfoKHR;
        vulkanExportSemaphoreCreateInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        timeline_semaphore_info.pNext = &vulkanExportSemaphoreCreateInfo;
#endif

        create_info.pNext = &timeline_semaphore_info;
        VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &timeline));
    }

    uint64 VulkanFence::GetValue() const {
        // uint64_t value;
        // vkGetSemaphoreCounterValue(m_device->GetDevice(), timeline, &value);
        // // vkGetSemaphoreCounterValue(m_device->GetDevice(), m_fence.timeline, &value);
        // return value;
        return current_value;
    }

    uint64 VulkanFence::GetDeviceValue() const{
        uint64_t value;
        vkGetSemaphoreCounterValue(m_device->GetDevice(), timeline, &value);
        // vkGetSemaphoreCounterValue(m_device->GetDevice(), m_fence.timeline, &value);
        return value;
    }

    void VulkanFence::Wait(uint64_t _value) {
        std::unique_lock<std::mutex> _(cv_m);
        while (current_value < _value) {
            std::this_thread::yield();
            cv.wait(_); }
    }

    void VulkanFence::Notify(uint64_t _value) {
        {
            std::unique_lock<std::mutex> _(cv_m);
            current_value = std::max(current_value, _value);
        }
        cv.notify_all();
    }

    void VulkanFence::Sync(uint64_t _value) {
        std::unique_lock<std::mutex> _(cv_m);
        while (current_value < _value) { cv.wait(_); }
    }

    void VulkanFence::HostWait(uint64_t _value) {
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pValues        = &_value;
        info.pSemaphores    = &timeline;
        vkWaitSemaphores(m_device->GetDevice(), &info, UINT64_MAX);
    }

    void VulkanFence::SignalHost(uint64_t _value) {
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = timeline;
        info.value     = _value;
        vkSignalSemaphore(m_device->GetDevice(), &info);
    }

    VulkanFence::~VulkanFence() {
        VK_CHECK_NULLPTR(timeline, "Timeline semaphore is null");
        vkDestroySemaphore(m_device->GetDevice(), timeline, VK_NULL_HANDLE);
    }

    VulkanTexture::~VulkanTexture() {
        //destroy image views
        for (auto& [key, view] : m_views) { vkDestroyImageView(m_device->GetDevice(), view, VK_NULL_HANDLE); }
        if (m_alloc.image != VK_NULL_HANDLE && m_alloc.alloc != VK_NULL_HANDLE) { vmaDestroyImage(m_device->GetVmaAllocator(), m_alloc.image, m_alloc.alloc); }
        //free descriptors
        for (auto& [key, desc] : m_descriptor_indices) { m_device->GetGlobalDescriptorHeap().FreeImageDescIdx(desc); }
    }

#pragma endregion

#pragma region    raytracing
    VkGeometryTypeKHR VulkanRHIRayTracingAccelerationStructure::METoVKGeometryTypeKHR(ERayTracingGeometryType _type) {
        switch (_type) {
            case RTGT_TRIANGLES: return VK_GEOMETRY_TYPE_TRIANGLES_KHR;
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

#pragma region [ destroy override ]

    void VulkanBuffer::Destroy() {
        if (b_deferred_delete) {
            m_device->EnqueueDeferredRelease(this); return;
        }
        MoerDelete(this);
    }

    void VulkanBuffer::SetName(const std::string_view _name) {
        debug_name = _name;
        m_device->SetResourceName(uint64(m_alloc.buffer), VK_OBJECT_TYPE_BUFFER, _name);
    }

    void VulkanTexture::Destroy() {
        if (b_deferred_delete) {
            m_device->EnqueueDeferredRelease(this);
            return;
        }
        MoerDelete(this);
    }

    void VulkanTexture::SetName(const std::string_view _name) {
        debug_name = _name;
        m_device->SetResourceName(uint64(m_alloc.image), VK_OBJECT_TYPE_IMAGE, _name);
    }
}
