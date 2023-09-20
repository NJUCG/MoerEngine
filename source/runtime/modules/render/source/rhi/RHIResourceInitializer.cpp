#include "rhi/RHIResourceInitilizer.h"
#include "misc/Hash.h"
uint32_t GetHash(const RHISamplerInitializer& target) {
    uint32_t hash;
    hash_combine(hash, GetHash(target.filter));
    hash_combine(hash, GetHash(target.address_mode_u));
    hash_combine(hash, GetHash(target.address_mode_v));
    hash_combine(hash, GetHash(target.address_mode_w));
    hash_combine(hash, GetHash(target.mip_lod_bias));
    hash_combine(hash, GetHash(target.min_mip_level));
    hash_combine(hash, GetHash(target.max_mip_level));
    hash_combine(hash, GetHash(target.max_anisotropy));
    hash_combine(hash, GetHash(target.border_color));
    hash_combine(hash, GetHash(target.compare_op));
    return hash;
}
bool operator==(const RHISamplerInitializer& lhs, const RHISamplerInitializer& rhs) {
    return lhs.filter == rhs.filter &&
           lhs.address_mode_u == rhs.address_mode_u &&
           lhs.address_mode_v == rhs.address_mode_v &&
           lhs.address_mode_w == rhs.address_mode_w &&
           lhs.mip_lod_bias == rhs.mip_lod_bias &&
           lhs.min_mip_level == rhs.min_mip_level &&
           lhs.max_mip_level == rhs.max_mip_level &&
           lhs.max_anisotropy == rhs.max_anisotropy &&
           lhs.border_color == rhs.border_color &&
           lhs.compare_op == rhs.compare_op;
}

uint32_t GetHash(const RHIDepthStencilStateInitializer& target) {
    uint32_t hash = 0;
    hash_combine(hash, GetHash(target.b_enable_depth_write));
    hash_combine(hash, GetHash(target.depth_test_op));
    hash_combine(hash, GetHash(target.b_enable_front_face_stencil));
    hash_combine(hash, GetHash(target.front_face_stencil_test));
    hash_combine(hash, GetHash(target.front_face_stencil_fail_stencilOp));
    hash_combine(hash, GetHash(target.front_face_depth_fail_stencilOp));
    hash_combine(hash, GetHash(target.front_face_pass_stencil_op));
    hash_combine(hash, GetHash(target.b_enable_back_face_stencil));
    hash_combine(hash, GetHash(target.back_face_stencil_test));
    hash_combine(hash, GetHash(target.back_face_stencil_fail_stencil_op));
    hash_combine(hash, GetHash(target.back_face_depth_fail_stencil_op));
    hash_combine(hash, GetHash(target.back_face_pass_stencil_op));
    hash_combine(hash, GetHash(target.stencil_readmask));
    hash_combine(hash, GetHash(target.stencil_writemask));
    return hash;
}

bool operator==(const RHIDepthStencilStateInitializer& lhs, const RHIDepthStencilStateInitializer& rhs){
    return lhs.b_enable_depth_write == rhs.b_enable_depth_write &&
           lhs.depth_test_op == rhs.depth_test_op &&
           lhs.b_enable_front_face_stencil == rhs.b_enable_front_face_stencil &&
           lhs.front_face_stencil_test == rhs.front_face_stencil_test &&
           lhs.front_face_stencil_fail_stencilOp == rhs.front_face_stencil_fail_stencilOp &&
           lhs.front_face_depth_fail_stencilOp == rhs.front_face_depth_fail_stencilOp &&
           lhs.front_face_pass_stencil_op == rhs.front_face_pass_stencil_op &&
           lhs.b_enable_back_face_stencil == rhs.b_enable_back_face_stencil &&
           lhs.back_face_stencil_test == rhs.back_face_stencil_test &&
           lhs.back_face_stencil_fail_stencil_op == rhs.back_face_stencil_fail_stencil_op &&
           lhs.back_face_depth_fail_stencil_op == rhs.back_face_depth_fail_stencil_op &&
           lhs.back_face_pass_stencil_op == rhs.back_face_pass_stencil_op &&
           lhs.stencil_readmask == rhs.stencil_readmask &&
           lhs.stencil_writemask == rhs.stencil_writemask;
}

uint32_t GetHash(const RHIRasterizationStateInitializer& target) {
    uint32_t hash = 0;
    hash_combine(hash, GetHash(target.fill_mode));
    hash_combine(hash, GetHash(target.cull_mode));
    hash_combine(hash, GetHash(target.b_depth_bias));
    hash_combine(hash, GetHash(target.b_depth_clamp_enable));
    hash_combine(hash, GetHash(target.b_enable_msaa));
    hash_combine(hash, GetHash(target.depth_bias));
    hash_combine(hash, GetHash(target.depth_bias_clamp));
    hash_combine(hash, GetHash(target.depth_bias_slop_factor));
    return hash;
}
bool operator==(const RHIRasterizationStateInitializer& lhs, const RHIRasterizationStateInitializer& rhs) {
    return lhs.fill_mode == rhs.fill_mode &&
    lhs.cull_mode == rhs.cull_mode &&
    lhs.b_depth_bias == rhs.b_depth_bias &&
    lhs.b_depth_clamp_enable == rhs.b_depth_clamp_enable &&
    lhs.b_enable_msaa == rhs.b_enable_msaa &&
    lhs.depth_bias == rhs.depth_bias &&
    lhs.depth_bias_clamp == rhs.depth_bias_clamp &&
    lhs.depth_bias_slop_factor == rhs.depth_bias_slop_factor;
}
uint32_t GetHash(const RHIMultisampleStateInitializer& target) {
    uint32_t hash = 0;
    hash_combine(hash,GetHash(target.sample_count));
    hash_combine(hash,GetHash(target.b_sample_shading));
    hash_combine(hash,GetHash(target.b_alpha_to_converge));
    hash_combine(hash,GetHash(target.b_alpha_to_one));
    hash_combine(hash,GetHash(target.min_sample_shading));
    return hash;
}
bool operator==(const RHIMultisampleStateInitializer& lhs, const RHIMultisampleStateInitializer& rhs) {
    return lhs.sample_count == rhs.sample_count &&
    lhs.b_sample_shading == rhs.b_sample_shading &&
    lhs.b_alpha_to_converge == rhs.b_alpha_to_converge &&
    lhs.b_alpha_to_one == rhs.b_alpha_to_one &&
    lhs.min_sample_shading == rhs.min_sample_shading;
}
uint32_t GetHash(const RHIBlendStateInitializer::AttachmentInitializer& target) {
    uint32_t hash = 0;
    hash_combine(hash, GetHash(target.color_blend_op));
    hash_combine(hash, GetHash(target.color_src_blend_factor));
    hash_combine(hash, GetHash(target.color_dst_blend_factor));
    hash_combine(hash, GetHash(target.alpha_blend_op));
    hash_combine(hash, GetHash(target.alpha_src_blend_factor));
    hash_combine(hash, GetHash(target.alpha_dst_blend_factor));
    hash_combine(hash, GetHash(target.color_write_mask));
    return 0;
}
bool operator==(const RHIBlendStateInitializer::AttachmentInitializer& lhs, const RHIBlendStateInitializer::AttachmentInitializer& rhs) {
    return lhs.color_blend_op == rhs.color_blend_op &&
           lhs.color_src_blend_factor == rhs.color_src_blend_factor &&
           lhs.color_dst_blend_factor == rhs.color_dst_blend_factor &&
           lhs.alpha_blend_op == rhs.alpha_blend_op &&
           lhs.alpha_src_blend_factor == rhs.alpha_src_blend_factor &&
           lhs.alpha_dst_blend_factor == rhs.alpha_dst_blend_factor &&
           lhs.color_write_mask == rhs.color_write_mask;
}
uint32_t GetHash(const RHIBlendStateInitializer& target) {
    uint32_t hash = 0;
    for (int i = 0; i < MAX_PASS_ATTACHMENT_COUNT; ++i) {
        hash_combine(hash, GetHash(target.attachments[i]));
    }
    return hash;
}
bool operator==(const RHIBlendStateInitializer& lhs, const RHIBlendStateInitializer& rhs) {
    bool _same = true;
    for (int i = 0; i < MAX_PASS_ATTACHMENT_COUNT; ++i) {
        _same = _same && lhs.attachments[i] == rhs.attachments[i];
    }
    return _same;
}
