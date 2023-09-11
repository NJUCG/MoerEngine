#ifndef RHI_RESOURCE_INITIALIZER_H
#define RHI_RESOURCE_INITIALIZER_H
#include "RHICommon.h"
#include <numeric>
#include "API_Macro.h"
#include "misc/Hash.h"
#include <array>
#include "math/Math.h"
struct RHISamplerInitializer {
    RHISamplerInitializer() {}
    RHISamplerInitializer(
        ESamplerFilter                      _filter,
        ESamplerAddressMode                 _address_mode_u = SAM_REPEAT,
        ESamplerAddressMode                 _address_mode_v = SAM_REPEAT,
        ESamplerAddressMode                 _address_mode_w = SAM_REPEAT,
        float                               _mip_lod_bias   = 0,
        float                               _min_mip_level  = 0,
        float                               _max_mip_level  = std::numeric_limits<float>::max(),
        float                               _max_anisotropy = 0,
        uint32_t                            _border_color   = 0,
        EnumInByte<ESamplerCompareFunction> _compare_op     = SCF_NEVER)
        : filter(_filter),
          address_mode_u(_address_mode_u),
          address_mode_v(_address_mode_v),
          address_mode_w(_address_mode_w),
          mip_lod_bias(_mip_lod_bias),
          min_mip_level(_min_mip_level),
          max_mip_level(_max_mip_level),
          max_anisotropy(_max_anisotropy),
          border_color(_border_color),
          compare_op(_compare_op) {}

    EnumInByte<ESamplerFilter>          filter         = SF_NEAREST;
    EnumInByte<ESamplerAddressMode>     address_mode_u = SAM_REPEAT;
    EnumInByte<ESamplerAddressMode>     address_mode_v = SAM_REPEAT;
    EnumInByte<ESamplerAddressMode>     address_mode_w = SAM_REPEAT;
    float                               mip_lod_bias   = 0;
    float                               min_mip_level  = 0;
    float                               max_mip_level  = std::numeric_limits<float>::max();
    int                                 max_anisotropy = 0;
    uint32_t                            border_color   = 0;
    EnumInByte<ESamplerCompareFunction> compare_op     = SCF_NEVER;

    RHI_API friend uint32_t GetHash(const RHISamplerInitializer& target);
    RHI_API friend bool     operator==(const RHISamplerInitializer& lhs, const RHISamplerInitializer& rhs);
};

struct RHIDepthStencilStateInitializer {
    bool                       b_enable_depth_write;
    EnumInByte<ECompareOption> depth_test_op;
    bool                       b_enable_front_face_stencil;
    EnumInByte<ECompareOption> front_face_stencil_test;
    EnumInByte<EStencilOp>     front_face_stencil_fail_stencilOp;
    EnumInByte<EStencilOp>     front_face_depth_fail_stencilOp;
    EnumInByte<EStencilOp>     front_face_pass_stencil_op;
    bool                       b_enable_back_face_stencil;
    EnumInByte<ECompareOption> back_face_stencil_test;
    EnumInByte<EStencilOp>     back_face_stencil_fail_stencil_op;
    EnumInByte<EStencilOp>     back_face_depth_fail_stencil_op;
    EnumInByte<EStencilOp>     back_face_pass_stencil_op;
    uint8_t                    stencil_readmask;
    uint8_t                    stencil_writemask;

    RHIDepthStencilStateInitializer(
        bool           _b_enable_depth_write              = true,
        ECompareOption _depth_test_op                     = CO_LESS_OR_EQUAL,
        bool           _b_enable_front_face_stencil       = false,
        ECompareOption _front_face_stencil_test           = CO_ALWAYS,
        EStencilOp     _front_face_stencil_fail_stencilOp = SO_KEEP,
        EStencilOp     _front_face_depth_fail_stencilOp   = SO_KEEP,
        EStencilOp     _front_face_pass_stencil_op        = SO_KEEP,
        bool           _b_enable_back_face_stencil        = false,
        ECompareOption _back_face_stencil_test            = CO_ALWAYS,
        EStencilOp     _back_face_stencil_fail_stencil_op = SO_KEEP,
        EStencilOp     _back_face_depth_fail_stencil_op   = SO_KEEP,
        EStencilOp     _back_face_pass_stencil_op         = SO_KEEP,
        uint8_t        _stencil_readmask                  = 0xFF,
        uint8_t        _stencil_writemask                 = 0xFF)
        : b_enable_depth_write(_b_enable_depth_write),
          depth_test_op(_depth_test_op),
          b_enable_front_face_stencil(_b_enable_front_face_stencil),
          front_face_stencil_test(_front_face_stencil_test),
          front_face_stencil_fail_stencilOp(_front_face_stencil_fail_stencilOp),
          front_face_depth_fail_stencilOp(_front_face_depth_fail_stencilOp),
          front_face_pass_stencil_op(_front_face_pass_stencil_op),
          b_enable_back_face_stencil(_b_enable_back_face_stencil),
          back_face_stencil_test(_back_face_stencil_test),
          back_face_stencil_fail_stencil_op(_back_face_stencil_fail_stencil_op),
          back_face_depth_fail_stencil_op(_back_face_depth_fail_stencil_op),
          back_face_pass_stencil_op(_back_face_pass_stencil_op),
          stencil_readmask(_stencil_readmask),
          stencil_writemask(_stencil_writemask) {}
    RHI_API friend uint32_t GetHash(const RHIDepthStencilStateInitializer& target);
    RHI_API friend bool     operator==(const RHIDepthStencilStateInitializer& lhs, const RHIDepthStencilStateInitializer& rhs);
};

struct RHIRasterizationStateInitializer {
    EnumInByte<ERasterizerFillMode> fill_mode;
    EnumInByte<ERasterizerCullMode> cull_mode;
    bool                            b_depth_bias;
    bool                            b_depth_clamp_enable;
    bool                            b_enable_msaa;
    float                           depth_bias;
    float                           depth_bias_clamp;
    float                           depth_bias_slop_factor;
    RHI_API friend uint32_t         GetHash(const RHIRasterizationStateInitializer& target);
    RHI_API friend bool             operator==(const RHIRasterizationStateInitializer& lhs, const RHIRasterizationStateInitializer& rhs);
};

struct RHIMultisampleStateInitializer {

    uint32_t sample_count = 1;
    /*enables per-sample shading to avoid shader aliasing */
    bool b_sample_shading = false;
    /*for alpha test, use msaa coverage to simulate transparency of transparent objects*/
    bool b_alpha_to_converge = false;

    bool b_alpha_to_one = false;
    /*a minimum fraction of sample shading if sample_shading is enabled, closer to 1 is smoother*/
    float                   min_sample_shading = 0.2f;
    RHI_API friend uint32_t GetHash(const RHIMultisampleStateInitializer& target);
    RHI_API friend bool     operator==(const RHIMultisampleStateInitializer& lhs, const RHIMultisampleStateInitializer& rhs);
};

struct RHIFBlendStateInitializer {
    /*corresponds to renderTarget in DX12*/
    struct AttachmentInitializer {

        EnumInByte<EBlendOperation> color_blend_op;
        EnumInByte<EBlendFactor>    color_src_blend_factor;
        EnumInByte<EBlendFactor>    color_dst_blend_factor;
        EnumInByte<EBlendOperation> alpha_blend_op;
        EnumInByte<EBlendFactor>    alpha_src_blend_factor;
        EnumInByte<EBlendFactor>    alpha_dst_blend_factor;
        EnumInByte<EColorWriteMask> color_write_mask;

        explicit AttachmentInitializer(
            EBlendOperation _color_blend_op         = BO_ADD,
            EBlendFactor    _color_src_blend_factor = BF_ONE,
            EBlendFactor    _color_dst_blend_factor = BF_ZERO,
            EBlendOperation _alpha_blend_op         = BO_ADD,
            EBlendFactor    _alpha_src_blend_factor = BF_ONE,
            EBlendFactor    _alpha_dst_blend_factor = BF_ONE,
            EColorWriteMask _color_write_mask       = CW_RGBA)
            : color_blend_op(_color_blend_op),
              color_src_blend_factor(_color_src_blend_factor),
              color_dst_blend_factor(_color_dst_blend_factor),
              alpha_blend_op(_alpha_blend_op),
              alpha_src_blend_factor(_alpha_src_blend_factor),
              alpha_dst_blend_factor(_alpha_dst_blend_factor),
              color_write_mask(_color_write_mask) {}
    };

    RHIFBlendStateInitializer() = default;
    explicit RHIFBlendStateInitializer(const AttachmentInitializer& _attachment_desc) {
        attachments[0] = _attachment_desc;
    }

    template<uint32_t attachment_num>
    explicit RHIFBlendStateInitializer(const std::array<AttachmentInitializer, attachment_num>& _attachment_descs) {
        static_assert(attachment_num < MAX_PASS_ATTACHMENT_COUNT);
        for (uint32_t i = 0; i < attachment_num; i++) {
            attachments[i] = _attachment_descs[i];
        }
    }

    std::array<AttachmentInitializer, MAX_PASS_ATTACHMENT_COUNT> attachments;

    RHI_API friend uint32_t GetHash(const RHIFBlendStateInitializer::AttachmentInitializer& _attachment_desc);
    RHI_API friend bool     operator==(const RHIFBlendStateInitializer::AttachmentInitializer& lhs, const RHIFBlendStateInitializer::AttachmentInitializer& rhs);

    RHI_API friend uint32_t GetHash(const RHIFBlendStateInitializer& Initializer);
    RHI_API friend bool     operator==(const RHIFBlendStateInitializer& lhs, const RHIFBlendStateInitializer& rhs);
};

struct RHIGraphicsPipelineStateInitializer {
    /*vulkan subpass setting, if enabled, this pipeline needs to be bounded with renderpasses*/
    bool b_prefer_subpasses;
};

/**
 *	Viewport bounds structure to set multiple view ports for the geometry shader
 *  (needs to be 1:1 to the D3D11 structure)
 */
struct ViewportBounds {
    float top_left_x{};
    float top_left_y{};
    float width{};
    float height{};
    float min_depth{};
    float max_depth{};

    ViewportBounds() = default;

    ViewportBounds(float _top_left_x, float _top_left_y, float _width, float _height, float _min_depth = 0.0f, float _max_depth = 1.0f)
        : top_left_x(_top_left_x), top_left_y(_top_left_y), width(_width), height(_height), min_depth(_min_depth), max_depth(_max_depth) {
    }
};

enum class EClearAttachment {
    NONE,
    COLOR,
    DEPTH_STENCIL
};
struct RHIClearAttachment {
    struct ClearDepthStencilValue {
        float    depth;
        uint32_t stencil;
        bool     operator==(const ClearDepthStencilValue& other) const {
            return depth == other.depth &&
                   stencil == other.stencil;
        }
    };
    union ClearColorValue {
        float    float32[4];
        int32_t  int32[4];
        uint32_t uint32[4];
        bool     operator==(const ClearColorValue& other) const {
            return uint32[0] == other.uint32[0] && uint32[1] == other.uint32[1] && uint32[2] == other.uint32[2] && uint32[3] == other.uint32[3];
        }
    };
    union ClearValue {
        ClearColorValue        color;
        ClearDepthStencilValue depth_stencil;
    } value;
    EClearAttachment attachment;
    RHIClearAttachment() : attachment(EClearAttachment::COLOR) {
        value.color.float32[0] = 0.f;
        value.color.float32[1] = 0.f;
        value.color.float32[2] = 0.f;
        value.color.float32[3] = 0.f;
    }
    explicit RHIClearAttachment(EClearAttachment none) : attachment(none) {}
    explicit RHIClearAttachment(float _depth, uint32_t _stencil = 0) : attachment(EClearAttachment::DEPTH_STENCIL) {
        value.depth_stencil.depth   = _depth;
        value.depth_stencil.stencil = _stencil;
    }
    bool operator==(const RHIClearAttachment& other) const {
        if (attachment == other.attachment) {
            return attachment == EClearAttachment::COLOR ? value.color == other.value.color : value.depth_stencil == other.value.depth_stencil;
        }
        return false;
    }
};

struct RHICopyTextureInfo {
    // Number of texels to copy. By default it will copy the whole resource if no size is specified.
    int3 size;

    // Position of the copy from the source texture/to destination texture
    int3 source_position;
    int3 dest_position;

    uint32_t src_slice_index = 0;
    uint32_t dst_slice_index = 0;
    uint32_t num_slices      = 1;

    // Mips to copy and destination mips
    uint32_t src_mip_index = 0;
    uint32_t dst_mip_index = 0;
    uint32_t num_mips      = 1;
};

struct RHISubresourceRange
{
    enum ESliceType: uint32_t{
        DEPTH_SLICE,
        STENCIL_SLICE,
        ALL
    };

    uint32_t mip_index = ALL;
    uint32_t array_slice = ALL;
    uint32_t plane_slice = ALL;

    RHISubresourceRange() = default;

    RHISubresourceRange(
        uint32_t _mip_index,
        uint32_t _array_slice,
        uint32_t _plane_slice)
        :mip_index(_mip_index)
          , array_slice (_array_slice)
          , plane_slice (_plane_slice)
    {}

    inline bool IsAllMips() const
    {
        return mip_index == ALL;
    }

    inline bool IsAllArraySlices() const
    {
        return array_slice == ALL;
    }

    inline bool IsAllPlaneSlices() const
    {
        return plane_slice == ALL;
    }

    inline bool IsWholeResource() const
    {
        return IsAllMips() && IsAllArraySlices() && IsAllPlaneSlices();
    }

    inline bool IgnoreDepthPlane() const
    {
        return plane_slice == STENCIL_SLICE;
    }

    inline bool IgnoreStencilPlane() const
    {
        return plane_slice == DEPTH_SLICE;
    }

    inline bool operator == (RHISubresourceRange const& rhs) const
    {
        return mip_index == rhs.mip_index
               && array_slice == rhs.array_slice
               && plane_slice == rhs.plane_slice;
    }

    inline bool operator != (RHISubresourceRange const& rhs) const
    {
        return !(*this == rhs);
    }
};
#endif// !RHI_RESOURCE_INITIALIZER_H
