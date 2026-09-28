#include "PixelFormat.h"

#include <stdexcept>

namespace {

void Require(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

} // namespace

int main() {
    using namespace Moer::Render;

    const PixelFormatInfo rgba8 = GetPixelFormatInfo(PF_R8G8B8A8_UNORM);
    Require(rgba8.IsDefined(), "RGBA8 metadata is missing");
    Require(rgba8.aspect == EPixelFormatAspect::Color, "RGBA8 must be a color format");
    Require(rgba8.block_width == 1 && rgba8.block_height == 1, "RGBA8 block extent is invalid");
    Require(rgba8.bytes_per_block == 4, "RGBA8 byte size is invalid");
    Require(rgba8.component_count == 4, "RGBA8 component count is invalid");
    Require(!rgba8.srgb, "RGBA8 UNORM must be linear");

    const PixelFormatInfo rgba8_srgb = GetPixelFormatInfo(PF_R8G8B8A8_SRGB);
    Require(rgba8_srgb.srgb, "RGBA8 SRGB metadata must preserve the transfer function");

    const PixelFormatInfo depth_stencil = GetPixelFormatInfo(PF_D32_SFLOAT_S8_UINT);
    Require(
        depth_stencil.aspect == EPixelFormatAspect::DepthStencil,
        "D32S8 must expose both depth and stencil aspects"
    );
    Require(depth_stencil.bytes_per_block == 8, "D32S8 texel block size is invalid");

    const PixelFormatInfo bc1 = GetPixelFormatInfo(PF_BC1_RGBA_UNORM_BLOCK);
    Require(bc1.compression == EPixelFormatCompression::BC, "BC1 compression family is invalid");
    Require(
        bc1.block_width == 4 && bc1.block_height == 4 && bc1.bytes_per_block == 8,
        "BC1 block layout is invalid"
    );
    Require(
        GetSizeFromPixelFormat(PF_BC1_RGBA_UNORM_BLOCK, Moer::uint3{16, 8, 1}) == 64,
        "BC1 image size calculation is invalid"
    );
    Require(
        GetSizeFromPixelFormat(PF_BC1_RGBA_UNORM_BLOCK, Moer::uint3{1, 1, 1}) == 8,
        "A small BC mip must still occupy one complete block"
    );
    Require(
        GetMaxPixelFormatRowsPerChunk(PF_BC1_RGBA_UNORM_BLOCK, 16, 64) == 8,
        "BC chunk rows must stay aligned to four-pixel blocks"
    );

    const PixelFormatInfo astc = GetPixelFormatInfo(PF_ASTC_10x6_SFLOAT_BLOCK);
    Require(astc.compression == EPixelFormatCompression::ASTC, "ASTC compression family is invalid");
    Require(
        astc.block_width == 10 && astc.block_height == 6 && astc.bytes_per_block == 16,
        "ASTC block layout is invalid"
    );
    Require(
        GetSizeFromPixelFormat(PF_ASTC_10x6_SFLOAT_BLOCK, Moer::uint3{11, 7, 1}) == 64,
        "ASTC image size calculation must round up to complete blocks"
    );
    Require(
        GetMaxPixelFormatRowsPerChunk(PF_ASTC_10x6_SFLOAT_BLOCK, 11, 64) == 12,
        "ASTC chunk rows must be aligned to six-pixel blocks"
    );
    Require(
        GetSizeFromPixelFormat(PF_ASTC_10x6_SFLOAT_BLOCK, Moer::uint3{11, 12, 1}) * 2 +
            GetSizeFromPixelFormat(PF_ASTC_10x6_SFLOAT_BLOCK, Moer::uint3{11, 1, 1}) ==
            GetSizeFromPixelFormat(PF_ASTC_10x6_SFLOAT_BLOCK, Moer::uint3{11, 25, 1}),
        "Aligned ASTC chunks and the final partial block row must preserve the image size"
    );

    const PixelFormatInfo etc2_rgba8 = GetPixelFormatInfo(PF_ETC2_R8G8B8A8_UNORM_BLOCK);
    Require(etc2_rgba8.bytes_per_block == 16, "ETC2 RGBA8 blocks must occupy 16 bytes");
    Require(
        GetSizeFromPixelFormat(PF_ETC2_R8G8B8A8_UNORM_BLOCK, Moer::uint3{4, 4, 1}) == 16,
        "A complete ETC2 RGBA8 block must occupy 16 bytes"
    );
    Require(
        GetMaxPixelFormatRowsPerChunk(PF_ETC2_R8G8B8A8_UNORM_BLOCK, 11, 64) == 4,
        "ETC2 chunk rows must be aligned to four-pixel blocks"
    );
    Require(
        GetMaxPixelFormatRowsPerChunk(PF_ETC2_R8G8B8A8_UNORM_BLOCK, 11, 47) == 0,
        "A chunk smaller than one ETC2 block row must be rejected"
    );

    Require(!GetPixelFormatInfo(PF_UNDEFINED).IsDefined(), "Undefined format must have no layout");
    Require(
        !GetPixelFormatInfo(PF_G8_B8R8_2PLANE_420_UNORM).IsDefined(),
        "Multi-plane formats need explicit plane layout support"
    );
    Require(
        GetMaxPixelFormatRowsPerChunk(PF_G8_B8R8_2PLANE_420_UNORM, 16, 64) == 0,
        "Multi-plane formats must not produce IO chunk sizes"
    );
    Require(
        !GetPixelFormatInfo(PF_PVRTC1_4BPP_UNORM_BLOCK_IMG).IsDefined(),
        "PVRTC needs minimum-surface layout support"
    );
    Require(
        GetMaxPixelFormatRowsPerChunk(PF_PVRTC1_4BPP_UNORM_BLOCK_IMG, 16, 64) == 0,
        "Unsupported formats must not produce IO chunk sizes"
    );
    return 0;
}
