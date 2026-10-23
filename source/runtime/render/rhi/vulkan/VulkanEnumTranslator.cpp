#include "VulkanEnumTranslator.h"
#include "log/LogSystem.h"

namespace Moer::Render {

namespace {
constexpr VulkanPixelFormatInfo g_platform_pixel_formats[static_cast<uint>(PF_Num)]{
    {0, VK_FORMAT_UNDEFINED},
    {1, VK_FORMAT_R4G4_UNORM_PACK8},
    {2, VK_FORMAT_R4G4B4A4_UNORM_PACK16},
    {2, VK_FORMAT_B4G4R4A4_UNORM_PACK16},
    {2, VK_FORMAT_R5G6B5_UNORM_PACK16},
    {2, VK_FORMAT_B5G6R5_UNORM_PACK16},
    {2, VK_FORMAT_R5G5B5A1_UNORM_PACK16},
    {2, VK_FORMAT_B5G5R5A1_UNORM_PACK16},
    {2, VK_FORMAT_A1R5G5B5_UNORM_PACK16},
    {1, VK_FORMAT_R8_UNORM},
    {1, VK_FORMAT_R8_SNORM},
    {1, VK_FORMAT_R8_USCALED},
    {1, VK_FORMAT_R8_SSCALED},
    {1, VK_FORMAT_R8_UINT},
    {1, VK_FORMAT_R8_SINT},
    {1, VK_FORMAT_R8_SRGB},
    {2, VK_FORMAT_R8G8_UNORM},
    {2, VK_FORMAT_R8G8_SNORM},
    {2, VK_FORMAT_R8G8_USCALED},
    {2, VK_FORMAT_R8G8_SSCALED},
    {2, VK_FORMAT_R8G8_UINT},
    {2, VK_FORMAT_R8G8_SINT},
    {2, VK_FORMAT_R8G8_SRGB},
    {3, VK_FORMAT_R8G8B8_UNORM},
    {3, VK_FORMAT_R8G8B8_SNORM},
    {3, VK_FORMAT_R8G8B8_USCALED},
    {3, VK_FORMAT_R8G8B8_SSCALED},
    {3, VK_FORMAT_R8G8B8_UINT},
    {3, VK_FORMAT_R8G8B8_SINT},
    {3, VK_FORMAT_R8G8B8_SRGB},
    {3, VK_FORMAT_B8G8R8_UNORM},
    {3, VK_FORMAT_B8G8R8_SNORM},
    {3, VK_FORMAT_B8G8R8_USCALED},
    {3, VK_FORMAT_B8G8R8_SSCALED},
    {3, VK_FORMAT_B8G8R8_UINT},
    {3, VK_FORMAT_B8G8R8_SINT},
    {3, VK_FORMAT_B8G8R8_SRGB},
    {4, VK_FORMAT_R8G8B8A8_UNORM},
    {4, VK_FORMAT_R8G8B8A8_SNORM},
    {4, VK_FORMAT_R8G8B8A8_USCALED},
    {4, VK_FORMAT_R8G8B8A8_SSCALED},
    {4, VK_FORMAT_R8G8B8A8_UINT},
    {4, VK_FORMAT_R8G8B8A8_SINT},
    {4, VK_FORMAT_R8G8B8A8_SRGB},
    {4, VK_FORMAT_B8G8R8A8_UNORM},
    {4, VK_FORMAT_B8G8R8A8_SNORM},
    {4, VK_FORMAT_B8G8R8A8_USCALED},
    {4, VK_FORMAT_B8G8R8A8_SSCALED},
    {4, VK_FORMAT_B8G8R8A8_UINT},
    {4, VK_FORMAT_B8G8R8A8_SINT},
    {4, VK_FORMAT_B8G8R8A8_SRGB},
    {4, VK_FORMAT_A8B8G8R8_UNORM_PACK32},
    {4, VK_FORMAT_A8B8G8R8_SNORM_PACK32},
    {4, VK_FORMAT_A8B8G8R8_USCALED_PACK32},
    {4, VK_FORMAT_A8B8G8R8_SSCALED_PACK32},
    {4, VK_FORMAT_A8B8G8R8_UINT_PACK32},
    {4, VK_FORMAT_A8B8G8R8_SINT_PACK32},
    {4, VK_FORMAT_A8B8G8R8_SRGB_PACK32},
    {4, VK_FORMAT_A2R10G10B10_UNORM_PACK32},
    {4, VK_FORMAT_A2R10G10B10_SNORM_PACK32},
    {4, VK_FORMAT_A2R10G10B10_USCALED_PACK32},
    {4, VK_FORMAT_A2R10G10B10_SSCALED_PACK32},
    {4, VK_FORMAT_A2R10G10B10_UINT_PACK32},
    {4, VK_FORMAT_A2R10G10B10_SINT_PACK32},
    {4, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
    {4, VK_FORMAT_A2B10G10R10_SNORM_PACK32},
    {4, VK_FORMAT_A2B10G10R10_USCALED_PACK32},
    {4, VK_FORMAT_A2B10G10R10_SSCALED_PACK32},
    {4, VK_FORMAT_A2B10G10R10_UINT_PACK32},
    {4, VK_FORMAT_A2B10G10R10_SINT_PACK32},
    {2, VK_FORMAT_R16_UNORM},
    {2, VK_FORMAT_R16_SNORM},
    {2, VK_FORMAT_R16_USCALED},
    {2, VK_FORMAT_R16_SSCALED},
    {2, VK_FORMAT_R16_UINT},
    {2, VK_FORMAT_R16_SINT},
    {2, VK_FORMAT_R16_SFLOAT},
    {4, VK_FORMAT_R16G16_UNORM},
    {4, VK_FORMAT_R16G16_SNORM},
    {4, VK_FORMAT_R16G16_USCALED},
    {4, VK_FORMAT_R16G16_SSCALED},
    {4, VK_FORMAT_R16G16_UINT},
    {4, VK_FORMAT_R16G16_SINT},
    {4, VK_FORMAT_R16G16_SFLOAT},
    {6, VK_FORMAT_R16G16B16_UNORM},
    {6, VK_FORMAT_R16G16B16_SNORM},
    {6, VK_FORMAT_R16G16B16_USCALED},
    {6, VK_FORMAT_R16G16B16_SSCALED},
    {6, VK_FORMAT_R16G16B16_UINT},
    {6, VK_FORMAT_R16G16B16_SINT},
    {6, VK_FORMAT_R16G16B16_SFLOAT},
    {8, VK_FORMAT_R16G16B16A16_UNORM},
    {8, VK_FORMAT_R16G16B16A16_SNORM},
    {8, VK_FORMAT_R16G16B16A16_USCALED},
    {8, VK_FORMAT_R16G16B16A16_SSCALED},
    {8, VK_FORMAT_R16G16B16A16_UINT},
    {8, VK_FORMAT_R16G16B16A16_SINT},
    {8, VK_FORMAT_R16G16B16A16_SFLOAT},
    {4, VK_FORMAT_R32_UINT},
    {4, VK_FORMAT_R32_SINT},
    {4, VK_FORMAT_R32_SFLOAT},
    {8, VK_FORMAT_R32G32_UINT},
    {8, VK_FORMAT_R32G32_SINT},
    {8, VK_FORMAT_R32G32_SFLOAT},
    {12, VK_FORMAT_R32G32B32_UINT},
    {12, VK_FORMAT_R32G32B32_SINT},
    {12, VK_FORMAT_R32G32B32_SFLOAT},
    {16, VK_FORMAT_R32G32B32A32_UINT},
    {16, VK_FORMAT_R32G32B32A32_SINT},
    {16, VK_FORMAT_R32G32B32A32_SFLOAT},
    {8, VK_FORMAT_R64_UINT},
    {8, VK_FORMAT_R64_SINT},
    {8, VK_FORMAT_R64_SFLOAT},
    {16, VK_FORMAT_R64G64_UINT},
    {16, VK_FORMAT_R64G64_SINT},
    {16, VK_FORMAT_R64G64_SFLOAT},
    {24, VK_FORMAT_R64G64B64_UINT},
    {24, VK_FORMAT_R64G64B64_SINT},
    {24, VK_FORMAT_R64G64B64_SFLOAT},
    {32, VK_FORMAT_R64G64B64A64_UINT},
    {32, VK_FORMAT_R64G64B64A64_SINT},
    {32, VK_FORMAT_R64G64B64A64_SFLOAT},
    {4, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
    {4, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32},
    {2, VK_FORMAT_D16_UNORM},
    {4, VK_FORMAT_X8_D24_UNORM_PACK32},
    {4, VK_FORMAT_D32_SFLOAT},
    {1, VK_FORMAT_S8_UINT},
    {4, VK_FORMAT_D16_UNORM_S8_UINT},
    {4, VK_FORMAT_D24_UNORM_S8_UINT},
    {8, VK_FORMAT_D32_SFLOAT_S8_UINT},
    {8, VK_FORMAT_BC1_RGB_UNORM_BLOCK},
    {8, VK_FORMAT_BC1_RGB_SRGB_BLOCK},
    {8, VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
    {8, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
    {16, VK_FORMAT_BC2_UNORM_BLOCK},
    {16, VK_FORMAT_BC2_SRGB_BLOCK},
    {16, VK_FORMAT_BC3_UNORM_BLOCK},
    {16, VK_FORMAT_BC3_SRGB_BLOCK},
    {8, VK_FORMAT_BC4_UNORM_BLOCK},
    {8, VK_FORMAT_BC4_SNORM_BLOCK},
    {16, VK_FORMAT_BC5_UNORM_BLOCK},
    {16, VK_FORMAT_BC5_SNORM_BLOCK},
    {16, VK_FORMAT_BC6H_UFLOAT_BLOCK},
    {16, VK_FORMAT_BC6H_SFLOAT_BLOCK},
    {16, VK_FORMAT_BC7_UNORM_BLOCK},
    {16, VK_FORMAT_BC7_SRGB_BLOCK},
    {8, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK},
    {8, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK},
    {8, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK},
    {8, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK},
    {16, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK},
    {16, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK},
    {8, VK_FORMAT_EAC_R11_UNORM_BLOCK},
    {8, VK_FORMAT_EAC_R11_SNORM_BLOCK},
    {16, VK_FORMAT_EAC_R11G11_UNORM_BLOCK},
    {16, VK_FORMAT_EAC_R11G11_SNORM_BLOCK},
    {16, VK_FORMAT_ASTC_4x4_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_4x4_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_5x4_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_5x4_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_5x5_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_5x5_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_6x5_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_6x5_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_6x6_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_6x6_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_8x5_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_8x5_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_8x6_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_8x6_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_8x8_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_8x8_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_10x5_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_10x5_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_10x6_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_10x6_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_10x8_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_10x8_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_10x10_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_10x10_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_12x10_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_12x10_SRGB_BLOCK},
    {16, VK_FORMAT_ASTC_12x12_UNORM_BLOCK},
    {16, VK_FORMAT_ASTC_12x12_SRGB_BLOCK},
    /*= 1000156000*/ {4, VK_FORMAT_G8B8G8R8_422_UNORM},
    /*= 1000156001*/ {4, VK_FORMAT_B8G8R8G8_422_UNORM},
    /*= 1000156002*/ {3, VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM},
    /*= 1000156003*/ {3, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM},
    /*= 1000156004*/ {3, VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM},
    /*= 1000156005*/ {3, VK_FORMAT_G8_B8R8_2PLANE_422_UNORM},
    /*= 1000156006*/ {3, VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM},
    /*= 1000156007*/ {2, VK_FORMAT_R10X6_UNORM_PACK16},
    /*= 1000156008*/ {4, VK_FORMAT_R10X6G10X6_UNORM_2PACK16},
    /*= 1000156009*/ {8, VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16},
    /*= 1000156010*/ {4, VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16},
    /*= 1000156011*/ {4, VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16},
    /*= 1000156012*/ {3, VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16},
    /*= 1000156013*/ {3, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16},
    /*= 1000156014*/ {3, VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16},
    /*= 1000156015*/ {3, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16},
    /*= 1000156016*/ {3, VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16},
    /*= 1000156017*/ {2, VK_FORMAT_R12X4_UNORM_PACK16},
    /*= 1000156018*/ {4, VK_FORMAT_R12X4G12X4_UNORM_2PACK16},
    /*= 1000156019*/ {8, VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16},
    /*= 1000156020*/ {4, VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16},
    /*= 1000156021*/ {4, VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16},
    /*= 1000156022*/ {3, VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16},
    /*= 1000156023*/ {3, VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16},
    /*= 1000156024*/ {3, VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16},
    /*= 1000156025*/ {3, VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16},
    /*= 1000156026*/ {3, VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16},
    /*= 1000156027*/ {8, VK_FORMAT_G16B16G16R16_422_UNORM},
    /*= 1000156028*/ {8, VK_FORMAT_B16G16R16G16_422_UNORM},
    /*= 1000156029*/ {3, VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM},
    /*= 1000156030*/ {3, VK_FORMAT_G16_B16R16_2PLANE_420_UNORM},
    /*= 1000156031*/ {3, VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM},
    /*= 1000156032*/ {3, VK_FORMAT_G16_B16R16_2PLANE_422_UNORM},
    /*= 1000156033*/ {3, VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM},
    /*= 1000330000*/ {3, VK_FORMAT_G8_B8R8_2PLANE_444_UNORM},
    /*= 1000330001*/ {3, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16},
    /*= 1000330002*/ {3, VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16},
    /*= 1000330003*/ {3, VK_FORMAT_G16_B16R16_2PLANE_444_UNORM},
    /*= 1000340000*/ {2, VK_FORMAT_A4R4G4B4_UNORM_PACK16},
    /*= 1000340001*/ {2, VK_FORMAT_A4B4G4R4_UNORM_PACK16},
    /*= 1000066000*/ {16, VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK},
    /*= 1000066001*/ {16, VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK},
    /*= 1000066002*/ {16, VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK},
    /*= 1000066003*/ {16, VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK},
    /*= 1000066004*/ {16, VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK},
    /*= 1000066005*/ {16, VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK},
    /*= 1000066006*/ {16, VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK},
    /*= 1000066007*/ {16, VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK},
    /*= 1000066008*/ {16, VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK},
    /*= 1000066009*/ {16, VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK},
    /*= 1000066010*/ {16, VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK},
    /*= 1000066011*/ {16, VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK},
    /*= 1000066012*/ {16, VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK},
    /*= 1000066013*/ {16, VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK},
    /*= 1000054000*/ {8, VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG},
    /*= 1000054001*/ {8, VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG},
    /*= 1000054002*/ {8, VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG},
    /*= 1000054003*/ {8, VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG},
    /*= 1000054004*/ {8, VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG},
    /*= 1000054005*/ {8, VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG},
    /*= 1000054006*/ {8, VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG},
    /*= 1000054007*/ {8, VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG},
    /*= 1000464000*/ {8, VK_FORMAT_R16G16_S10_5_NV}
};

static_assert(g_platform_pixel_formats[PF_R8G8B8A8_UNORM].format == VK_FORMAT_R8G8B8A8_UNORM);
static_assert(g_platform_pixel_formats[PF_G8B8G8R8_422_UNORM].format == VK_FORMAT_G8B8G8R8_422_UNORM);
static_assert(g_platform_pixel_formats[PF_R16G16_S10_5_NV].format == VK_FORMAT_R16G16_S10_5_NV);
static_assert(g_platform_pixel_formats[PF_ETC2_R8G8B8A8_UNORM_BLOCK].stride == 16);

} // namespace

const VulkanPixelFormatInfo& VulkanEnumTranslator::GetPixelFormatInfo(EPixelFormat _format) {
    return g_platform_pixel_formats[static_cast<uint>(_format)];
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
    const uint32_t format_index = static_cast<uint32_t>(_format);
    if (format_index >= static_cast<uint32_t>(PF_Num)) {
        LOG_CRITICAL("Unsupported pixel format: {}", static_cast<uint32_t>(_format));
        return VK_FORMAT_MAX_ENUM;
    }

    const VkFormat vk_format = g_platform_pixel_formats[format_index].format;
    if (vk_format == VK_FORMAT_UNDEFINED && _format != PF_UNDEFINED) {
        LOG_CRITICAL("Pixel format {} has no Vulkan mapping", format_index);
        return VK_FORMAT_MAX_ENUM;
    }
    return vk_format;
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
    for (uint32_t format_index = 0; format_index < static_cast<uint32_t>(PF_Num); ++format_index) {
        if (g_platform_pixel_formats[format_index].format == _format) {
            return static_cast<EPixelFormat>(format_index);
        }
    }
    return PF_UNDEFINED;
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
    translate_flag(
        ERHIAccessFlags::COMMAND_PREPROCESS_READ_BIT_NV, VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_NV
    );
    translate_flag(
        ERHIAccessFlags::COMMAND_PREPROCESS_WRITE_BIT_NV, VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_NV
    );
    translate_flag(
        ERHIAccessFlags::FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT,
        VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR
    );
    translate_flag(
        ERHIAccessFlags::FRAGMENT_DENSITY_MAP_READ_BIT_EXT, VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT
    );
    translate_flag(
        ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
    );
    translate_flag(
        ERHIAccessFlags::ACCELERATION_STRUCTURE_WRITE_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
    );
    translate_flag(ERHIAccessFlags::SHADER_SAMPLED_READ, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    translate_flag(ERHIAccessFlags::SHADER_RESOURCE_VIEW, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    translate_flag(
        ERHIAccessFlags::UNORDERED_ACCESS_VIEW,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
    );
    translate_flag(
        ERHIAccessFlags::TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT,
        VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT
    );
    translate_flag(
        ERHIAccessFlags::TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
        VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT
    );
    translate_flag(
        ERHIAccessFlags::TRANSFORM_FEEDBACK_WRITE_BIT_EXT, VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT
    );
    translate_flag(
        ERHIAccessFlags::CONDITIONAL_RENDERING_READ_BIT_EXT, VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT
    );
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

VkDescriptorType VulkanEnumTranslator::METoVKDescriptorType(
    EShaderParameterType           _type,
    EShaderCodeResourceBindingType _binding_type
) {

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
    auto is_acceleration_structure = [](EShaderCodeResourceBindingType type) {
        return type == EShaderCodeResourceBindingType::RAYTRACING_ACCELERATION_STRUCTURE;
    };
    switch (_type) {
        case EShaderParameterType::CBV:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case EShaderParameterType::SAMPLER:
        case EShaderParameterType::BINDLESS_SAMPLER_INDEX: // MARK...
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case EShaderParameterType::SRV:
            if (is_texture(_binding_type))
                return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            if (is_buffer(_binding_type))
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (is_acceleration_structure(_binding_type))
                return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            LOG_ERROR("Unsupported SRV type: {}", ToString(_binding_type));
        case EShaderParameterType::UAV:
            if (is_texture(_binding_type))
                return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (is_buffer(_binding_type))
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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
        case EShaderType::ST_HULL:
            return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case EShaderType::ST_DOMAIN:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
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

VkFilter VulkanEnumTranslator::METoVKMinMagFilterMode(ESamplerFilter _filter) {
    return VulkanEnumTranslator::METoVKImageFilter(_filter);
}

VkSamplerMipmapMode VulkanEnumTranslator::METoVKMipmapMode(ESamplerFilter _filter) {
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

VkSamplerAddressMode VulkanEnumTranslator::METoVKWrapMode(ESamplerAddressMode _address_mode) {
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

VkGeometryFlagsKHR VulkanEnumTranslator::METoVKGeometryFlags(ERayTracingGeometryFlags _flags) {
    VkGeometryFlagsKHR vk_flags = 0;

    auto translate_flag = [&vk_flags, &_flags](
                              ERayTracingGeometryFlags _search_me_flags,
                              VkGeometryFlagsKHR       _added_if_found,
                              VkGeometryFlagsKHR       _added_if_not_found = 0
                          ) {
        const bool has_flag = (_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    translate_flag(ERayTracingGeometryFlags::GEOMETRY_OPAQUE, VK_GEOMETRY_OPAQUE_BIT_KHR);
    translate_flag(
        ERayTracingGeometryFlags::NO_DUPLICATE_ANY_HIT_INVOCATION,
        VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR
    );
    return vk_flags;
}

VkGeometryTypeKHR VulkanEnumTranslator::METoVKGeometryType(ERayTracingGeometryType _type) {
    switch (_type) {
        case RTGT_TRIANGLES:
            return VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        case RTGT_AABBS:
            return VK_GEOMETRY_TYPE_AABBS_KHR;
        default:
            LOG_CRITICAL("Unsupported ray tracing geometry type: {}", static_cast<uint32_t>(_type));
            return VK_GEOMETRY_TYPE_MAX_ENUM_KHR;
    }
}

VkBuildAccelerationStructureFlagsKHR VulkanEnumTranslator::METoVKAccelerationStructureBuildType(
    ERayTracingAccelerationStructureBuildFlags _flags
) {
    VkBuildAccelerationStructureFlagsKHR vk_flags = 0;

    auto translate_flag = [&vk_flags, &_flags](
                              ERayTracingAccelerationStructureBuildFlags _search_me_flags,
                              VkBuildAccelerationStructureFlagsKHR       _added_if_found,
                              VkBuildAccelerationStructureFlagsKHR       _added_if_not_found = 0
                          ) {
        const bool has_flag = (_flags & _search_me_flags) == _search_me_flags;
        vk_flags |= has_flag ? _added_if_found : _added_if_not_found;
    };

    translate_flag(
        ERayTracingAccelerationStructureBuildFlags::ALLOW_UPDATE,
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
    );
    translate_flag(
        ERayTracingAccelerationStructureBuildFlags::ALLOW_COMPACTION,
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR
    );
    translate_flag(
        ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE,
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
    );
    translate_flag(
        ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_BUILD,
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
    );
    translate_flag(
        ERayTracingAccelerationStructureBuildFlags::MINIMIZE_MEMORY,
        VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR
    );
    return vk_flags;
}

VkBuildAccelerationStructureModeKHR
VulkanEnumTranslator::METoVKBuildAccelerationStructureMode(ERaytracingBuildMode _mode) {
    switch (_mode) {
        case ERaytracingBuildMode::BUILD:
            return VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        case ERaytracingBuildMode::UPDATE:
            return VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    }
    assert(false && "unsupported build mode");
}

} // namespace Moer::Render
