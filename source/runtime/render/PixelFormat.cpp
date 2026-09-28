#include "PixelFormat.h"
#include "Core.h"
#include <algorithm>
#include <limits>

namespace Moer::Render {
namespace {

constexpr PixelFormatInfo Color(uint8 _bytes_per_block, uint8 _component_count, bool _srgb = false) noexcept {
    return PixelFormatInfo{
        .block_width     = 1,
        .block_height    = 1,
        .block_depth     = 1,
        .bytes_per_block = _bytes_per_block,
        .component_count = _component_count,
        .aspect          = EPixelFormatAspect::Color,
        .srgb            = _srgb,
    };
}

constexpr PixelFormatInfo
DepthStencil(uint8 _bytes_per_block, EPixelFormatAspect _aspect, uint8 _component_count = 1) noexcept {
    return PixelFormatInfo{
        .block_width     = 1,
        .block_height    = 1,
        .block_depth     = 1,
        .bytes_per_block = _bytes_per_block,
        .component_count = _component_count,
        .aspect          = _aspect,
    };
}

constexpr PixelFormatInfo Compressed(
    uint8                   _block_width,
    uint8                   _block_height,
    uint8                   _bytes_per_block,
    uint8                   _component_count,
    EPixelFormatCompression _compression,
    bool                    _srgb = false
) noexcept {
    return PixelFormatInfo{
        .block_width     = _block_width,
        .block_height    = _block_height,
        .block_depth     = 1,
        .bytes_per_block = _bytes_per_block,
        .component_count = _component_count,
        .aspect          = EPixelFormatAspect::Color,
        .compression     = _compression,
        .srgb            = _srgb,
    };
}

} // namespace

PixelFormatInfo GetPixelFormatInfo(EPixelFormat format) noexcept {
    switch (format) {
        case PF_R4G4_UNORM_PACK8:
            return Color(1, 2);
        case PF_R4G4B4A4_UNORM_PACK16:
        case PF_B4G4R4A4_UNORM_PACK16:
        case PF_R5G5B5A1_UNORM_PACK16:
        case PF_B5G5R5A1_UNORM_PACK16:
        case PF_A1R5G5B5_UNORM_PACK16:
        case PF_A4R4G4B4_UNORM_PACK16:
        case PF_A4B4G4R4_UNORM_PACK16:
            return Color(2, 4);
        case PF_R5G6B5_UNORM_PACK16:
        case PF_B5G6R5_UNORM_PACK16:
            return Color(2, 3);

        case PF_R8_UNORM:
        case PF_R8_SNORM:
        case PF_R8_USCALED:
        case PF_R8_SSCALED:
        case PF_R8_UINT:
        case PF_R8_SINT:
            return Color(1, 1);
        case PF_R8_SRGB:
            return Color(1, 1, true);
        case PF_R8G8_UNORM:
        case PF_R8G8_SNORM:
        case PF_R8G8_USCALED:
        case PF_R8G8_SSCALED:
        case PF_R8G8_UINT:
        case PF_R8G8_SINT:
            return Color(2, 2);
        case PF_R8G8_SRGB:
            return Color(2, 2, true);
        case PF_R8G8B8_UNORM:
        case PF_R8G8B8_SNORM:
        case PF_R8G8B8_USCALED:
        case PF_R8G8B8_SSCALED:
        case PF_R8G8B8_UINT:
        case PF_R8G8B8_SINT:
        case PF_B8G8R8_UNORM:
        case PF_B8G8R8_SNORM:
        case PF_B8G8R8_USCALED:
        case PF_B8G8R8_SSCALED:
        case PF_B8G8R8_UINT:
        case PF_B8G8R8_SINT:
            return Color(3, 3);
        case PF_R8G8B8_SRGB:
        case PF_B8G8R8_SRGB:
            return Color(3, 3, true);
        case PF_R8G8B8A8_UNORM:
        case PF_R8G8B8A8_SNORM:
        case PF_R8G8B8A8_USCALED:
        case PF_R8G8B8A8_SSCALED:
        case PF_R8G8B8A8_UINT:
        case PF_R8G8B8A8_SINT:
        case PF_B8G8R8A8_UNORM:
        case PF_B8G8R8A8_SNORM:
        case PF_B8G8R8A8_USCALED:
        case PF_B8G8R8A8_SSCALED:
        case PF_B8G8R8A8_UINT:
        case PF_B8G8R8A8_SINT:
        case PF_A8B8G8R8_UNORM_PACK32:
        case PF_A8B8G8R8_SNORM_PACK32:
        case PF_A8B8G8R8_USCALED_PACK32:
        case PF_A8B8G8R8_SSCALED_PACK32:
        case PF_A8B8G8R8_UINT_PACK32:
        case PF_A8B8G8R8_SINT_PACK32:
        case PF_A2R10G10B10_UNORM_PACK32:
        case PF_A2R10G10B10_SNORM_PACK32:
        case PF_A2R10G10B10_USCALED_PACK32:
        case PF_A2R10G10B10_SSCALED_PACK32:
        case PF_A2R10G10B10_UINT_PACK32:
        case PF_A2R10G10B10_SINT_PACK32:
        case PF_A2B10G10R10_UNORM_PACK32:
        case PF_A2B10G10R10_SNORM_PACK32:
        case PF_A2B10G10R10_USCALED_PACK32:
        case PF_A2B10G10R10_SSCALED_PACK32:
        case PF_A2B10G10R10_UINT_PACK32:
        case PF_A2B10G10R10_SINT_PACK32:
            return Color(4, 4);
        case PF_R8G8B8A8_SRGB:
        case PF_B8G8R8A8_SRGB:
        case PF_A8B8G8R8_SRGB_PACK32:
            return Color(4, 4, true);

        case PF_R16_UNORM:
        case PF_R16_SNORM:
        case PF_R16_USCALED:
        case PF_R16_SSCALED:
        case PF_R16_UINT:
        case PF_R16_SINT:
        case PF_R16_SFLOAT:
        case PF_R10X6_UNORM_PACK16:
        case PF_R12X4_UNORM_PACK16:
            return Color(2, 1);
        case PF_R16G16_UNORM:
        case PF_R16G16_SNORM:
        case PF_R16G16_USCALED:
        case PF_R16G16_SSCALED:
        case PF_R16G16_UINT:
        case PF_R16G16_SINT:
        case PF_R16G16_SFLOAT:
        case PF_R10X6G10X6_UNORM_2PACK16:
        case PF_R12X4G12X4_UNORM_2PACK16:
            return Color(4, 2);
        case PF_R16G16B16_UNORM:
        case PF_R16G16B16_SNORM:
        case PF_R16G16B16_USCALED:
        case PF_R16G16B16_SSCALED:
        case PF_R16G16B16_UINT:
        case PF_R16G16B16_SINT:
        case PF_R16G16B16_SFLOAT:
            return Color(6, 3);
        case PF_R16G16B16A16_UNORM:
        case PF_R16G16B16A16_SNORM:
        case PF_R16G16B16A16_USCALED:
        case PF_R16G16B16A16_SSCALED:
        case PF_R16G16B16A16_UINT:
        case PF_R16G16B16A16_SINT:
        case PF_R16G16B16A16_SFLOAT:
        case PF_R10X6G10X6B10X6A10X6_UNORM_4PACK16:
        case PF_R12X4G12X4B12X4A12X4_UNORM_4PACK16:
            return Color(8, 4);

        case PF_R32_UINT:
        case PF_R32_SINT:
        case PF_R32_SFLOAT:
            return Color(4, 1);
        case PF_R32G32_UINT:
        case PF_R32G32_SINT:
        case PF_R32G32_SFLOAT:
            return Color(8, 2);
        case PF_R32G32B32_UINT:
        case PF_R32G32B32_SINT:
        case PF_R32G32B32_SFLOAT:
            return Color(12, 3);
        case PF_R32G32B32A32_UINT:
        case PF_R32G32B32A32_SINT:
        case PF_R32G32B32A32_SFLOAT:
            return Color(16, 4);
        case PF_R64_UINT:
        case PF_R64_SINT:
        case PF_R64_SFLOAT:
            return Color(8, 1);
        case PF_R64G64_UINT:
        case PF_R64G64_SINT:
        case PF_R64G64_SFLOAT:
            return Color(16, 2);
        case PF_R64G64B64_UINT:
        case PF_R64G64B64_SINT:
        case PF_R64G64B64_SFLOAT:
            return Color(24, 3);
        case PF_R64G64B64A64_UINT:
        case PF_R64G64B64A64_SINT:
        case PF_R64G64B64A64_SFLOAT:
            return Color(32, 4);
        case PF_B10G11R11_UFLOAT_PACK32:
        case PF_E5B9G9R9_UFLOAT_PACK32:
            return Color(4, 3);

        case PF_D16_UNORM:
            return DepthStencil(2, EPixelFormatAspect::Depth);
        case PF_X8_D24_UNORM_PACK32:
        case PF_D32_SFLOAT:
            return DepthStencil(4, EPixelFormatAspect::Depth);
        case PF_S8_UINT:
            return DepthStencil(1, EPixelFormatAspect::Stencil);
        case PF_D16_UNORM_S8_UINT:
        case PF_D24_UNORM_S8_UINT:
            return DepthStencil(4, EPixelFormatAspect::DepthStencil, 2);
        case PF_D32_SFLOAT_S8_UINT:
            return DepthStencil(8, EPixelFormatAspect::DepthStencil, 2);

        case PF_BC1_RGB_UNORM_BLOCK:
            return Compressed(4, 4, 8, 3, EPixelFormatCompression::BC);
        case PF_BC1_RGB_SRGB_BLOCK:
            return Compressed(4, 4, 8, 3, EPixelFormatCompression::BC, true);
        case PF_BC1_RGBA_UNORM_BLOCK:
            return Compressed(4, 4, 8, 4, EPixelFormatCompression::BC);
        case PF_BC1_RGBA_SRGB_BLOCK:
            return Compressed(4, 4, 8, 4, EPixelFormatCompression::BC, true);
        case PF_BC2_UNORM_BLOCK:
        case PF_BC3_UNORM_BLOCK:
        case PF_BC7_UNORM_BLOCK:
            return Compressed(4, 4, 16, 4, EPixelFormatCompression::BC);
        case PF_BC2_SRGB_BLOCK:
        case PF_BC3_SRGB_BLOCK:
        case PF_BC7_SRGB_BLOCK:
            return Compressed(4, 4, 16, 4, EPixelFormatCompression::BC, true);
        case PF_BC4_UNORM_BLOCK:
        case PF_BC4_SNORM_BLOCK:
            return Compressed(4, 4, 8, 1, EPixelFormatCompression::BC);
        case PF_BC5_UNORM_BLOCK:
        case PF_BC5_SNORM_BLOCK:
            return Compressed(4, 4, 16, 2, EPixelFormatCompression::BC);
        case PF_BC6H_UFLOAT_BLOCK:
        case PF_BC6H_SFLOAT_BLOCK:
            return Compressed(4, 4, 16, 3, EPixelFormatCompression::BC);

        case PF_ETC2_R8G8B8_UNORM_BLOCK:
            return Compressed(4, 4, 8, 3, EPixelFormatCompression::ETC2_EAC);
        case PF_ETC2_R8G8B8_SRGB_BLOCK:
            return Compressed(4, 4, 8, 3, EPixelFormatCompression::ETC2_EAC, true);
        case PF_ETC2_R8G8B8A1_UNORM_BLOCK:
            return Compressed(4, 4, 8, 4, EPixelFormatCompression::ETC2_EAC);
        case PF_ETC2_R8G8B8A1_SRGB_BLOCK:
            return Compressed(4, 4, 8, 4, EPixelFormatCompression::ETC2_EAC, true);
        case PF_ETC2_R8G8B8A8_UNORM_BLOCK:
            return Compressed(4, 4, 16, 4, EPixelFormatCompression::ETC2_EAC);
        case PF_ETC2_R8G8B8A8_SRGB_BLOCK:
            return Compressed(4, 4, 16, 4, EPixelFormatCompression::ETC2_EAC, true);
        case PF_EAC_R11_UNORM_BLOCK:
        case PF_EAC_R11_SNORM_BLOCK:
            return Compressed(4, 4, 8, 1, EPixelFormatCompression::ETC2_EAC);
        case PF_EAC_R11G11_UNORM_BLOCK:
        case PF_EAC_R11G11_SNORM_BLOCK:
            return Compressed(4, 4, 16, 2, EPixelFormatCompression::ETC2_EAC);

#define MOER_ASTC_CASE(width, height)                                           \
    case PF_ASTC_##width##x##height##_UNORM_BLOCK:                              \
    case PF_ASTC_##width##x##height##_SFLOAT_BLOCK:                             \
        return Compressed(width, height, 16, 4, EPixelFormatCompression::ASTC); \
    case PF_ASTC_##width##x##height##_SRGB_BLOCK:                               \
        return Compressed(width, height, 16, 4, EPixelFormatCompression::ASTC, true)

            MOER_ASTC_CASE(4, 4);
            MOER_ASTC_CASE(5, 4);
            MOER_ASTC_CASE(5, 5);
            MOER_ASTC_CASE(6, 5);
            MOER_ASTC_CASE(6, 6);
            MOER_ASTC_CASE(8, 5);
            MOER_ASTC_CASE(8, 6);
            MOER_ASTC_CASE(8, 8);
            MOER_ASTC_CASE(10, 5);
            MOER_ASTC_CASE(10, 6);
            MOER_ASTC_CASE(10, 8);
            MOER_ASTC_CASE(10, 10);
            MOER_ASTC_CASE(12, 10);
            MOER_ASTC_CASE(12, 12);

#undef MOER_ASTC_CASE

        default:
            // No single-plane block footprint is defined for the remaining
            // video, PVRTC, and vendor-specific formats. Do not guess a size.
            return {};
    }
}

uint32 GetMaxPixelFormatRowsPerChunk(EPixelFormat format, uint32 width, uint64 byte_limit) noexcept {
    const PixelFormatInfo info = GetPixelFormatInfo(format);
    if (!info.IsDefined() || info.block_depth != 1 || width == 0 || byte_limit == 0) {
        return 0;
    }

    const uint64 blocks_per_row = (static_cast<uint64>(width) + info.block_width - 1) / info.block_width;
    const uint64 bytes_per_block_row = blocks_per_row * info.bytes_per_block;
    const uint64 max_block_rows = byte_limit / bytes_per_block_row;
    const uint64 representable_block_rows = std::numeric_limits<uint32>::max() / info.block_height;
    return static_cast<uint32>(std::min(max_block_rows, representable_block_rows) * info.block_height);
}

bool IsPixelFormatBC(EPixelFormat _format) {
    return GetPixelFormatInfo(_format).compression == EPixelFormatCompression::BC;
}

uint64 GetSizeFromImageFormat(EPixelFormat _format, const uint3 _size) {
    return GetSizeFromPixelFormat(_format, _size);
}

uint64 GetByteFromPixelFormat(EPixelFormat format) {
    const PixelFormatInfo info = GetPixelFormatInfo(format);
    MOER_ASSERT(
        info.IsDefined(),
        "Unsupported pixel format in GetByteFromPixelFormat: {}",
        static_cast<std::uint32_t>(format)
    );
    MOER_ASSERT(
        !info.IsCompressed(),
        "Compressed pixel format does not have a fixed byte count per pixel: {}",
        static_cast<std::uint32_t>(format)
    );
    return info.bytes_per_block;
}

uint64 GetChannelFromPixelFormat(EPixelFormat format) {
    const PixelFormatInfo info = GetPixelFormatInfo(format);
    MOER_ASSERT(
        info.IsDefined(),
        "Unsupported pixel format in GetChannelFromPixelFormat: {}",
        static_cast<std::uint32_t>(format)
    );
    return info.component_count;
}

uint64 GetSizeFromPixelFormat(EPixelFormat format, const uint3 size) {
    const PixelFormatInfo info = GetPixelFormatInfo(format);
    MOER_ASSERT(
        info.IsDefined(),
        "Unsupported pixel format in GetSizeFromPixelFormat: {}",
        static_cast<std::uint32_t>(format)
    );

    const uint64 block_count_x = (static_cast<uint64>(size.x) + info.block_width - 1) / info.block_width;
    const uint64 block_count_y = (static_cast<uint64>(size.y) + info.block_height - 1) / info.block_height;
    const uint64 block_count_z = (static_cast<uint64>(size.z) + info.block_depth - 1) / info.block_depth;
    return block_count_x * block_count_y * block_count_z * info.bytes_per_block;
}

} // namespace Moer::Render
