#ifndef RHI_PLATFORM_COMMON_H
#define RHI_PLATFORM_COMMON_H

#include <cstdint>
#include "misc/EnumBitOperation.h"
#pragma region CommonEnums
/** Maximum number of miplevels in a texture. */
enum { MAX_TEXTURE_MIP_COUNT = 15 };

/** Maximum number of static/skeletal mesh LODs */
enum { MAX_MESH_LOD_COUNT = 8 };

/** The maximum number of vertex elements which can be used by a vertex declaration. */
enum {
    MAX_VERTEX_ELEMENT_COUNT         = 17,
    MAX_VERTEX_ELEMENT_COUNT_NumBits = 5,
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

struct Offset2D {
    int32_t x;
    int32_t y;
    bool    operator==(const Offset2D&) const = default;
};
struct Extent2D {
    union {
        uint32_t width;
        uint32_t x;
    };
    union {
        uint32_t height;
        uint32_t y;
    };
    Extent2D(uint32_t _x, uint32_t _y) : x(_x), y(_y) {
    }
    bool operator==(const Extent2D& other) const {
        return x == other.x && y == other.y;
    };
};

struct Offset3D {
    int32_t x;
    int32_t y;
    int32_t z;
    bool    operator==(const Offset3D&) const = default;
};
struct Extent3D {
    union {
        uint32_t width;
        uint32_t x;
    };
    union {
        uint32_t height;
        uint32_t y;
    };
    union {
        uint32_t depth;
        uint32_t z;
    };
    Extent3D(uint32_t _x, uint32_t _y, uint32_t _z) : x(_x), y(_y), z(_z) {
    }
    bool operator==(const Extent3D& other) const {
        return x == other.x && y == other.y && z == other.z;
    };
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
	 * Create a buffer that can be bound as a shader resource.
	 * This is only needed for buffer types which wouldn't ordinarily be used as a texel buffer, like a vertex buffer.
	 */
    SHADER_RESOURCE = 1 << 8,

    /** Request that this buffer is directly CPU accessible. */
    CPU_VISIBLE = 1 << 9,

    /** Buffer should go in fast vram (hint only). Requires TRANSIENT */
    FAST_VRAM = 1 << 10,

    /** Buffer should be allocated from transient memory. */
    TRANSIENT = 1 << 11,

    /** Create a buffer that can be shared with an external RHI or process. */
    SHARED = 1 << 12,

    /**
	 * Buffer contains opaque ray tracing acceleration structure data.
	 * Resources with this flag can't be bound directly to any shader stage and only can be used with ray tracing APIs.
	 * This flag is mutually exclusive with all other buffer usage except BUF_Static.
	*/
    ACCELERATION_STRUCTURE = 1 << 13,

    VERTEX_BUFFER     = 1 << 14,
    INDEX_BUFFER      = 1 << 15,
    STRUCTURED_BUFFER = 1 << 16,

    /** Buffer memory is allocated independently for multiple GPUs, rather than shared via driver aliasing */
    MULTI_GPU_ALLOCATION = 1 << 17,

    /**
	 * Tells the render graph to not bother transferring across GPUs in multi-GPU scenarios.  Useful for cases where
	 * a buffer is read back to the CPU (such as streaming request buffers), or written to each frame by CPU (such
	 * as indirect arg buffers), and the other GPU doesn't actually care about the data.
	*/
    MULT_GPU_GRAPHICS_IGNORE = 1 << 18,

    /** Allows buffer to be used as a scratch buffer for building ray tracing acceleration structure,
	 * which implies unordered access. Only changes the buffer alignment and can be combined with other usage.
	**/
    ACCELERATION_STRUCTURE_STORAGE = (1 << 19) | UNORDERED_ACCESS,

    // Helper bit-masks
    DYNAMIC = (LIFE_CYCLE_DYNAMIC | LIFE_CYCLE_ONE_FRAME),
};

ENUM_BIT_OP_IMPL(EBufferUsageFlags, FLAG)

enum class ETextureDimension : uint8_t {
    TEX_2D,
    TEX_2D_ARRAY,
    TEX_3D,
    TEX_CUBE,
    TEX_CUBE_ARRAY,
    NumBits = 3
};

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
    RRT_MULTI_SAMPLE_STATE,
    RRT_BLEND_STATE,
    RRT_VERTEX_STATE_INITIALIZER,
    RRT_VERTEX_SHADER,
    RRT_MESH_SHADER,
    RRT_AMPLIFICATION_SHADER,
    RRT_FRAGMENT_SHADER,
    RRT_GEOMETRY_SHADER,
    RRT_RAY_TRACING_SHADER,
    RRT_COMPUTE_SHADER,
    RRT_GRAPHIC_PIPELINE_STATE,
    RRT_COMPUTE_PIPELINE_STATE,
    RRT_RAY_TRACING_PIPELINE_STATE,
    RRT_PIPELINE_BOUND_SHADER_STATE,
    RRT_UNIFORM_BUFFER_LAYOUT,
    RRT_UNIFORM_BUFFER,
    RRT_BUFFER,
    RRT_TEXTURE,
    RRT_TEXTURE2D,
    RRT_TEXTURE2D_ARRAY,
    RRT_TEXTURE3D,
    RRT_TEXTURE_CUBE,
    RRT_TEXTURE_REFERENCE,
    RRT_TimestampCalibrationQuery,
    RRT_GPU_FENCE,
    RRT_BARRIER,
    RRT_RenderQuery,
    RRT_RenderQueryPool,
    RRT_VIEWPORT,
    RRT_UNORDERED_ACCESS_VIEW,
    RRT_SHADER_RESOURCE_VIEW,
    RRT_RAYTRACING_ACCELERATION_STRUCTURE,
    RRT_STAGING_BUFFER,
    RRT_CustomPresent,
    RRT_SHADER_LIBRARY,
    RRT_PIPELINE_BINARY_DATA_LIBRARY,

    RRT_Num
};

enum class EAttachmentLoadOp : uint8_t {
    LOAD,
    CLEAR,
    NONE,
    Num,
    NumBits = 2
};
static_assert((int32_t)EAttachmentLoadOp::Num <= 1 << (uint32_t)EAttachmentLoadOp::NumBits, "EAttachmentLoadOp::Num will not fit on EAttachmentLoadOp::NumBits");
enum class EAttachmentStoreOp : uint8_t {
    STORE = 0,
    NONE,
    MULTISAMPLE_RESOLVE,
    Num,
    NumBits = 2
};
static_assert((int32_t)EAttachmentStoreOp::Num <= 1 << (uint32_t)EAttachmentStoreOp::NumBits, "EAttachmentStoreOp::Num will not fit on EAttachmentStoreOp::NumBits");

//todo: maybe get rid of some of them, cause not every frag is supported
enum ERHIPipelineStageFlags : uint32_t {
    PS_TOP_OF_PIPE                      = 0x00000001,
    PS_DRAW_INDIRECT                    = 0x00000002,
    PS_VERTEX_INPUT                     = 0x00000004,
    PS_VERTEX_SHADER                    = 0x00000008,
    PS_TESSELLATION_CONTROL_SHADER      = 0x00000010,
    PS_TESSELLATION_EVALUATION_SHADER   = 0x00000020,
    PS_GEOMETRY_SHADER                  = 0x00000040,
    PS_FRAGMENT_SHADER                  = 0x00000080,
    PS_EARLY_FRAGMENT_TESTS             = 0x00000100,
    PS_LATE_FRAGMENT_TESTS              = 0x00000200,
    PS_COLOR_ATTACHMENT_OUTPUT          = 0x00000400,
    PS_COMPUTE_SHADER                   = 0x00000800,
    PS_TRANSFER                         = 0x00001000,
    PS_BOTTOM_OF_PIPE                   = 0x00002000,
    PS_HOST                             = 0x00004000,
    PS_ALL_GRAPHICS                     = 0x00008000,
    PS_ALL_COMMANDS                     = 0x00010000,
    PS_NONE                             = 0,
    PS_TRANSFORM_FEEDBACK               = 0x01000000,
    PS_CONDITIONAL_RENDERING            = 0x00040000,
    PS_ACCELERATION_STRUCTURE_BUILD     = 0x02000000,
    PS_RAY_TRACING_SHADER               = 0x00200000,
    PS_FRAGMENT_DENSITY_PROCESS         = 0x00800000,
    PS_FRAGMENT_SHADING_RATE_ATTACHMENT = 0x00400000,
    PS_COMMAND_PREPROCESS_BIT_NV        = 0x00020000,
    PS_TASK_SHADER_BIT                  = 0x00080000,
    PS_MESH_SHADER                      = 0x00100000,
    PS_SHADING_RATE_IMAGE_NV            = PS_FRAGMENT_SHADING_RATE_ATTACHMENT,
    PS_RAY_TRACING_SHADER_NV            = PS_RAY_TRACING_SHADER,
    PS_ACCELERATION_STRUCTURE_BUILD_NV  = PS_ACCELERATION_STRUCTURE_BUILD,
    PS_TASK_SHADER_NV                   = PS_TASK_SHADER_BIT,
    PS_MESH_SHADER_NV                   = PS_MESH_SHADER,
    PS_NONE_KHR                         = PS_NONE
};
#pragma endregion

#pragma region pixel format

//temporarily only have effects in vulkan rhi
enum class EBarrierDependencyScope : uint8_t {
    /* for barrier happens between stages in
     * PS_FRAGMENT_SHADER
     * PS_EARLY_FRAGMENT_TESTS
     * PS_LATE_FRAGMENT_TESTS
     * PS_COLOR_ATTACHMENT_OUTPUT
     * Dependency scope can be called framebuffer local or framebuffer global.
     * In vulkan, it means barriers only guarantees ordering between corresponding framebuffer regions
     * between stages above, which can be used to avoid resource flush during multi-subpass in TBR archs.
     *
     * not set means framebuffer global
     */
    BY_REGION,

    // not set means view-global in multi-view rendering
    VIEW_LOCAL,

    // not set means device-local
    NON_DEVICE_LOCAL

};
enum class ERHIAccessFlags : uint32_t {
    UNDEFINED              = 0ULL,
    INDIRECT_COMMAND_READ  = 1 << 0,
    INDEX_READ             = 1 << 1,
    VERTEX_ATTRIBUTE_READ  = 1 << 2,
    UNIFORM_READ           = 1 << 3, /* constant buffer in dx12 while uniform buffer in vulkan */
    INPUT_ATTACHMENT_READ  = 1 << 4,
    SHADER_READ            = 1 << 5,
    SHADER_WRITE           = 1 << 6,
    COLOR_ATTACHMENT_READ  = 1 << 7,
    COLOR_ATTACHMENT_WRITE = 1 << 8,
    DEPTH_STENCIL_READ     = 1 << 9,
    DEPTH_STENCIL_WRITE    = 1 << 10,
    TRANSFER_READ          = 1 << 11,
    TRANSFER_WRITE         = 1 << 12,
    MEMORY_READ            = 1 << 15,

    MEMORY_WRITE  = 1 << 16,
    CPU_READ_BIT  = 1 << 13,
    CPU_WRITE_BIT = 1 << 14,

    SHADER_SAMPLED_READ                       = 1 << 17,
    SHADER_RESOURCE_VIEW                      = 1 << 18,
    UNORDERED_ACCESS_VIEW                     = 1 << 19,
    TRANSFORM_FEEDBACK_WRITE_BIT_EXT          = 1 << 20,
    TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT   = 1 << 21,
    TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT  = 1 << 22,
    CONDITIONAL_RENDERING_READ_BIT_EXT        = 1 << 23,
    COMMAND_PREPROCESS_READ_BIT_VN            = 1 << 24,
    COMMAND_PREPROCESS_WRITE_BIT_NV           = 1 << 25,
    FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT = 1 << 26,
    ACCELERATION_STRUCTURE_READ_BIT           = 1 << 27,
    ACCELERATION_STRUCTURE_WRITE_BIT          = 1 << 28,
    FRAGMENT_DENSITY_MAP_READ_BIT_EXT         = 1 << 29

};
ENUM_BIT_OP_IMPL(ERHIAccessFlags, FLAG)

enum ETextureLayout : uint32_t {
    TEXTURE_LAYOUT_UNDEFINED                        = 1 << 0,
    TEXTURE_LAYOUT_COMMON                           = 1 << 1,
    TEXTURE_LAYOUT_COLOR_ATTACHMENT                 = 1 << 2,
    TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE              = 1 << 3,
    TEXTURE_LAYOUT_DEPTH_STENCIL_READ               = 1 << 4,
    TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL         = 1 << 5,
    TEXTURE_LAYOUT_TRANSFER_SRC                     = 1 << 6,
    TEXTURE_LAYOUT_TRANSFER_DST                     = 1 << 7,
    TEXTURE_LAYOUT_PRE_INITIALIZED                  = 1 << 8, /* for linear textures which data can be written to memory immediately without layout transfer */
    TEXTURE_LAYOUT_DEPTH_READ_STENCIL_WRITE         = 1 << 9,
    TEXTURE_LAYOUT_DEPTH_WRITE_STENCIL_READ         = 1 << 10,
    TEXTURE_LAYOUT_DEPTH_WRITE                      = 1 << 11,
    TEXTURE_LAYOUT_DEPTH_READ                       = 1 << 12,
    TEXTURE_LAYOUT_STENCIL_WRITE                    = 1 << 13,
    TEXTURE_LAYOUT_STENCIL_READ                     = 1 << 14,
    TEXTURE_LAYOUT_VIDEO_ENCODE                     = 1 << 15,
    TEXTURE_LAYOUT_VIDEO_DECODE                     = 1 << 16,
    TEXTURE_LAYOUT_READ                             = 1 << 17,
    TEXTURE_LAYOUT_WRITE                            = 1 << 18,
    TEXTURE_LAYOUT_PRESENT_SRC                      = 1 << 19,
    TEXTURE_LAYOUT_SHARED_PRESENT                   = 1 << 20,
    TEXTURE_LAYOUT_FRAGMENT_DENSITY_MAP             = 1 << 21,
    TEXTURE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT = 1 << 22,
    TEXTURE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL = 1 << 23,

    TEXTURE_LAYOUT_QUEUE_TYPE_GRAPHICS = 1 << 29,
    TEXTURE_LAYOUT_QUEUE_TYPE_COMPUTE  = 1 << 30,
    TEXTURE_LAYOUT_Num
};
#pragma endregion
enum EShaderType : uint8_t {
    ST_VERTEX,
    ST_GEOMETRY,
    ST_FRAGMENT,
    ST_COMPUTE,
    ST_MESH,
    ST_AMPLIFICATION,
    ST_RAY_GEN,
    ST_RAY_MISS,
    ST_RAY_HIT,
    ST_CALLABLE,
    ST_Num,
    ST_NumBits = 4
};
static_assert(ST_Num <= (1 << ST_NumBits), "ST_Num exceeds ST_NumBits bound");
enum EVertexElementType : uint8_t {
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
    VET_Num,

    VET_NumBits = 5,
};
static_assert(VET_Num <= (1 << VET_NumBits), "VET_Num will not fit on VET_NumBits");

enum EVertexInputRate : uint8_t {
    VIR_VERTEX,
    VIR_INSTANCE,
    VIR_Num,
    VIR_NumBits = 2
};
static_assert(VIR_Num < (1 << VIR_NumBits), "VIR_Num will not fit on VIR_NumBits");
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
    Texture2D,
    Texture2DArray,
    Texture2DMS,
    Texture3D,
    // Texture3DArray: not used in the renderer.
    TextureCube,
    TextureCubeArray,
    TextureMetadata,

    Buffer,
    StructuredBuffer,
    ByteAddressBuffer,
    RaytracingAccelerationStructure,

    // RWTexture1D: not used in the renderer.
    // RWTexture1DArray: not used in the renderer.
    RWTexture2D,
    RWTexture2DArray,
    RWTexture3D,
    // RWTexture3DArray: not used in the renderer.
    RWTextureCube,
    // RWTextureCubeArray: not used in the renderer.
    RWTextureMetadata,

    RWBuffer,
    RWStructuredBuffer,
    RWByteAddressBuffer,
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
    
    UBMT_Num,
    UBMT_NumBits = 3,
};
static_assert(UBMT_Num <= (1 << UBMT_NumBits), "EUniformBufferBaseType_Num will not fit on EUniformBufferBaseType_NumBits");
using UniformBufferGlobalBindingPoint = uint8_t;

enum {
    /** The maximum number of static slots allowed. */
    MAX_UNIFORM_BUFFER_GLOBAL_BINDING_POINT = 255
};

/** Returns whether a static uniform buffer slot index is valid. */
inline bool IsUniformBufferGlobalBindingPointValid(const UniformBufferGlobalBindingPoint binding_point_) {
    return binding_point_ < MAX_UNIFORM_BUFFER_GLOBAL_BINDING_POINT;
}

/** The list of flags declaring which binding models are allowed for a uniform buffer layout. */
enum class EUniformBufferBindingFlags : uint8_t
{
    /** If set, the uniform buffer can be bound as an RHI shader parameter on an RHI shader (i.e. RHISetShaderUniformBuffer). */
    SHADER = 1 << 0,
    
    /** If set, the uniform buffer can be bound globally through a static slot (i.e. RHISetStaticUniformBuffers). */
    STATIC = 1 << 1,

    /** If set, the uniform buffer can be bound globally or per-shader, depending on the use case. Only one binding model should be
	 *  used at a time, and RHI validation will emit an error if both are used for a particular uniform buffer at the same time. This
	 *  is designed for difficult cases where a fixed single binding model would produce an unnecessary maintenance burden. Using this
	 *  disables some RHI validation errors for global bindings, so use with care.
	 */
    ALL = STATIC | SHADER
};

enum EResourceAccessMode {
    RAM_READ_ONLY,
    RAM_WRITE_ONLY,
    RAM_Num
};
enum class ETextureUsageFlags : uint64_t {
    UNDEFINED,

    ATTACHMENT_RENDER,
    ATTACHMENT_RESOLVE,
    ATTACHMENT_DEPTH_STENCIL,

    SRGB,

    SHADER_RESOURCE,
    CPU_VISIBLE,
    TILLING_NONE,
    DYNAMIC,

    INPUT_ATTACHMENT,
    TRANSFER_SRC,
    TRANSFER_DST,
    SAMPLED,
    UNORDERED_ACCESS,
    COLOR_ATTACHMENT,
    DEPTH_STENCIL_ATTACHMENT,
    TRANSIENT_ATTACHMENT,

    VIDEO_DECODE,

    FRAGMENT_DENSITY_MAP,
    FRAGMENT_SHADING_RATE_ATTACHMENT,

    VIDEO_ENCODE,
    ATTACHMENT_FEEDBACK_LOOP,
    Num
};
ENUM_BIT_OP_IMPL(ETextureUsageFlags, FLAG)
enum class ETextureAspectFlags : uint32_t {
    // no
    NONE,
    COLOR = 1 << 1,
    DEPTH_SLICE = 1 << 2,
    STENCIL_SLICE = 1 << 3,
    META_DATA = 1 << 4,
    //for multi-planer texture
    PLANE_0 = 1 << 5,
    PLANE_1 = 1 << 6,
    PLANE_2 = 1 << 7,

    //for ycbcr(used in video encoding, decoding) sampler color conversion
    MEMORY_PLANE_0 = 1 << 8,
    MEMORY_PLANE_1 = 1 << 9,
    MEMORY_PLANE_2 = 1 << 10,
    MEMORY_PLANE_3 = 1 << 11
};

/* various shading rate palette, VSR_{fragment_invocation_count}_{region_size}
 * @fragment_invocation_count means fragment shading invocation per region
 * @region_size means one shading result will be used to color ${regions_size} pixels
 * */
enum EVariousShadingRate : uint8_t {
    VSR_NONE,
    VSR_16_1x1,
    VSR_8_1x1,
    VSR_4_1x1,
    VSR_2_1x1,
    VSR_1_1x1,
    VSR_1_1x2,
    VSR_1_2x1,
    VSR_1_2x2,
    VSR_1_2x4,
    VSR_1_4x2,
    VSR_1_4x4
};
enum EVRSRateCombinerOp : uint8_t {
    VRSRB_KEEP,
    VRSRB_OVERRIDE,
    VRSRB_MIN,
    VRSRB_MAX,
    VRSRB_MUL
};

#pragma endregion

#pragma region shader platform
enum EShaderPlatform : uint16_t {
    SP_WIN_D3D_SM5,
    SP_METAL,
    SP_WIN_D3D_ES3_1,
    SP_METAL_SM5,
    SP_WIN_VULKAN_ES3_1,
    SP_VULKAN_SM5,
    SP_VULKAN_SM6,
    SP_WIN_D3D_SM6,
    SP_METAL_SM6,
    SP_CUSTOM_PLATFORM_FIRST,
    SP_Num,
    SP_NumBits = 16

};
static_assert(SP_Num < (1 << SP_NumBits) && "");
#pragma endregion
#endif// !RHI_PLATFORM_COMMON_H
