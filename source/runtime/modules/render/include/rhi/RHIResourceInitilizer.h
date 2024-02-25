#ifndef RHI_RESOURCE_INITIALIZER_H
#define RHI_RESOURCE_INITIALIZER_H
#include "PixelFormat.h"
#include "RHICommon.h"
#include "API_Macro.h"
#include "misc/Hash.h"
#include "misc/STL.h"
#include "math/Math.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"

#include <numeric>
#include <cassert>

namespace RHIConfig {

    enum class MultiSample {
        NONE,
        MSAA_2X,
        MSAA_4X,
        MSAA_8X,
        MSAA_16X
    };
    enum class Blend {
        NONE,
        ALPHA_BLEND,
        ADDITIVE_BLEND,
        PRE_MULTIPLIED_ALPHA_BLEND
    };

    enum class Rast {
        NONE,
        CULL_BACK,
        CULL_FRONT,
        CULL_NONE
    };

    enum class DepthStencil {
        NONE,
        DEPTH_WRITE,
        DEPTH_WRITE_LESS,
        DEPTH_WRITE_LESS_EQUAL,
        DEPTH_WRITE_GREATER,
        DEPTH_WRITE_GREATER_EQUAL
    };

    enum class Attachment {
        NONE,
        COLOR,
        DEPTH_STENCIL,
        RESOLVE
    };
}// namespace RHIConfig
struct RHISamplerCreateInfo {
    RHISamplerCreateInfo() = default;
    explicit RHISamplerCreateInfo(
        ESamplerFilter                      _filter,
        ETextureLayout                      _texture_layout,
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
          texture_layout(_texture_layout),
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
    ETextureLayout                      texture_layout = ETextureLayout::TEXTURE_LAYOUT_UNDEFINED;
    EnumInByte<ESamplerAddressMode>     address_mode_u = SAM_REPEAT;
    EnumInByte<ESamplerAddressMode>     address_mode_v = SAM_REPEAT;
    EnumInByte<ESamplerAddressMode>     address_mode_w = SAM_REPEAT;
    float                               mip_lod_bias   = 0;
    float                               min_mip_level  = 0;
    float                               max_mip_level  = std::numeric_limits<float>::max();
    int                                 max_anisotropy = 0;
    uint32_t                            border_color   = 0;
    EnumInByte<ESamplerCompareFunction> compare_op     = SCF_NEVER;

    RHISamplerCreateInfo& SetFilter(ESamplerFilter _filter) {
        filter = _filter;
        return *this;
    }

    RHISamplerCreateInfo& SetTextureLayout(ETextureLayout _texture_layout) {
        texture_layout = _texture_layout;
        return *this;
    }

    RHISamplerCreateInfo& SetCompareOp(ESamplerCompareFunction _compare_op) {
        compare_op = _compare_op;
        return *this;
    }

    RENDER_API friend uint32_t
                           GetHash(const RHISamplerCreateInfo& target);
    RENDER_API friend bool operator==(const RHISamplerCreateInfo& lhs, const RHISamplerCreateInfo& rhs);
};

struct RHIDepthStencilStateInfo {
    bool                       b_enable_depth_write;
    EnumInByte<ECompareOption> depth_test_op;

    bool                       b_enable_front_face_stencil;
    EnumInByte<ECompareOption> front_face_stencil_test;
    EnumInByte<EStencilOp>     front_face_stencil_fail_stencil_op;
    EnumInByte<EStencilOp>     front_face_depth_fail_stencil_op;
    EnumInByte<EStencilOp>     front_face_pass_stencil_op;

    bool                       b_enable_back_face_stencil;
    EnumInByte<ECompareOption> back_face_stencil_test;
    EnumInByte<EStencilOp>     back_face_stencil_fail_stencil_op;
    EnumInByte<EStencilOp>     back_face_depth_fail_stencil_op;
    EnumInByte<EStencilOp>     back_face_pass_stencil_op;

    uint8_t stencil_readmask;
    uint8_t stencil_writemask;

    explicit RHIDepthStencilStateInfo(
        bool           _b_enable_depth_write              = false,
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
          front_face_stencil_fail_stencil_op(_front_face_stencil_fail_stencilOp),
          front_face_depth_fail_stencil_op(_front_face_depth_fail_stencilOp),
          front_face_pass_stencil_op(_front_face_pass_stencil_op),
          b_enable_back_face_stencil(_b_enable_back_face_stencil),
          back_face_stencil_test(_back_face_stencil_test),
          back_face_stencil_fail_stencil_op(_back_face_stencil_fail_stencil_op),
          back_face_depth_fail_stencil_op(_back_face_depth_fail_stencil_op),
          back_face_pass_stencil_op(_back_face_pass_stencil_op),
          stencil_readmask(_stencil_readmask),
          stencil_writemask(_stencil_writemask) {}

    template<RHIConfig::DepthStencil preset = RHIConfig::
                 DepthStencil::NONE>
    static RHIDepthStencilStateInfo Preset() {
        if constexpr (preset == RHIConfig::DepthStencil::NONE) {
            return RHIDepthStencilStateInfo();
        } else if constexpr (preset == RHIConfig::DepthStencil::DEPTH_WRITE) {
            return RHIDepthStencilStateInfo(true, CO_LESS_OR_EQUAL);
        } else if constexpr (preset == RHIConfig::DepthStencil::DEPTH_WRITE_LESS) {
            return RHIDepthStencilStateInfo(true, CO_LESS);
        } else if constexpr (preset == RHIConfig::DepthStencil::DEPTH_WRITE_LESS_EQUAL) {
            return RHIDepthStencilStateInfo(true, CO_LESS_OR_EQUAL);
        } else if constexpr (preset == RHIConfig::DepthStencil::DEPTH_WRITE_GREATER) {
            return RHIDepthStencilStateInfo(true, CO_GREATER);
        } else if constexpr (preset == RHIConfig::DepthStencil::DEPTH_WRITE_GREATER_EQUAL) {
            return RHIDepthStencilStateInfo(true, CO_GREATER_OR_EQUAL);
        } else {
            static_assert(preset == RHIConfig::DepthStencil::NONE, "Invalid preset");
        }
    }
    RENDER_API friend uint32_t GetHash(const RHIDepthStencilStateInfo& target);
    RENDER_API friend bool     operator==(const RHIDepthStencilStateInfo& lhs, const RHIDepthStencilStateInfo& rhs);
};

struct RHIRasterizeInfo {
    EnumInByte<ERasterizerFillMode> fill_mode              = FM_FILL;
    EnumInByte<ERasterizerCullMode> cull_mode              = RCM_NONE;
    bool                            b_depth_bias           = false;
    bool                            b_depth_clamp_enable   = false;
    bool                            b_enable_msaa          = false;
    float                           depth_bias             = 0;
    float                           depth_bias_clamp       = 0;
    float                           depth_bias_slop_factor = 0;
    template<RHIConfig::Rast preset = RHIConfig::Rast::NONE>
    static RHIRasterizeInfo Preset() {
        if constexpr (preset == RHIConfig::Rast::NONE) {
            return RHIRasterizeInfo();
        } else if constexpr (preset == RHIConfig::Rast::CULL_BACK) {
            return RHIRasterizeInfo(FM_FILL, RCM_BACK, false, false, false, 0, 0, 0);
        } else if constexpr (preset == RHIConfig::Rast::CULL_FRONT) {
            return RHIRasterizeInfo(FM_FILL, RCM_FRONT, false, false, false, 0, 0, 0);
        } else if constexpr (preset == RHIConfig::Rast::CULL_NONE) {
            return RHIRasterizeInfo(FM_FILL, RCM_NONE, false, false, false, 0, 0, 0);
        } else {
            static_assert(preset == RHIConfig::Rast::NONE, "Invalid preset");
        }
    }

    RHIRasterizeInfo& SetFillMode(ERasterizerFillMode mode) {
        fill_mode = mode;
        return *this;
    }

    RHIRasterizeInfo& SetCullMode(ERasterizerCullMode mode) {
        cull_mode = mode;
        return *this;
    }

    RHIRasterizeInfo& SetDepthBias(float bias, float clamp, float slope) {
        b_depth_bias           = true;
        depth_bias             = bias;
        depth_bias_clamp       = clamp;
        depth_bias_slop_factor = slope;
        return *this;
    }

    RHIRasterizeInfo& SetDepthClampEnable(bool enable) {
        b_depth_clamp_enable = enable;
        return *this;
    }

    RHIRasterizeInfo& SetEnableMSAA(bool enable) {
        b_enable_msaa = enable;
        return *this;
    }

    RENDER_API friend uint32_t GetHash(const RHIRasterizeInfo& target);
    RENDER_API friend bool     operator==(const RHIRasterizeInfo& lhs, const RHIRasterizeInfo& rhs);
};

struct RHIMultisampleStateInfo {

    uint32_t sample_count = 1;
    /*enables per-sample shading to avoid shader aliasing */
    bool b_sample_shading = false;
    /*for alpha test, use msaa coverage to simulate transparency of transparent objects*/
    bool b_alpha_to_converge = false;

    bool b_alpha_to_one = false;
    /*a minimum fraction of sample shading if sample_shading is enabled, closer to 1 is smoother*/
    float min_sample_shading = 0.2f;
    template<RHIConfig::MultiSample preset = RHIConfig::MultiSample::NONE>
    static RHIMultisampleStateInfo Preset() {
        if constexpr (preset == RHIConfig::MultiSample::NONE) {
            return RHIMultisampleStateInfo();
        } else if constexpr (preset == RHIConfig::MultiSample::MSAA_2X) {
            return RHIMultisampleStateInfo(2);
        } else if constexpr (preset == RHIConfig::MultiSample::MSAA_4X) {
            return RHIMultisampleStateInfo(4);
        } else if constexpr (preset == RHIConfig::MultiSample::MSAA_8X) {
            return RHIMultisampleStateInfo(8);
        } else if constexpr (preset == RHIConfig::MultiSample::MSAA_16X) {
            return RHIMultisampleStateInfo(16);
        } else {
            static_assert(preset == RHIConfig::MultiSample::NONE, "Invalid preset");
        }
    }

    RHIMultisampleStateInfo& SetSampleCount(uint32_t count) {
        sample_count = count;
        return *this;
    }

    RHIMultisampleStateInfo& SetSampleShading(bool enable) {
        b_sample_shading = enable;
        return *this;
    }

    RHIMultisampleStateInfo& SetAlphaToConverge(bool enable) {
        b_alpha_to_converge = enable;
        return *this;
    }

    RHIMultisampleStateInfo& SetAlphaToOne(bool enable) {
        b_alpha_to_one = enable;
        return *this;
    }

    RHIMultisampleStateInfo& SetMinSampleShading(float min) {
        min_sample_shading = min;
        return *this;
    }

    RENDER_API friend uint32_t GetHash(const RHIMultisampleStateInfo& target);
    RENDER_API friend bool     operator==(const RHIMultisampleStateInfo& lhs, const RHIMultisampleStateInfo& rhs);
};

struct RHIBlendAttachmentInfo {
    EnumInByte<EBlendOperation> color_blend_op;
    EnumInByte<EBlendFactor>    color_src_blend_factor;
    EnumInByte<EBlendFactor>    color_dst_blend_factor;
    EnumInByte<EBlendOperation> alpha_blend_op;
    EnumInByte<EBlendFactor>    alpha_src_blend_factor;
    EnumInByte<EBlendFactor>    alpha_dst_blend_factor;
    EnumInByte<EColorWriteMask> color_write_mask;

    RHIBlendAttachmentInfo(
        EBlendOperation _color_blend_op         = BO_ADD,
        EBlendFactor    _color_src_blend_factor = BF_ONE,
        EBlendFactor    _color_dst_blend_factor = BF_ZERO,
        EBlendOperation _alpha_blend_op         = BO_ADD,
        EBlendFactor    _alpha_src_blend_factor = BF_ONE,
        EBlendFactor    _alpha_dst_blend_factor = BF_ZERO,
        EColorWriteMask _color_write_mask       = CW_RGBA)
        : color_blend_op(_color_blend_op),
          color_src_blend_factor(_color_src_blend_factor),
          color_dst_blend_factor(_color_dst_blend_factor),
          alpha_blend_op(_alpha_blend_op),
          alpha_src_blend_factor(_alpha_src_blend_factor),
          alpha_dst_blend_factor(_alpha_dst_blend_factor),
          color_write_mask(_color_write_mask) {}

    template<RHIConfig::Blend preset = RHIConfig::Blend::NONE>
    static RHIBlendAttachmentInfo Preset() {
        if constexpr (preset == RHIConfig::Blend::NONE) {
            return RHIBlendAttachmentInfo();
        } else if constexpr (preset == RHIConfig::Blend::ALPHA_BLEND) {
            return RHIBlendAttachmentInfo(
                BO_ADD,
                BF_SRC_ALPHA,
                BF_ONE_MINUS_SRC_ALPHA,
                BO_ADD,
                BF_ONE,
                BF_ONE_MINUS_SRC_ALPHA,
                CW_RGBA);
        } else if constexpr (preset == RHIConfig::Blend::ADDITIVE_BLEND) {
            return RHIBlendAttachmentInfo(
                BO_ADD,
                BF_ONE,
                BF_ONE,
                BO_ADD,
                BF_ONE,
                BF_ONE,
                CW_RGBA);
        } else if constexpr (preset == RHIConfig::Blend::PRE_MULTIPLIED_ALPHA_BLEND) {
            return RHIBlendAttachmentInfo(
                BO_ADD,
                BF_ONE,
                BF_ONE_MINUS_SRC_ALPHA,
                BO_ADD,
                BF_ONE,
                BF_ONE_MINUS_SRC_ALPHA,
                CW_RGBA);
        } else {
            static_assert(preset == RHIConfig::Blend::NONE, "Invalid preset");
        }
    }
    RHIBlendAttachmentInfo& SetColorBlendOp(EBlendOperation op) {
        color_blend_op = op;
        return *this;
    }
    RHIBlendAttachmentInfo& SetColorSrcBlendFactor(EBlendFactor factor) {
        color_src_blend_factor = factor;
        return *this;
    }

    RHIBlendAttachmentInfo& SetColorDstBlendFactor(EBlendFactor factor) {
        color_dst_blend_factor = factor;
        return *this;
    }

    RHIBlendAttachmentInfo& SetAlphaBlendOp(EBlendOperation op) {
        alpha_blend_op = op;
        return *this;
    }

    RHIBlendAttachmentInfo& SetAlphaSrcBlendFactor(EBlendFactor factor) {
        alpha_src_blend_factor = factor;
        return *this;
    }

    RHIBlendAttachmentInfo& SetAlphaDstBlendFactor(EBlendFactor factor) {
        alpha_dst_blend_factor = factor;
        return *this;
    }

    RHIBlendAttachmentInfo& SetColorWriteMask(EColorWriteMask mask) {
        color_write_mask = mask;
        return *this;
    }

    RENDER_API friend uint32_t GetHash(const RHIBlendAttachmentInfo& _attachment_desc);
    RENDER_API friend bool     operator==(const RHIBlendAttachmentInfo& lhs, const RHIBlendAttachmentInfo& rhs);
};
struct RHIBlendStateInfo {

    RHIBlendStateInfo() = default;
    explicit RHIBlendStateInfo(const RHIBlendAttachmentInfo& _attachment_desc) {
        attachments[0] = _attachment_desc;
    }

    template<uint32_t attachment_num>
    explicit RHIBlendStateInfo(const Moer::StaticArray<RHIBlendAttachmentInfo, attachment_num>& _attachment_descs) {
        static_assert(attachment_num < MAX_PASS_ATTACHMENT_COUNT);
        for (uint32_t i = 0; i < attachment_num; i++) {
            attachments[i] = _attachment_descs[i];
        }
    }

    Moer::StaticArray<RHIBlendAttachmentInfo, MAX_PASS_ATTACHMENT_COUNT> attachments;

    RENDER_API friend uint32_t GetHash(const RHIBlendStateInfo& Initializer);
    RENDER_API friend bool     operator==(const RHIBlendStateInfo& lhs, const RHIBlendStateInfo& rhs);
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

namespace Moer {
    class WindowHandle;
}
struct RHIViewportInitializer {
    Moer::WindowHandle* window_handle;
    bool                b_is_full_screen;
    bool                b_vsync;
    EPixelFormat        preferred_format;
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
    RHIClearAttachment() : RHIClearAttachment(EClearAttachment::COLOR) {
    }
    explicit RHIClearAttachment(EClearAttachment none) : attachment(none) {
        switch (none) {

            case EClearAttachment::NONE:

            case EClearAttachment::COLOR:
                value.color.float32[0] = 0.f;
                value.color.float32[1] = 0.f;
                value.color.float32[2] = 0.f;
                value.color.float32[3] = 0.f;
                break;
            case EClearAttachment::DEPTH_STENCIL:
                value.depth_stencil.depth   = 0.f;
                value.depth_stencil.stencil = 0;
                break;
        }
    }
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
    friend uint32_t GetHash(const RHIClearAttachment& value) {
        uint32_t hash = GetHash(value.attachment);
        if (EClearAttachment::COLOR == value.attachment) {
            HashCombine(hash, GetHash(value.value.color.uint32[0]));
            HashCombine(hash, GetHash(value.value.color.uint32[1]));
            HashCombine(hash, GetHash(value.value.color.uint32[2]));
            HashCombine(hash, GetHash(value.value.color.uint32[3]));
        } else {
            HashCombine(hash, GetHash(value.value.depth_stencil.depth));
            HashCombine(hash, GetHash(value.value.depth_stencil.stencil));
        }
        return hash;
    }
};
//contains one mip subresource data
struct RHISubresourceSlice {
    const static uint8_t s_all = std::numeric_limits<uint8_t>::max();

    ETextureAspectFlags aspect = ETextureAspectFlags::NONE;
    uint8_t             mip_index;
    uint8_t             array_index;
    uint8_t             array_count;
    uint8_t             plane_index;
    uint8_t             plane_count;

    RHISubresourceSlice(
        ETextureAspectFlags _aspect,
        uint8_t             _mip_index,
        uint8_t             _array_index,
        uint8_t             _array_count,
        uint8_t             _plane_index = 0,
        uint8_t             _plane_count = s_all)
        : aspect(_aspect),
          mip_index(_mip_index),
          array_index(_array_index),
          array_count(_array_count),
          plane_index(_plane_index),
          plane_count(_plane_count) {}

    RHISubresourceSlice(
        ETextureAspectFlags _aspect,
        uint32_t            _mip_index,
        uint32_t            _array_index,
        uint32_t            _plane_index = 0)
        : aspect(_aspect),
          mip_index(_mip_index),
          array_index(_array_index),
          array_count(1),
          plane_index(_plane_index),
          plane_count(s_all) {}

    RHISubresourceSlice() : RHISubresourceSlice(
                                ETextureAspectFlags::COLOR,
                                0,
                                0,
                                1,
                                0,
                                s_all) {}
    inline bool IsAllArraySlices() const {
        return array_count == s_all;
    }
    //usually set to zero
    inline bool IsAllPlaneSlices() const {
        return plane_count == s_all;
    }

    inline bool IgnoreDepthPlane() const {
        return aspect == ETextureAspectFlags::STENCIL_SLICE;
    }

    inline bool IgnoreStencilPlane() const {
        return aspect == ETextureAspectFlags::DEPTH_SLICE;
    }

    inline bool operator==(RHISubresourceSlice const& rhs) const {
        return mip_index == rhs.mip_index && array_index == rhs.array_index && array_count == rhs.array_count && plane_index == rhs.plane_index && plane_count == rhs.plane_count;
    }

    inline bool operator!=(RHISubresourceSlice const& rhs) const {
        return !(*this == rhs);
    }
};
struct RHISubresourceRange : public RHISubresourceSlice {

    uint8_t num_mips = s_all;

    RHISubresourceRange(ETextureAspectFlags _aspect) : RHISubresourceSlice(
                                                           _aspect,
                                                           0,
                                                           0){};
    RHISubresourceRange(
        ETextureAspectFlags _aspect,
        uint32_t            _mip_index,
        uint32_t            _num_mips,
        uint32_t            _array_index,
        uint32_t            _array_count,
        uint32_t            _plane_index,
        uint32_t            _plane_count)
        : RHISubresourceSlice(_aspect, _mip_index, _array_index, _array_count, _plane_index, _plane_count),
          num_mips(_num_mips) {}

    RHISubresourceRange(
        ETextureAspectFlags _aspect,
        uint32_t            _mip_index,
        uint32_t            _array_index,
        uint32_t            _plane_index)
        : RHISubresourceSlice(_aspect,
                              _mip_index,
                              _array_index,
                              _plane_index) {}

    RHISubresourceRange() : RHISubresourceRange(ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, (uint32_t)s_all) {}
    inline bool IsAllMips() const {
        return num_mips == s_all;
    }

    inline bool IsAllArraySlices() const {
        return array_count == s_all;
    }
    //usually set to zero
    inline bool IsAllPlaneSlices() const {
        return plane_count == s_all;
    }

    inline bool IsWholeResource() const {
        return IsAllMips() && IsAllArraySlices() && IsAllPlaneSlices();
    }

    inline bool IgnoreDepthPlane() const {
        return aspect == ETextureAspectFlags::STENCIL_SLICE;
    }

    inline bool IgnoreStencilPlane() const {
        return aspect == ETextureAspectFlags::DEPTH_SLICE;
    }

    inline bool operator==(RHISubresourceRange const& rhs) const {
        return mip_index == rhs.mip_index && num_mips == rhs.num_mips && array_index == rhs.array_index && array_count == rhs.array_count && plane_index == rhs.plane_index && plane_count == rhs.plane_count;
    }

    inline bool operator!=(RHISubresourceRange const& rhs) const {
        return !(*this == rhs);
    }
};

struct RHICopyTextureInfo {
    RHISubresourceSlice src_slice;
    RHISubresourceSlice dst_slice;
    ETextureLayout      src_layout;
    ETextureLayout      dst_layout;

    Offset3D src_offset;

    Offset3D dst_offset;
    Extent3D extent;

    RHICopyTextureInfo(ETextureLayout      _src_layout,
                       RHISubresourceSlice _src_slice,
                       ETextureLayout      _dst_layout,
                       RHISubresourceSlice _dst_slice,
                       Offset3D            _src_offset,
                       Offset3D            _dst_offset,
                       Extent3D            _extent)
        : src_layout(_src_layout),
          dst_layout(_dst_layout),
          src_slice(_src_slice),
          dst_slice(_dst_slice),
          src_offset(_src_offset),
          dst_offset(_dst_offset),
          extent(_extent) {
    }
};

struct RHICopyTextureToBufferInfo {
    RHISubresourceSlice texture_slice;

    uint64_t buffer_offset;
    /* The buffer_row_length is the number of pixels from one row to the next.
     * The buffer_texture_height is the number of rows from one texture layer to the next.*/
    uint32_t buffer_row_length;
    uint32_t buffer_texture_height;

    Offset3D texture_offset;
    Extent3D texture_extent;

    ETextureLayout src_layout;

    RHICopyTextureToBufferInfo(
        ETextureLayout      _src_layout,
        Extent3D            _extent,
        RHISubresourceSlice _slice)
        : src_layout(_src_layout),
          texture_extent(_extent),
          buffer_offset(0),
          buffer_row_length(0),
          buffer_texture_height(0),
          texture_offset(0),
          texture_slice(_slice) {
    }

    RHICopyTextureToBufferInfo(
        ETextureLayout      _src_layout,
        Offset3D            _texture_offset,
        Extent3D            _extent,
        RHISubresourceSlice _slice,
        uint64_t            _buffer_offset,
        uint32_t            _buffer_row_length     = 0,
        uint32_t            _buffer_texture_height = 0)
        : src_layout(_src_layout),
          texture_extent(_extent),
          buffer_offset(_buffer_offset),
          buffer_row_length(_buffer_row_length),
          buffer_texture_height(_buffer_texture_height),
          texture_offset(_texture_offset),
          texture_slice(_slice) {
    }
};
struct RHICopyBufferToTextureInfo {

    RHISubresourceSlice texture_slice;
    uint64_t            buffer_offset;
    /* he buffer_row_length is the number of pixels from one row to the next.
     * The buffer_texture_height is the number of rows from one texture layer to the next.*/
    uint32_t buffer_row_length;
    uint32_t buffer_texture_height;

    Offset3D texture_offset;
    Extent3D texture_extent;

    ETextureLayout dst_layout;

    RHICopyBufferToTextureInfo(
        ETextureLayout      _dst_layout,
        Extent3D            _extent,
        RHISubresourceSlice _slice)
        : dst_layout(_dst_layout),
          texture_extent(_extent),
          buffer_offset(0),
          buffer_row_length(0),
          buffer_texture_height(0),
          texture_offset(0),
          texture_slice(_slice) {
    }

    RHICopyBufferToTextureInfo(
        ETextureLayout      _dst_layout,
        Offset3D            _texture_offset,
        Extent3D            _extent,
        RHISubresourceSlice _slice,
        uint64_t            _buffer_offset,
        uint32_t            _buffer_row_length     = 0,
        uint32_t            _buffer_texture_height = 0)
        : dst_layout(_dst_layout),
          texture_extent(_extent),
          buffer_offset(_buffer_offset),
          buffer_row_length(_buffer_row_length),
          buffer_texture_height(_buffer_texture_height),
          texture_offset(_texture_offset),
          texture_slice(_slice) {
    }
};
struct RHIBufferRegion {
    uint32_t src_offset;
    uint32_t dst_offset;
    uint64_t size;
};
struct RHICopyBufferInfo {
    RHICopyBufferInfo() = default;
    RHICopyBufferInfo(RHIBufferRegion&& region) : regions({std::move(region)}) {
    }

    Moer::Array<RHIBufferRegion> regions;

private:
    bool Validate() const {
        uint64_t total_size = 0;
        for (auto& region : regions) {
            total_size += region.size;
        }
        return total_size > 0;
    }
};

/* todo: transition information definition */
struct RHIResourceTransitionInfo {
};

///* resource copy definition */
//struct RHICopyTextureRegion {
//    /** offset in texture */
//    uint32_t dst_x;
//    uint32_t dst_y;
//    uint32_t dst_z;
//
//    /** offset in source image data */
//    int32_t src_x;
//    int32_t src_y;
//    int32_t src_z;
//
//    /** size of region to copy */
//    uint32_t width;
//    uint32_t height;
//    uint32_t depth;
//
//    RHICopyTextureRegion() = default;
//    explicit RHICopyTextureRegion(int3 _dst_range, int3 _src_range, int3 _src_size)
//        : dst_x(_dst_range.x),
//          dst_y(_dst_range.y),
//          dst_z(_dst_range.z),
//          src_x(_src_range.x),
//          src_y(_src_range.y),
//          src_z(_src_range.z),
//          width(_src_size.x),
//          height(_src_size.y),
//          depth(_src_size.z) {}
//};

struct RHIDispatchIndirectParameters {
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
};

struct RHIDrawIndirectParameters {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t start_vertex_location;
    uint32_t start_instance_location;
};

struct RHIDrawIndexedIndirectParameters {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t start_index_location;
    int32_t  base_vertex_location;
    uint32_t start_instance_location;
};

/* todo: transient constructor definitions */

/* todo: ray-tracing constructor definitions */

#endif// !RHI_RESOURCE_INITIALIZER_H
