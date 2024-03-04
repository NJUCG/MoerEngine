#include "KtxImageHelper.h"

#include "math/Base.h"
#include <mutex>
#include <gl_format.h>
#include <ktx.h>
#include <astc_codec_internals.h>

namespace Moer {
    EPixelFormat KtxImageHelper::GetFormatFromOpenGLInternalFormat(const GLenum intername_foramt) {
        switch (intername_foramt) {
            //
            // 8 bits per component
            //
            case GL_R8: return PF_R8_UNORM;         // 1-component, 8-bit unsigned normalized
            case GL_RG8: return PF_R8G8_UNORM;      // 2-component, 8-bit unsigned normalized
            case GL_RGB8: return PF_R8G8B8_UNORM;   // 3-component, 8-bit unsigned normalized
            case GL_RGBA8: return PF_R8G8B8A8_UNORM;// 4-component, 8-bit unsigned normalized

            case GL_R8_SNORM: return PF_R8_SNORM;         // 1-component, 8-bit signed normalized
            case GL_RG8_SNORM: return PF_R8G8_SNORM;      // 2-component, 8-bit signed normalized
            case GL_RGB8_SNORM: return PF_R8G8B8_SNORM;   // 3-component, 8-bit signed normalized
            case GL_RGBA8_SNORM: return PF_R8G8B8A8_SNORM;// 4-component, 8-bit signed normalized

            case GL_R8UI: return PF_R8_UINT;         // 1-component, 8-bit unsigned integer
            case GL_RG8UI: return PF_R8G8_UINT;      // 2-component, 8-bit unsigned integer
            case GL_RGB8UI: return PF_R8G8B8_UINT;   // 3-component, 8-bit unsigned integer
            case GL_RGBA8UI: return PF_R8G8B8A8_UINT;// 4-component, 8-bit unsigned integer

            case GL_R8I: return PF_R8_SINT;         // 1-component, 8-bit signed integer
            case GL_RG8I: return PF_R8G8_SINT;      // 2-component, 8-bit signed integer
            case GL_RGB8I: return PF_R8G8B8_SINT;   // 3-component, 8-bit signed integer
            case GL_RGBA8I: return PF_R8G8B8A8_SINT;// 4-component, 8-bit signed integer

            case GL_SR8: return PF_R8_SRGB;      // 1-component, 8-bit sRGB
            case GL_SRG8: return PF_R8G8_SRGB;   // 2-component, 8-bit sRGB
            case GL_SRGB8: return PF_R8G8B8_SRGB;// 3-component, 8-bit sRGB
            case GL_SRGB8_ALPHA8:
                return PF_R8G8B8A8_SRGB;// 4-component, 8-bit sRGB

            //
            // 16 bits per component
            //
            case GL_R16: return PF_R16_UNORM;            // 1-component, 16-bit unsigned normalized
            case GL_RG16: return PF_R16G16_UNORM;        // 2-component, 16-bit unsigned normalized
            case GL_RGB16: return PF_R16G16B16_UNORM;    // 3-component, 16-bit unsigned normalized
            case GL_RGBA16: return PF_R16G16B16A16_UNORM;// 4-component, 16-bit unsigned normalized

            case GL_R16_SNORM: return PF_R16_SNORM;            // 1-component, 16-bit signed normalized
            case GL_RG16_SNORM: return PF_R16G16_SNORM;        // 2-component, 16-bit signed normalized
            case GL_RGB16_SNORM: return PF_R16G16B16_SNORM;    // 3-component, 16-bit signed normalized
            case GL_RGBA16_SNORM: return PF_R16G16B16A16_SNORM;// 4-component, 16-bit signed normalized

            case GL_R16UI: return PF_R16_UINT;            // 1-component, 16-bit unsigned integer
            case GL_RG16UI: return PF_R16G16_UINT;        // 2-component, 16-bit unsigned integer
            case GL_RGB16UI: return PF_R16G16B16_UINT;    // 3-component, 16-bit unsigned integer
            case GL_RGBA16UI: return PF_R16G16B16A16_UINT;// 4-component, 16-bit unsigned integer

            case GL_R16I: return PF_R16_SINT;            // 1-component, 16-bit signed integer
            case GL_RG16I: return PF_R16G16_SINT;        // 2-component, 16-bit signed integer
            case GL_RGB16I: return PF_R16G16B16_SINT;    // 3-component, 16-bit signed integer
            case GL_RGBA16I: return PF_R16G16B16A16_SINT;// 4-component, 16-bit signed integer

            case GL_R16F: return PF_R16_SFLOAT;        // 1-component, 16-bit floating-point
            case GL_RG16F: return PF_R16G16_SFLOAT;    // 2-component, 16-bit floating-point
            case GL_RGB16F: return PF_R16G16B16_SFLOAT;// 3-component, 16-bit floating-point
            case GL_RGBA16F:
                return PF_R16G16B16A16_SFLOAT;// 4-component, 16-bit floating-point

            //
            // 32 bits per component
            //
            case GL_R32UI: return PF_R32_UINT;            // 1-component, 32-bit unsigned integer
            case GL_RG32UI: return PF_R32G32_UINT;        // 2-component, 32-bit unsigned integer
            case GL_RGB32UI: return PF_R32G32B32_UINT;    // 3-component, 32-bit unsigned integer
            case GL_RGBA32UI: return PF_R32G32B32A32_UINT;// 4-component, 32-bit unsigned integer

            case GL_R32I: return PF_R32_SINT;            // 1-component, 32-bit signed integer
            case GL_RG32I: return PF_R32G32_SINT;        // 2-component, 32-bit signed integer
            case GL_RGB32I: return PF_R32G32B32_SINT;    // 3-component, 32-bit signed integer
            case GL_RGBA32I: return PF_R32G32B32A32_SINT;// 4-component, 32-bit signed integer

            case GL_R32F: return PF_R32_SFLOAT;        // 1-component, 32-bit floating-point
            case GL_RG32F: return PF_R32G32_SFLOAT;    // 2-component, 32-bit floating-point
            case GL_RGB32F: return PF_R32G32B32_SFLOAT;// 3-component, 32-bit floating-point
            case GL_RGBA32F:
                return PF_R32G32B32A32_SFLOAT;// 4-component, 32-bit floating-point

            //
            // Packed
            //
            case GL_R3_G3_B2: return PF_UNDEFINED;                    // 3-component 3:3:2,       unsigned normalized
            case GL_RGB4: return PF_UNDEFINED;                        // 3-component 4:4:4,       unsigned normalized
            case GL_RGB5: return PF_R5G5B5A1_UNORM_PACK16;            // 3-component 5:5:5,       unsigned normalized
            case GL_RGB565: return PF_R5G6B5_UNORM_PACK16;            // 3-component 5:6:5,       unsigned normalized
            case GL_RGB10: return PF_A2R10G10B10_UNORM_PACK32;        // 3-component 10:10:10,    unsigned normalized
            case GL_RGB12: return PF_UNDEFINED;                       // 3-component 12:12:12,    unsigned normalized
            case GL_RGBA2: return PF_UNDEFINED;                       // 4-component 2:2:2:2,     unsigned normalized
            case GL_RGBA4: return PF_R4G4B4A4_UNORM_PACK16;           // 4-component 4:4:4:4,     unsigned normalized
            case GL_RGBA12: return PF_UNDEFINED;                      // 4-component 12:12:12:12, unsigned normalized
            case GL_RGB5_A1: return PF_A1R5G5B5_UNORM_PACK16;         // 4-component 5:5:5:1,     unsigned normalized
            case GL_RGB10_A2: return PF_A2R10G10B10_UNORM_PACK32;     // 4-component 10:10:10:2,  unsigned normalized
            case GL_RGB10_A2UI: return PF_A2R10G10B10_UINT_PACK32;    // 4-component 10:10:10:2,  unsigned integer
            case GL_R11F_G11F_B10F: return PF_B10G11R11_UFLOAT_PACK32;// 3-component 11:11:10,    floating-point
            case GL_RGB9_E5:
                return PF_E5B9G9R9_UFLOAT_PACK32;// 3-component/exp 9:9:9/5, floating-point

                //
                // S3TC/DXT/BC
                //

            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: return PF_BC1_RGB_UNORM_BLOCK;  // line through 3D space, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: return PF_BC1_RGBA_UNORM_BLOCK;// line through 3D space plus 1-bit alpha, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: return PF_BC2_UNORM_BLOCK;     // line through 3D space plus line through 1D space, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: return PF_BC3_UNORM_BLOCK;     // line through 3D space plus 4-bit alpha, 4x4 blocks, unsigned normalized

            case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT: return PF_BC1_RGB_SRGB_BLOCK;       // line through 3D space, 4x4 blocks, sRGB
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT: return PF_BC1_RGBA_SRGB_BLOCK;// line through 3D space plus 1-bit alpha, 4x4 blocks, sRGB
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT: return PF_BC2_SRGB_BLOCK;     // line through 3D space plus line through 1D space, 4x4 blocks, sRGB
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT: return PF_BC3_SRGB_BLOCK;     // line through 3D space plus 4-bit alpha, 4x4 blocks, sRGB

            case GL_COMPRESSED_LUMINANCE_LATC1_EXT: return PF_BC4_UNORM_BLOCK;             // line through 1D space, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT: return PF_BC5_UNORM_BLOCK;       // two lines through 1D space, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_SIGNED_LUMINANCE_LATC1_EXT: return PF_BC4_SNORM_BLOCK;      // line through 1D space, 4x4 blocks, signed normalized
            case GL_COMPRESSED_SIGNED_LUMINANCE_ALPHA_LATC2_EXT: return PF_BC5_SNORM_BLOCK;// two lines through 1D space, 4x4 blocks, signed normalized

            case GL_COMPRESSED_RED_RGTC1: return PF_BC4_UNORM_BLOCK;       // line through 1D space, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RG_RGTC2: return PF_BC5_UNORM_BLOCK;        // two lines through 1D space, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_SIGNED_RED_RGTC1: return PF_BC4_SNORM_BLOCK;// line through 1D space, 4x4 blocks, signed normalized
            case GL_COMPRESSED_SIGNED_RG_RGTC2: return PF_BC5_SNORM_BLOCK; // two lines through 1D space, 4x4 blocks, signed normalized

            case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT: return PF_BC6H_UFLOAT_BLOCK;// 3-component, 4x4 blocks, unsigned floating-point
            case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT: return PF_BC6H_SFLOAT_BLOCK;  // 3-component, 4x4 blocks, signed floating-point
            case GL_COMPRESSED_RGBA_BPTC_UNORM: return PF_BC7_UNORM_BLOCK;          // 4-component, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
                return PF_BC7_SRGB_BLOCK;// 4-component, 4x4 blocks, sRGB

            //
            // ETC
            //
            case GL_ETC1_RGB8_OES: return PF_ETC2_R8G8B8_UNORM_BLOCK;// 3-component ETC1, 4x4 blocks, unsigned normalized

            case GL_COMPRESSED_RGB8_ETC2: return PF_ETC2_R8G8B8_UNORM_BLOCK;                      // 3-component ETC2, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2: return PF_ETC2_R8G8B8A1_UNORM_BLOCK;// 4-component ETC2 with 1-bit alpha, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA8_ETC2_EAC: return PF_ETC2_R8G8B8A8_UNORM_BLOCK;               // 4-component ETC2, 4x4 blocks, unsigned normalized

            case GL_COMPRESSED_SRGB8_ETC2: return PF_ETC2_R8G8B8_SRGB_BLOCK;                      // 3-component ETC2, 4x4 blocks, sRGB
            case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2: return PF_ETC2_R8G8B8A1_SRGB_BLOCK;// 4-component ETC2 with 1-bit alpha, 4x4 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC: return PF_ETC2_R8G8B8A8_SRGB_BLOCK;         // 4-component ETC2, 4x4 blocks, sRGB

            case GL_COMPRESSED_R11_EAC: return PF_EAC_R11_UNORM_BLOCK;       // 1-component ETC, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RG11_EAC: return PF_EAC_R11G11_UNORM_BLOCK;   // 2-component ETC, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_SIGNED_R11_EAC: return PF_EAC_R11_SNORM_BLOCK;// 1-component ETC, 4x4 blocks, signed normalized
            case GL_COMPRESSED_SIGNED_RG11_EAC:
                return PF_EAC_R11G11_SNORM_BLOCK;// 2-component ETC, 4x4 blocks, signed normalized

            //
            // PVRTC
            //
            case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG: return PF_PVRTC1_2BPP_UNORM_BLOCK_IMG; // 3-component PVRTC, 16x8 blocks, unsigned normalized
            case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG: return PF_PVRTC1_4BPP_UNORM_BLOCK_IMG; // 3-component PVRTC,  8x8 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG: return PF_PVRTC1_2BPP_UNORM_BLOCK_IMG;// 4-component PVRTC, 16x8 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG: return PF_PVRTC1_4BPP_UNORM_BLOCK_IMG;// 4-component PVRTC,  8x8 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_PVRTC_2BPPV2_IMG: return PF_PVRTC2_2BPP_UNORM_BLOCK_IMG;// 4-component PVRTC,  8x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG: return PF_PVRTC2_4BPP_UNORM_BLOCK_IMG;// 4-component PVRTC,  4x4 blocks, unsigned normalized

            case GL_COMPRESSED_SRGB_PVRTC_2BPPV1_EXT: return PF_PVRTC1_2BPP_SRGB_BLOCK_IMG;      // 3-component PVRTC, 16x8 blocks, sRGB
            case GL_COMPRESSED_SRGB_PVRTC_4BPPV1_EXT: return PF_PVRTC1_4BPP_SRGB_BLOCK_IMG;      // 3-component PVRTC,  8x8 blocks, sRGB
            case GL_COMPRESSED_SRGB_ALPHA_PVRTC_2BPPV1_EXT: return PF_PVRTC1_2BPP_SRGB_BLOCK_IMG;// 4-component PVRTC, 16x8 blocks, sRGB
            case GL_COMPRESSED_SRGB_ALPHA_PVRTC_4BPPV1_EXT: return PF_PVRTC1_4BPP_SRGB_BLOCK_IMG;// 4-component PVRTC,  8x8 blocks, sRGB
            case GL_COMPRESSED_SRGB_ALPHA_PVRTC_2BPPV2_IMG: return PF_PVRTC2_2BPP_SRGB_BLOCK_IMG;// 4-component PVRTC,  8x4 blocks, sRGB
            case GL_COMPRESSED_SRGB_ALPHA_PVRTC_4BPPV2_IMG:
                return PF_PVRTC2_4BPP_SRGB_BLOCK_IMG;// 4-component PVRTC,  4x4 blocks, sRGB

            //
            // ASTC
            //
            case GL_COMPRESSED_RGBA_ASTC_4x4_KHR: return PF_ASTC_4x4_UNORM_BLOCK;    // 4-component ASTC, 4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_5x4_KHR: return PF_ASTC_5x4_UNORM_BLOCK;    // 4-component ASTC, 5x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_5x5_KHR: return PF_ASTC_5x5_UNORM_BLOCK;    // 4-component ASTC, 5x5 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_6x5_KHR: return PF_ASTC_6x5_UNORM_BLOCK;    // 4-component ASTC, 6x5 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_6x6_KHR: return PF_ASTC_6x6_UNORM_BLOCK;    // 4-component ASTC, 6x6 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_8x5_KHR: return PF_ASTC_8x5_UNORM_BLOCK;    // 4-component ASTC, 8x5 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_8x6_KHR: return PF_ASTC_8x6_UNORM_BLOCK;    // 4-component ASTC, 8x6 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_8x8_KHR: return PF_ASTC_8x8_UNORM_BLOCK;    // 4-component ASTC, 8x8 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_10x5_KHR: return PF_ASTC_10x5_UNORM_BLOCK;  // 4-component ASTC, 10x5 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_10x6_KHR: return PF_ASTC_10x6_UNORM_BLOCK;  // 4-component ASTC, 10x6 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_10x8_KHR: return PF_ASTC_10x8_UNORM_BLOCK;  // 4-component ASTC, 10x8 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_10x10_KHR: return PF_ASTC_10x10_UNORM_BLOCK;// 4-component ASTC, 10x10 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_12x10_KHR: return PF_ASTC_12x10_UNORM_BLOCK;// 4-component ASTC, 12x10 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_12x12_KHR: return PF_ASTC_12x12_UNORM_BLOCK;// 4-component ASTC, 12x12 blocks, unsigned normalized

            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR: return PF_ASTC_4x4_SRGB_BLOCK;    // 4-component ASTC, 4x4 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR: return PF_ASTC_5x4_SRGB_BLOCK;    // 4-component ASTC, 5x4 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR: return PF_ASTC_5x5_SRGB_BLOCK;    // 4-component ASTC, 5x5 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR: return PF_ASTC_6x5_SRGB_BLOCK;    // 4-component ASTC, 6x5 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR: return PF_ASTC_6x6_SRGB_BLOCK;    // 4-component ASTC, 6x6 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR: return PF_ASTC_8x5_SRGB_BLOCK;    // 4-component ASTC, 8x5 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR: return PF_ASTC_8x6_SRGB_BLOCK;    // 4-component ASTC, 8x6 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR: return PF_ASTC_8x8_SRGB_BLOCK;    // 4-component ASTC, 8x8 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR: return PF_ASTC_10x5_SRGB_BLOCK;  // 4-component ASTC, 10x5 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR: return PF_ASTC_10x6_SRGB_BLOCK;  // 4-component ASTC, 10x6 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR: return PF_ASTC_10x8_SRGB_BLOCK;  // 4-component ASTC, 10x8 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR: return PF_ASTC_10x10_SRGB_BLOCK;// 4-component ASTC, 10x10 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR: return PF_ASTC_12x10_SRGB_BLOCK;// 4-component ASTC, 12x10 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR: return PF_ASTC_12x12_SRGB_BLOCK;// 4-component ASTC, 12x12 blocks, sRGB

            case GL_COMPRESSED_RGBA_ASTC_3x3x3_OES: return PF_UNDEFINED;// 4-component ASTC, 3x3x3 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_4x3x3_OES: return PF_UNDEFINED;// 4-component ASTC, 4x3x3 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_4x4x3_OES: return PF_UNDEFINED;// 4-component ASTC, 4x4x3 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_4x4x4_OES: return PF_UNDEFINED;// 4-component ASTC, 4x4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_5x4x4_OES: return PF_UNDEFINED;// 4-component ASTC, 5x4x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_5x5x4_OES: return PF_UNDEFINED;// 4-component ASTC, 5x5x4 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_5x5x5_OES: return PF_UNDEFINED;// 4-component ASTC, 5x5x5 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_6x5x5_OES: return PF_UNDEFINED;// 4-component ASTC, 6x5x5 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_6x6x5_OES: return PF_UNDEFINED;// 4-component ASTC, 6x6x5 blocks, unsigned normalized
            case GL_COMPRESSED_RGBA_ASTC_6x6x6_OES: return PF_UNDEFINED;// 4-component ASTC, 6x6x6 blocks, unsigned normalized

            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_3x3x3_OES: return PF_UNDEFINED;// 4-component ASTC, 3x3x3 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x3x3_OES: return PF_UNDEFINED;// 4-component ASTC, 4x3x3 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4x3_OES: return PF_UNDEFINED;// 4-component ASTC, 4x4x3 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4x4_OES: return PF_UNDEFINED;// 4-component ASTC, 4x4x4 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4x4_OES: return PF_UNDEFINED;// 4-component ASTC, 5x4x4 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5x4_OES: return PF_UNDEFINED;// 4-component ASTC, 5x5x4 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5x5_OES: return PF_UNDEFINED;// 4-component ASTC, 5x5x5 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5x5_OES: return PF_UNDEFINED;// 4-component ASTC, 6x5x5 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6x5_OES: return PF_UNDEFINED;// 4-component ASTC, 6x6x5 blocks, sRGB
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6x6_OES:
                return PF_UNDEFINED;// 4-component ASTC, 6x6x6 blocks, sRGB

            //
            // ATC
            //
            case GL_ATC_RGB_AMD: return PF_UNDEFINED;                // 3-component, 4x4 blocks, unsigned normalized
            case GL_ATC_RGBA_EXPLICIT_ALPHA_AMD: return PF_UNDEFINED;// 4-component, 4x4 blocks, unsigned normalized
            case GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD:
                return PF_UNDEFINED;// 4-component, 4x4 blocks, unsigned normalized

            //
            // Palletized
            //
            case GL_PALETTE4_RGB8_OES: return PF_UNDEFINED;    // 3-component 8:8:8,   4-bit palette, unsigned normalized
            case GL_PALETTE4_RGBA8_OES: return PF_UNDEFINED;   // 4-component 8:8:8:8, 4-bit palette, unsigned normalized
            case GL_PALETTE4_R5_G6_B5_OES: return PF_UNDEFINED;// 3-component 5:6:5,   4-bit palette, unsigned normalized
            case GL_PALETTE4_RGBA4_OES: return PF_UNDEFINED;   // 4-component 4:4:4:4, 4-bit palette, unsigned normalized
            case GL_PALETTE4_RGB5_A1_OES: return PF_UNDEFINED; // 4-component 5:5:5:1, 4-bit palette, unsigned normalized
            case GL_PALETTE8_RGB8_OES: return PF_UNDEFINED;    // 3-component 8:8:8,   8-bit palette, unsigned normalized
            case GL_PALETTE8_RGBA8_OES: return PF_UNDEFINED;   // 4-component 8:8:8:8, 8-bit palette, unsigned normalized
            case GL_PALETTE8_R5_G6_B5_OES: return PF_UNDEFINED;// 3-component 5:6:5,   8-bit palette, unsigned normalized
            case GL_PALETTE8_RGBA4_OES: return PF_UNDEFINED;   // 4-component 4:4:4:4, 8-bit palette, unsigned normalized
            case GL_PALETTE8_RGB5_A1_OES:
                return PF_UNDEFINED;// 4-component 5:5:5:1, 8-bit palette, unsigned normalized

            //
            // Depth/stencil
            //
            case GL_DEPTH_COMPONENT16: return PF_D16_UNORM;
            case GL_DEPTH_COMPONENT24: return PF_X8_D24_UNORM_PACK32;
            case GL_DEPTH_COMPONENT32: return PF_UNDEFINED;
            case GL_DEPTH_COMPONENT32F: return PF_D32_SFLOAT;
            case GL_DEPTH_COMPONENT32F_NV: return PF_D32_SFLOAT;
            case GL_STENCIL_INDEX1: return PF_UNDEFINED;
            case GL_STENCIL_INDEX4: return PF_UNDEFINED;
            case GL_STENCIL_INDEX8: return PF_S8_UINT;
            case GL_STENCIL_INDEX16: return PF_UNDEFINED;
            case GL_DEPTH24_STENCIL8: return PF_D24_UNORM_S8_UINT;
            case GL_DEPTH32F_STENCIL8: return PF_D32_SFLOAT_S8_UINT;
            case GL_DEPTH32F_STENCIL8_NV: return PF_D32_SFLOAT_S8_UINT;

            default: return PF_UNDEFINED;
        }
    }

    struct AstcHeader {
        uint8_t magic[4];
        uint8_t blockdim_x;
        uint8_t blockdim_y;
        uint8_t blockdim_z;
        uint8_t xsize[3];// x-size = xsize[0] + xsize[1] + xsize[2]
        uint8_t ysize[3];// x-size, y-size and z-size are given in texels;
        uint8_t zsize[3];// block count is inferred
    };

    Vector3i toBlockDim(const EPixelFormat format) {
        switch (format) {
            case PF_ASTC_4x4_UNORM_BLOCK:
            case PF_ASTC_4x4_SRGB_BLOCK:
                return {4, 4, 1};
            case PF_ASTC_5x4_UNORM_BLOCK:
            case PF_ASTC_5x4_SRGB_BLOCK:
                return {5, 4, 1};
            case PF_ASTC_5x5_UNORM_BLOCK:
            case PF_ASTC_5x5_SRGB_BLOCK:
                return {5, 5, 1};
            case PF_ASTC_6x5_UNORM_BLOCK:
            case PF_ASTC_6x5_SRGB_BLOCK:
                return {6, 5, 1};
            case PF_ASTC_6x6_UNORM_BLOCK:
            case PF_ASTC_6x6_SRGB_BLOCK:
                return {6, 6, 1};
            case PF_ASTC_8x5_UNORM_BLOCK:
            case PF_ASTC_8x5_SRGB_BLOCK:
                return {8, 5, 1};
            case PF_ASTC_8x6_UNORM_BLOCK:
            case PF_ASTC_8x6_SRGB_BLOCK:
                return {8, 6, 1};
            case PF_ASTC_8x8_UNORM_BLOCK:
            case PF_ASTC_8x8_SRGB_BLOCK:
                return {8, 8, 1};
            case PF_ASTC_10x5_UNORM_BLOCK:
            case PF_ASTC_10x5_SRGB_BLOCK:
                return {10, 5, 1};
            case PF_ASTC_10x6_UNORM_BLOCK:
            case PF_ASTC_10x6_SRGB_BLOCK:
                return {10, 6, 1};
            case PF_ASTC_10x8_UNORM_BLOCK:
            case PF_ASTC_10x8_SRGB_BLOCK:
                return {10, 8, 1};
            case PF_ASTC_10x10_UNORM_BLOCK:
            case PF_ASTC_10x10_SRGB_BLOCK:
                return {10, 10, 1};
            case PF_ASTC_12x10_UNORM_BLOCK:
            case PF_ASTC_12x10_SRGB_BLOCK:
                return {12, 10, 1};
            case PF_ASTC_12x12_UNORM_BLOCK:
            case PF_ASTC_12x12_SRGB_BLOCK:
                return {12, 12, 1};
            default:
                throw std::runtime_error{"Invalid astc format"};
        }
    }

    void KtxImageHelper::DecodeAstcImage(ImageReadDesc& desc) {
        static bool                  initialized{false};
        static std::mutex            initialization;
        std::unique_lock<std::mutex> lock{initialization};
        if (!initialized) {
            // Init stuff
            prepare_angular_tables();
            build_quantization_mode_table();
            initialized = true;
        }

        const auto     blockdim = toBlockDim(desc.format);
        const uint8_t* data_ptr = static_cast<const uint8_t*>(desc.data);

        Decode(desc, blockdim);
    }
    bool KtxImageHelper::IsAstc(EPixelFormat format) {
        return (format == PF_ASTC_4x4_UNORM_BLOCK ||
                format == PF_ASTC_4x4_SRGB_BLOCK ||
                format == PF_ASTC_5x4_UNORM_BLOCK ||
                format == PF_ASTC_5x4_SRGB_BLOCK ||
                format == PF_ASTC_5x5_UNORM_BLOCK ||
                format == PF_ASTC_5x5_SRGB_BLOCK ||
                format == PF_ASTC_6x5_UNORM_BLOCK ||
                format == PF_ASTC_6x5_SRGB_BLOCK ||
                format == PF_ASTC_6x6_UNORM_BLOCK ||
                format == PF_ASTC_6x6_SRGB_BLOCK ||
                format == PF_ASTC_8x5_UNORM_BLOCK ||
                format == PF_ASTC_8x5_SRGB_BLOCK ||
                format == PF_ASTC_8x6_UNORM_BLOCK ||
                format == PF_ASTC_8x6_SRGB_BLOCK ||
                format == PF_ASTC_8x8_UNORM_BLOCK ||
                format == PF_ASTC_8x8_SRGB_BLOCK ||
                format == PF_ASTC_10x5_UNORM_BLOCK ||
                format == PF_ASTC_10x5_SRGB_BLOCK ||
                format == PF_ASTC_10x6_UNORM_BLOCK ||
                format == PF_ASTC_10x6_SRGB_BLOCK ||
                format == PF_ASTC_10x8_UNORM_BLOCK ||
                format == PF_ASTC_10x8_SRGB_BLOCK ||
                format == PF_ASTC_10x10_UNORM_BLOCK ||
                format == PF_ASTC_10x10_SRGB_BLOCK ||
                format == PF_ASTC_12x10_UNORM_BLOCK ||
                format == PF_ASTC_12x10_SRGB_BLOCK ||
                format == PF_ASTC_12x12_UNORM_BLOCK ||
                format == PF_ASTC_12x12_SRGB_BLOCK);
    }

    void KtxImageHelper::Decode(ImageReadDesc& desc, Vector3i blockdim) {
        // Actual decoding
        astc_decode_mode decode_mode = DECODE_LDR_SRGB;
        uint32_t         bitness     = 8;
        swizzlepattern   swz_decode  = {0, 1, 2, 3};

        int xdim = blockdim.x;
        int ydim = blockdim.y;
        int zdim = blockdim.z;

        if ((xdim < 3 || xdim > 6 || ydim < 3 || ydim > 6 || zdim < 3 || zdim > 6) &&
            (xdim < 4 || xdim == 7 || xdim == 9 || xdim == 11 || xdim > 12 ||
             ydim < 4 || ydim == 7 || ydim == 9 || ydim == 11 || ydim > 12 || zdim != 1)) {
            throw std::runtime_error{"Error reading astc: invalid block"};
        }

        int xsize = desc.width;
        int ysize = desc.height;
        int zsize = desc.depth;

        if (xsize == 0 || ysize == 0 || zsize == 0) {
            throw std::runtime_error{"Error reading astc: invalid size"};
        }

        int xblocks = (xsize + xdim - 1) / xdim;
        int yblocks = (ysize + ydim - 1) / ydim;
        int zblocks = (zsize + zdim - 1) / zdim;

        auto astc_image = allocate_image(bitness, xsize, ysize, zsize, 0);
        initialize_image(astc_image);

        imageblock pb;
        uint8_t*   data = static_cast<uint8_t*>(desc.data);
        for (int z = 0; z < zblocks; z++) {
            for (int y = 0; y < yblocks; y++) {
                for (int x = 0; x < xblocks; x++) {
                    int            offset = (((z * yblocks + y) * xblocks) + x) * 16;
                    const uint8_t* bp     = data + offset;

                    physical_compressed_block pcb = *reinterpret_cast<const physical_compressed_block*>(bp);
                    symbolic_compressed_block scb;

                    physical_to_symbolic(xdim, ydim, zdim, pcb, &scb);
                    decompress_symbolic_block(decode_mode, xdim, ydim, zdim, x * xdim, y * ydim, z * zdim, &scb, &pb);
                    write_imageblock(astc_image, &pb, xdim, ydim, zdim, x * xdim, y * ydim, z * zdim, swz_decode);
                }
            }
        }

        desc.data_callback(desc.data);

        desc.data          = new uint8_t[astc_image->xsize * astc_image->ysize * astc_image->zsize * 4];
        desc.data_callback = free;
        memcpy(desc.data, astc_image->imagedata8[0][0], astc_image->xsize * astc_image->ysize * astc_image->zsize * 4);

        desc.format = EPixelFormat::PF_R8G8B8A8_SRGB;
        desc.width  = astc_image->xsize;
        desc.height = astc_image->ysize;
        desc.depth  = astc_image->zsize;

        destroy_image(astc_image);
    }

}