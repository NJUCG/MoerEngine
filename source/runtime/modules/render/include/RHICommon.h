#ifndef RHI_PLATFORM_COMMON_H
#define RHI_PLATFORM_COMMON_H

#include <stdint.h>
#include "misc/EnumBitOperation.h"
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
enum : uint8_t {
    MAX_PASS_ATTACHMENT_COUNT = 8
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
enum ESamplerFilter : uint8_t {
    SF_NEAREST,
    SF_LINEAR,
    SF_CUBIC,
    SF_ANISOTROPIC_NEAREST,
    SF_ANISOTROPIC_LINEAR,

    SF_Num,
    SF_NumBits = 3,
};
enum ESamplerAddressMode : uint8_t {
    SAM_REPEAT,
    SAM_MIRRORED_REPEAT,
    SAM_CLAMP_TO_EDGE,
    //my not supported
    SAM_CLAMP_TO_BORDER,
    SAM_Num,
    SAM_NumBits = 2
};
enum ESamplerCompareFunction : uint8_t {
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

enum ERasterizerFillMode : uint8_t {
    FM_FILL,
    FM_LINE,
    FM_POINT,
    FM_FILL_RECTANGLE_NV,

    FM_Num,
    FM_NumBits = 2,
};

enum ERasterizerCullMode : uint8_t {
    CM_NONE,
    CM_FRONT,
    CM_BACK,
    CM_FRONT_AND_BACK,

    CM_Num,
    CM_NumBits = 2,
};

enum EColorWriteMask : uint8_t {
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

enum ECompareOption : uint8_t {
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

enum EStencilOp : uint8_t {
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
enum EBlendOperation : uint8_t {
    BO_ADD,
    BO_SUBTRACT,
    BO_REVERSE_SUBTRACT,
    BO_MIN,
    BO_MAX,

    BO_Num,
    BO_NumBits = 3,
};
static_assert(BO_Num <= (1 << BO_NumBits), "EBlendOperation_Num will not fit on EBlendOperation_NumBits");

enum EBlendFactor : uint8_t {
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

enum class EPrimitiveTopology : uint8_t {
    POINT_LIST,
    LINE_LIST,
    LINE_STRIP,
    TRIANGLE_LIST,
    TRIANGLE_STRIP,
    TRIANGLE_LIST_WITH_ADJACENCY,
    TRIANGLE_STRIP_WITH_ADJACENCY,
    PATCH_LIST,
    Num,
    NumBits = 3,
};
static_assert((uint32_t)EPrimitiveTopology::Num <= (1 << (uint32_t)EPrimitiveTopology::NumBits), "EPrimitiveTopologyType::Num will not fit on EPrimitiveTopologyType::NumBits");

enum class EBufferUsageFlags : uint32_t {
    NONE = 0,

    /** The buffer will be written to once. */
    LIFE_CYCLE_STATIC = 1 << 0,

    /** The buffer will be written to occasionally, GPU read only, CPU write only.  The data lifetime is until the next update, or the buffer is destroyed. */
    LIFE_CYCLE_DYNAMIC = 1 << 1,

    /** The buffer's data will have a lifetime of one frame.  It MUST be written to each frame, or a new one created each frame. */
    LIFE_CYCLE_ONE_FRAME = 1 << 2,

    /** Allows an unordered access view to be created for the buffer. */
    UNORDERED_ACCESS = 1 << 3,

    /** Create a byte address buffer, which is basically a structured buffer with a uint32 type. */
    BYTE_ADDRESS_BUFFER = 1 << 4,

    /** Buffer that the GPU will use as a source for a copy. */
    TRANSFER_SRC = 1 << 5,

    /** Create a buffer that can be bound as a transform feed back target. currently not used*/
    TRANSFORM_FEEDBACK_BUFFER = 1 << 6,

    /** Create a buffer which contains the arguments used by DispatchIndirect or DrawIndirect. */
    INDIRECT_BUFFER = 1 << 7,

    /** 
	 * Create a buffer that can be bound as a texel buffer. 
	 * This is only needed for buffer types which wouldn't ordinarily be used as a texel buffer, like a vertex buffer.
	 */
    TEXEL_BUFFER = 1 << 8,

    /** Request that this buffer is directly CPU accessible. */
    CPU_VISIBLE = 1 << 9,

    /** Buffer should go in fast vram (hint only). Requires BUF_Transient */
    FAST_VRAM = 1 << 10,

    /** Buffer should be allocated from transient memory. */
    TRANSIENT = NONE,

    /** Create a buffer that can be shared with an external RHI or process. */
    SHARED = 1 << 12,

    /**
	 * Buffer contains opaque ray tracing acceleration structure data.
	 * Resources with this flag can't be bound directly to any shader stage and only can be used with ray tracing APIs.
	 * This flag is mutually exclusive with all other buffer flags except BUF_Static.
	*/
    ACCELERATION_STRUCTURE = 1 << 13,

    VERTEX_BUFFER  = 1 << 14,
    INDEX_BUFFER   = 1 << 15,
    STORAGE_BUFFER = 1 << 16,

    /** Buffer memory is allocated independently for multiple GPUs, rather than shared via driver aliasing */
    MULTI_GPU_ALLOCATION = 1 << 17,

    /**
	 * Tells the render graph to not bother transferring across GPUs in multi-GPU scenarios.  Useful for cases where
	 * a buffer is read back to the CPU (such as streaming request buffers), or written to each frame by CPU (such
	 * as indirect arg buffers), and the other GPU doesn't actually care about the data.
	*/
    MULT_GPU_GRAPHICS_IGNORE = 1 << 18,

    /** Allows buffer to be used as a scratch buffer for building ray tracing acceleration structure,
	 * which implies unordered access. Only changes the buffer alignment and can be combined with other flags.
	**/
    ACCELERATION_STRUCTURE_STORAGE = (1 << 19) | UNORDERED_ACCESS,

    // Helper bit-masks
    DYNAMIC = (LIFE_CYCLE_DYNAMIC | LIFE_CYCLE_ONE_FRAME),
};

ENUM_BIT_OP_IMPL(EBufferUsageFlags, FLAG)

/*from UE*/
enum class EGPUVenderId {
    Unknown    = -1,
    NotQueried = 0,

    Amd         = 0x1002,
    ImgTec      = 0x1010,
    Nvidia      = 0x10DE,
    Arm         = 0x13B5,
    Broadcom    = 0x14E4,
    Qualcomm    = 0x5143,
    Intel       = 0x8086,
    Apple       = 0x106B,
    Vivante     = 0x7a05,
    VeriSilicon = 0x1EB1,

    Kazan    = 0x10003,// VkVendorId
    Codeplay = 0x10004,// VkVendorId
    Mesa     = 0x10005,// VkVendorId
};

enum ERHIResourceType {
    RRT_NONE,

    RRT_SAMPLER,
    RRT_RASTERIZE_STATE,
    RRT_DEPTH_STENCIL_STATE,
    RRT_BLEND_STATE,
    RRT_VERTEX_DESCRIPTION,
    RRT_VERTEX_SHADER,
    RRT_MESH_SHADER,
    RRT_AmplificationShader,
    RRT_FRAGMENT_SHADER,
    RRT_GEOMETRY_SHADER,
    RRT_RAY_TRACING_SHADER,
    RRT_COMPUTE_SHADER,
    RRT_GRAPHIC_PIPELINE_STATE,
    RRT_COMPUTE_PIPELINE_STATE,
    RRT_RAY_TRACING_PIPELINE_STATE,
    RRT_BoundShaderState,
    RRT_UniformBufferLayout,
    RRT_UniformBuffer,
    RRT_Buffer,
    RRT_Texture,
    RRT_Texture2D,
    RRT_Texture2DArray,
    RRT_Texture3D,
    RRT_TextureCube,
    RRT_TextureReference,
    RRT_TimestampCalibrationQuery,
    RRT_GPUFence,
    RRT_RenderQuery,
    RRT_RenderQueryPool,
    RRT_ComputeFence,
    RRT_Viewport,
    RRT_UnorderedAccessView,
    RRT_ShaderResourceView,
    RRT_RayTracingAccelerationStructure,
    RRT_StagingBuffer,
    RRT_CustomPresent,
    RRT_ShaderLibrary,
    RRT_PipelineBinaryLibrary,

    RRT_Num
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
#pragma region Shader Resources

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
    CONSTANT_BUFFER,
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

enum EUniformBufferBaseType : uint8_t {
    UBMT_INVALID,

    // Invalid type when trying to use bool, to have explicit error message to programmer on why
    // they shouldn't use bool in shader parameter structures.
    UBMT_BOOL,

    // Parameter types.
    UBMT_INT32,
    UBMT_UINT32,
    UBMT_FLOAT32,

    // RHI resources not tracked by render graph.
    //UBMT_TEXTURE,
    //UBMT_SRV,
    //UBMT_UAV,
    //UBMT_SAMPLER,

    // Resources tracked by render graph.
    //UBMT_RDG_TEXTURE,
    //UBMT_RDG_TEXTURE_ACCESS,
    //UBMT_RDG_TEXTURE_ACCESS_ARRAY,
    //UBMT_RDG_TEXTURE_SRV,
    //UBMT_RDG_TEXTURE_UAV,
    //UBMT_RDG_BUFFER_ACCESS,
    //UBMT_RDG_BUFFER_ACCESS_ARRAY,
    //UBMT_RDG_BUFFER_SRV,
    //UBMT_RDG_BUFFER_UAV,
    //UBMT_RDG_UNIFORM_BUFFER,

    // Nested structure.
    UBMT_NESTED_STRUCT,

    // Structure that is nested on C++ side, but included on shader side.
    UBMT_INCLUDED_STRUCT,

    // GPU Indirection reference of struct, like is currently named Uniform buffer.
    //UBMT_REFERENCED_STRUCT,

    // Structure dedicated to setup render targets for a rasterizer pass.
    //UBMT_RENDER_TARGET_BINDING_SLOTS,

    UBMT_Num,
    UBMT_NumBits = 3,
};
static_assert(UBMT_Num <= (1 << UBMT_NumBits), "EUniformBufferBaseType_Num will not fit on EUniformBufferBaseType_NumBits");
using FUniformBufferGlobalBindingPoint = uint8_t;

enum {
    /** The maximum number of static slots allowed. */
    MAX_UNIFORM_BUFFER_GLOBAL_BINDING_POINT = 255
};

/** Returns whether a static uniform buffer slot index is valid. */
inline bool IsUniformBufferGlobalBindingPointValid(const FUniformBufferGlobalBindingPoint binding_point_) {
    return binding_point_ < MAX_UNIFORM_BUFFER_GLOBAL_BINDING_POINT;
}

enum EResourceAccessMode {
    RAM_READ_ONLY,
    RAM_WRITE_ONLY,
    RAM_Num
};

enum ETextureAspectFlagBits {
    IA_COLOR_BIT              = 0x001,
    IA_DEPTH_BIT              = 0x002,
    IA_STENCIL_BIT            = 0x004,
    IA_METADATA_BIT           = 0x008,
    IA_PLANE_0_BIT            = 0x010,
    IA_PLANE_1_BIT            = 0x020,
    IA_PLANE_2_BIT            = 0x040,
    IA_NONE                   = 0,
    IA_MEMORY_PLANE_0_BIT_EXT = 0x080,
    IA_MEMORY_PLANE_1_BIT_EXT = 0x100,
    IA_MEMORY_PLANE_2_BIT_EXT = 0x200,
    IA_MEMORY_PLANE_3_BIT_EXT = 0x400
};

#pragma endregion
#endif// !RHI_PLATFORM_COMMON_H
