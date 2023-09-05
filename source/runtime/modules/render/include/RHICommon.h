#ifndef RHI_PLATFORM_COMMON_H
#define RHI_PLATFORM_COMMON_H
namespace __ENGINE_NAME__ {
namespace RHI {
#include <stdint.h>
#pragma region CommonEnums
    /** Maximum number of miplevels in a texture. */
    enum { MAX_TEXTURE_MIP_COUNT = 15 };

    /** Maximum number of static/skeletal mesh LODs */
    enum { MAX_MESH_LOD_COUNT = 8 };

    /** The maximum number of vertex elements which can be used by a vertex declaration. */
    enum {
        MaxVertexElementCount         = 17,
        MaxVertexElementCount_NumBits = 5,
    };

    enum class ERHIZBuffer {
        // Before changing this, make sure all math & shader assumptions are correct! Also wrap your C++ assumptions with
        //		static_assert(ERHIZBuffer::IsInvertedZBuffer(), ...);
        // Shader-wise, make sure to update Definitions.usf, HAS_INVERTED_Z_BUFFER
        FarPlane  = 0,
        NearPlane = 1,

        // 'bool' for knowing if the API is using Inverted Z buffer
        IsInverted = (FarPlane < ERHIZBuffer::NearPlane),
    };

    namespace EShadingPath {
        enum {
            Deferred,
            Forward,
            Num
        };
    }
#pragma endregion

#pragma region Descriptors Regulation
    /** The alignment in bytes between elements of array shader parameters. */
    enum { ShaderArrayElementAlignBytes = 16 };

    /** The number of render-targets that may be simultaneously written to. */
    enum {
        MaxSimultaneousColorAttachments         = 8,
        MaxSimultaneousColorAttachments_NumBits = 3,
    };
#pragma endregion

#pragma region cross-platform param types
    enum ESamplerFilter {
        SF_NEAREST,
        SF_LINEAR,
        SF_CUBIC,
        SF_ANISOTROPIC_NEAREST,
        SF_ANISOTROPIC_LINEAR,

        SF_Num,
        SF_NumBits = 3,
    };
    enum ESamplerAddressMode {
        AM_REPEAT,
        AM_MIRRORED_REPEAT,
        AM_CLAMP_TO_EDGE,
        //my not supported
        AM_CLAMP_TO_BORDER,
        AM_Num,
        AM_NumBits = 2
    };
    enum ESamplerCompareFunction {
        SCF_NEVER,
        SCF_LESS,
        SCF_EQUAL,
        SCF_LESS_OR_EQUAL,
        SCF_GREATER,
        SCF_NOT_EQUAL,
        SCF_GREATER_OR_EQUAL,
        SCF_ALWAYS,
        SCF_Num
    };

    enum ERasterizerFillMode {
        FM_FILL,
        FM_LINE,
        FM_POINT,
        FM_FILL_RECTANGLE_NV,

        FM_Num,
        FM_NumBits = 2,
    };

    enum ERasterizerCullMode {
        CM_NONE,
        CM_FRONT,
        CM_BACK,
        CM_FRONT_AND_BACK,

        CM_Num,
        CM_NumBits = 2,
    };

    enum EColorWriteMask {
        CW_RED   = 0x01,
        CW_GREEN = 0x02,
        CW_BLUE  = 0x04,
        CW_ALPHA = 0x08,

        CW_NONE = 0,
        CW_RGB  = CW_RED | CW_GREEN | CW_BLUE,
        CW_RGBA = CW_RED | CW_GREEN | CW_BLUE | CW_ALPHA,
        CW_RG   = CW_RED | CW_GREEN,
        CW_BA   = CW_BLUE | CW_ALPHA,

        EColorWriteMask_NumBits = 4,
    };

    enum ECompareOption {
        CO_NEVER,
        CO_LESS,
        CO_EQUAL,
        CO_LESS_OR_EQUAL,
        CO_GREATER,
        CO_NOT_EQUAL,
        CO_GREATER_OR_EQUAL,
        CO_ALWAYS,

        CO_Num,
        CO_NumBits = 3,

        // Utility enumerations
        CO_DEPTH_NEAR_OR_EQUAL    = (uint32_t)(ERHIZBuffer::IsInverted) != 0 ? CO_GREATER_OR_EQUAL : CO_LESS_OR_EQUAL,
        CO_DEPTH_NEAR             = (uint32_t)(ERHIZBuffer::IsInverted) != 0 ? CO_GREATER : CO_LESS,
        CO_DEPTH_FARTHER_OR_EQUAL = (uint32_t)(ERHIZBuffer::IsInverted) != 0 ? CO_LESS_OR_EQUAL : CO_GREATER_OR_EQUAL,
        CO_DEPTH_FARTHER          = (uint32_t)(ERHIZBuffer::IsInverted) != 0 ? CO_LESS : CO_GREATER,
    };
    static_assert(CO_Num <= (1 << CO_NumBits), "CO_Num will not fit on CO_NumBits");

    enum EStencilMask {
        SM_Default,
        SM_255,
        SM_1,
        SM_2,
        SM_4,
        SM_8,
        SM_16,
        SM_32,
        SM_64,
        SM_128,
        SM_Count
    };

    enum EStencilOp {
        SO_KEEP,
        SO_ZERO,
        SO_REPLACE,
        SO_INCREMENT_AND_CLAMP,
        SO_DECREMENT_AND_CLAMP,
        SO_INVERT,
        SO_INCREMENT_AND_WRAP,
        SO_DECREMENT_AND_WRAP,
        SO_Num,
        SO_NumBits = 3,
    };
    static_assert(SO_Num <= (1 << SO_NumBits), "EStencilOp_Num will not fit on EStencilOp_NumBits");

    /* more to support */
    enum EBlendOperation {
        BO_ADD,
        BO_SUBTRACT,
        BO_REVERSE_SUBTRACT,
        BO_MIN,
        BO_MAX,

        BO_Num,
        BO_NumBits = 3,
    };
    static_assert(BO_Num <= (1 << BO_NumBits), "EBlendOperation_Num will not fit on EBlendOperation_NumBits");

    enum EBlendFactor {
        BF_ZERO,
        BF_ONE,
        BF_SRC_COLOR,
        BF_ONE_MINUS_SRC_COLOR,
        BF_DST_COLOR,
        BF_ONE_MINUS_DST_COLOR,
        BF_SRC_ALPHA,
        BF_ONE_MINUS_SRC_ALPHA,
        BF_DST_ALPHA,
        BF_ONE_MINUS_DST_ALPHA,
        BF_CONSTANT_ALPHA,
        BF_ONE_MINUS_CONSTANT_ALPHA,
        BF_SRC1_COLOR,
        BF_ONE_MINUS_SRC1_COLOR,
        BF_SRC1_ALPHA,
        BF_ONE_MINUS_SRC1_ALPHA,
        BF_Num,
        BF_NumBits = 4,
    };
    static_assert(BF_Num <= (1 << BF_NumBits), "EBlendFactor_Num will not fit on EBlendFactor_NumBits");

    enum class EShaderCodeResourceBindingType : uint8_t {
        Invalid,

        Sampler,

        //// Texture1D: not used in the renderer.
        //// Texture1DArray: not used in the renderer.
        //Texture2D,
        //Texture2DArray,
        //Texture2DMS,
        //Texture3D,
        //// Texture3DArray: not used in the renderer.
        //TextureCube,
        //TextureCubeArray,
        //TextureMetadata,

        //Buffer,
        //StructuredBuffer,
        //ByteAddressBuffer,
        //RaytracingAccelerationStructure,

        //// RWTexture1D: not used in the renderer.
        //// RWTexture1DArray: not used in the renderer.
        //RWTexture2D,
        //RWTexture2DArray,
        //RWTexture3D,
        //// RWTexture3DArray: not used in the renderer.
        //RWTextureCube,
        //// RWTextureCubeArray: not used in the renderer.
        //RWTextureMetadata,

        //RWBuffer,
        //RWStructuredBuffer,
        //RWByteAddressBuffer,

        COMBINED_IMAGE_SAMPLER,
        SAMPLED_IMAGE,
        STORAGE_IMAGE,
        UNIFORM_TEXEL_BUFFER,
        STORAGE_TEXEL_BUFFER,
        UNIFORM_BUFFER,
        STORAGE_BUFFER,
        UNIFORM_BUFFER_DYNAMIC,
        STORAGE_BUFFER_DYNAMIC,
        INPUT_ATTACHMENT,
        INLINE_UNIFORM_BLOCK,
        ACCELERATION_STRUCTURE,
        SAMPLE_WEIGHT_IMAGE_QCOM,
        BLOCK_MATCH_IMAGE_QCOM,
        MUTABLE_EXT,
        INLINE_UNIFORM_BLOCK_EXT = INLINE_UNIFORM_BLOCK,
        MUTABLE_VALVE            = MUTABLE_EXT,
        MAX
    };
#pragma endregion

    enum EVertexAttributeType {
        VET_None,
        VET_Float1,
        VET_Float2,
        VET_Float3,
        VET_Float4,
        VET_PackedNormal,// FPackedNormal
        VET_UByte4,
        VET_UByte4N,
        VET_Color,
        VET_Short2,
        VET_Short4,
        VET_Short2N,// 16 bit word normalized to (value/32767.0,value/32767.0,0,0,1)
        VET_Half2,  // 16 bit float using 1 bit sign, 5 bit exponent, 10 bit mantissa
        VET_Half4,
        VET_Short4N,// 4 X 16 bit word, normalized
        VET_UShort2,
        VET_UShort4,
        VET_UShort2N, // 16 bit word normalized to (value/65535.0,value/65535.0,0,0,1)
        VET_UShort4N, // 4 X 16 bit word unsigned, normalized
        VET_URGB10A2N,// 10 bit r, g, b and 2 bit a normalized to (value/1023.0f, value/1023.0f, value/1023.0f, value/3.0f)
        VET_UInt,
        VET_MAX,

        VET_NumBits = 5,
    };
    static_assert(VET_MAX <= (1 << VET_NumBits), "VET_MAX will not fit on VET_NumBits");

    enum ECubeFace {
        CUBE_FACE_PX = 0,
        CUBE_FACE_NX,
        CUBE_FACE_PY,
        CUBE_FACE_NY,
        CUBE_FACE_PZ,
        CUBE_FACE_NZ,
        CUBE_FACE_Num
    };

}
}// namespace __ENGINE_NAME__::RHI
#endif// !RHI_PLATFORM_COMMON_H
