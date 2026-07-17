#include "ShadowDepthPass.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "misc/Timer.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

#include <cmath>
#include <sstream>

namespace {

using namespace Moer;
constexpr float DEG2RAD         = PI / 180.0f;
using ShadowCacheConfigSnapshot = Moer::Render::Raster::RasterContext::CSMData::ShadowCacheConfigSnapshot;
using ShadowCacheEntry          = Moer::Render::Raster::RasterContext::CSMData::ShadowCacheEntry;

struct CSMCascadeCandidate {
    float4x4 world2shadow_clip{};
    float4   scale_data{};
    float3   snapped_light_space_center = float3(0.f, 0.f, 0.f);
    float    world_units_per_texel      = 0.0f;
    float3   light_direction            = float3(0.f, 0.f, 0.f);
    float    absolute_light_z_min       = 0.0f;
    float    absolute_light_z_max       = 0.0f;
    float    receiver_z_min             = 0.0f;
    float    receiver_z_max             = 0.0f;
    uint     intersecting_caster_count  = 0u;
    bool     used_legacy_z_fallback     = false;
};

struct CSMCascadeSphere {
    float3 center = float3(0.f, 0.f, 0.f);
    float  radius = 0.0f;
};

StaticArray<float, CSM_MAX_CASCADES> get_csm_blend(
    const StaticArray<float, CSM_MAX_CASCADES> split_point_raw,
    const float                                blend_percentage,
    const uint                                 enabled_cascade_layers
) {
    StaticArray<float, CSM_MAX_CASCADES> blend_start_points;
    for (uint i = 0; i < enabled_cascade_layers; i++) {
        float width_raw       = (i == 0) ? split_point_raw[0] : (split_point_raw[i] - split_point_raw[i - 1]);
        blend_start_points[i] = split_point_raw[i] - width_raw * blend_percentage;
    }
    return blend_start_points;
}

StaticArray<float, CSM_MAX_CASCADES> get_cascade_split_points(
    const float near_clip,
    const float far_clip,
    const float lerp_factor,
    const uint  enabled_cascade_layers
) {
    StaticArray<float, CSM_MAX_CASCADES> split_points;
    for (uint i = 0; i < enabled_cascade_layers; i++) {
        float split_ratio  = (float)(i + 1) / (float)enabled_cascade_layers;
        float log_split    = near_clip * Pow(far_clip / near_clip, split_ratio);
        float linear_split = near_clip + (far_clip - near_clip) * split_ratio;
        split_points[i]    = Lerp(log_split, linear_split, lerp_factor);
    }
    return split_points;
}

StaticArray<float, CSM_MAX_CASCADES> transform_split_points_to_ratios(
    const StaticArray<float, CSM_MAX_CASCADES> split_points,
    const float                                near_clip,
    const float                                far_clip,
    const uint                                 enabled_cascade_layers
) {
    StaticArray<float, CSM_MAX_CASCADES> split_ratios;
    for (uint i = 0; i < enabled_cascade_layers; i++) {
        split_ratios[i] = (split_points[i] - near_clip) / (far_clip - near_clip);
    }
    return split_ratios;
}

StaticArray<float, CSM_MAX_CASCADES> transform_split_ratios_to_points(
    const StaticArray<float, CSM_MAX_CASCADES> split_ratios,
    const float                                near_clip,
    const float                                far_clip,
    const uint                                 enabled_cascade_layers
) {
    StaticArray<float, CSM_MAX_CASCADES> split_points;
    for (uint i = 0; i < enabled_cascade_layers; i++) {
        split_points[i] = near_clip + split_ratios[i] * (far_clip - near_clip);
    }
    return split_points;
}

float get_csm_auto_shadow_far_clip(const RasterConfig& ui_config, const float near_clip, const float far_clip) {
    const float max_cover_ratio =
        Clamp(ui_config.shadow_csm_auto_max_cover_ratio_of_camera, 1e-4f, 1.0f);
    return near_clip + (far_clip - near_clip) * max_cover_ratio;
}

ShadowCacheConfigSnapshot build_shadow_cache_config_snapshot(const RasterConfig& ui_config) {
    ShadowCacheConfigSnapshot snapshot{};
    snapshot.shadow_map_mode                       = static_cast<int>(ui_config.shadow_map_mode);
    snapshot.shadow_sampling_mode                  = ui_config.shadow_sampling_mode;
    snapshot.shadow_csm_num_of_cascades            = ui_config.shadow_csm_num_of_cascades;
    snapshot.shadow_csm_sm_size                    = ui_config.shadow_csm_sm_size;
    snapshot.shadow_csm_lerp_factor                = ui_config.shadow_csm_lerp_factor;
    snapshot.shadow_csm_blend_percentage           = ui_config.shadow_csm_blend_percentage;
    snapshot.shadow_csm_blend_option               = ui_config.shadow_csm_blend_option;
    snapshot.shadow_csm_auto_max_cover_ratio_of_camera =
        ui_config.shadow_csm_auto_max_cover_ratio_of_camera;
    snapshot.shadow_pcss_enabled                   = ui_config.shadow_pcss_enabled;
    snapshot.shadow_pcss_light_size_world          = ui_config.shadow_pcss_light_size_world;
    snapshot.shadow_cache_enabled                  = ui_config.shadow_cache_enabled;
    snapshot.shadow_cache_disable_first_n_cascades = ui_config.shadow_cache_disable_first_n_cascades;
    snapshot.shadow_csm_cover_ratio_of_camera      = ui_config.shadow_csm_cover_ratio_of_camera;
    snapshot.shadow_cache_camera_move_threshold_in_texels =
        ui_config.shadow_cache_camera_move_threshold_in_texels;
    return snapshot;
}

bool is_shadow_cache_config_equal(
    const ShadowCacheConfigSnapshot& lhs,
    const ShadowCacheConfigSnapshot& rhs
) {
    if (lhs.shadow_map_mode != rhs.shadow_map_mode || lhs.shadow_sampling_mode != rhs.shadow_sampling_mode ||
        lhs.shadow_csm_num_of_cascades != rhs.shadow_csm_num_of_cascades ||
        lhs.shadow_csm_sm_size != rhs.shadow_csm_sm_size ||
        lhs.shadow_csm_lerp_factor != rhs.shadow_csm_lerp_factor ||
        lhs.shadow_csm_blend_percentage != rhs.shadow_csm_blend_percentage ||
        lhs.shadow_csm_blend_option != rhs.shadow_csm_blend_option ||
        lhs.shadow_csm_auto_max_cover_ratio_of_camera !=
            rhs.shadow_csm_auto_max_cover_ratio_of_camera ||
        lhs.shadow_pcss_enabled != rhs.shadow_pcss_enabled ||
        lhs.shadow_pcss_light_size_world != rhs.shadow_pcss_light_size_world ||
        lhs.shadow_cache_enabled != rhs.shadow_cache_enabled ||
        lhs.shadow_cache_disable_first_n_cascades != rhs.shadow_cache_disable_first_n_cascades) {
        return false;
    }

    for (uint i = 0; i < CSM_MAX_CASCADES; i++) {
        if (lhs.shadow_csm_cover_ratio_of_camera[i] != rhs.shadow_csm_cover_ratio_of_camera[i] ||
            lhs.shadow_cache_camera_move_threshold_in_texels[i] !=
                rhs.shadow_cache_camera_move_threshold_in_texels[i]) {
            return false;
        }
    }

    return true;
}

void invalidate_shadow_cache_entries(Moer::Render::Raster::RasterContext::CSMData& csm_data) {
    for (auto& shadow_cache_entry : csm_data.shadow_cache_entries) {
        shadow_cache_entry.valid = false;
    }
}

void store_shadow_cache_entry(
    ShadowCacheEntry&          shadow_cache_entry,
    const CSMCascadeCandidate& candidate,
    const uint64_t             frame_index
) {
    shadow_cache_entry.valid                      = true;
    shadow_cache_entry.world2shadow_clip          = candidate.world2shadow_clip;
    shadow_cache_entry.scale_data                 = candidate.scale_data;
    shadow_cache_entry.snapped_light_space_center = candidate.snapped_light_space_center;
    shadow_cache_entry.world_units_per_texel      = candidate.world_units_per_texel;
    shadow_cache_entry.light_direction            = candidate.light_direction;
    shadow_cache_entry.absolute_light_z_min       = candidate.absolute_light_z_min;
    shadow_cache_entry.absolute_light_z_max       = candidate.absolute_light_z_max;
    shadow_cache_entry.last_update_frame          = frame_index;
}

bool is_shadow_cache_projection_compatible(
    const CSMCascadeCandidate& candidate,
    const ShadowCacheEntry&    shadow_cache_entry
) {
    const float direction_delta = Lengthf(candidate.light_direction - shadow_cache_entry.light_direction);
    if (direction_delta > 1e-5f) {
        return false;
    }

    const float width_epsilon = Max(candidate.world_units_per_texel * 0.01f, 1e-5f);
    if (std::abs(candidate.scale_data.x - shadow_cache_entry.scale_data.x) > width_epsilon) {
        return false;
    }

    const float z_epsilon = Max(candidate.world_units_per_texel * 0.1f, 1e-4f);
    return candidate.absolute_light_z_min >= shadow_cache_entry.absolute_light_z_min - z_epsilon &&
           candidate.absolute_light_z_max <= shadow_cache_entry.absolute_light_z_max + z_epsilon;
}

float get_shadow_cache_move_in_texels(
    const CSMCascadeCandidate& candidate,
    const ShadowCacheEntry&    shadow_cache_entry
) {
    const float  texel_size = Max(candidate.world_units_per_texel, 1e-6f);
    const float2 delta      = float2(
        candidate.snapped_light_space_center.x - shadow_cache_entry.snapped_light_space_center.x,
        candidate.snapped_light_space_center.y - shadow_cache_entry.snapped_light_space_center.y
    );
    return Lengthf(delta) / texel_size;
}

float3 get_stable_light_up(const float3& normalized_light_dir) {
    const float3 world_up    = float3(0.f, 1.f, 0.f);
    const float3 fallback_up = float3(0.f, 0.f, 1.f);
    return std::abs(Dot(normalized_light_dir, world_up)) > 0.99f ? fallback_up : world_up;
}

CSMCascadeSphere build_cascade_bounding_sphere(const StaticArray<float3, 8>& frustum_corners) {
    CSMCascadeSphere sphere{};
    for (const float3& corner : frustum_corners) {
        sphere.center += corner;
    }
    sphere.center *= 1.0f / 8.0f;

    for (const float3& corner : frustum_corners) {
        sphere.radius = Max(sphere.radius, Lengthf(corner - sphere.center));
    }
    return sphere;
}

bool is_finite(const float3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Box3D transform_bounds_to_snapped_light_space(
    const Box3D&    world_bounds,
    const float3x3& world_to_light_view_rotate_only,
    const float3&   snapped_light_space_center
) {
    if (!world_bounds.IsValid() || !is_finite(world_bounds.min) || !is_finite(world_bounds.max)) {
        return {};
    }

    const float3 world_center = world_bounds.GetCenter();
    const float3 world_half   = world_bounds.GetExtent() * 0.5f;
    const float3 light_center = world_to_light_view_rotate_only * world_center - snapped_light_space_center;
    const float3 light_half   = float3(
        std::abs(world_to_light_view_rotate_only[0][0]) * world_half.x +
            std::abs(world_to_light_view_rotate_only[0][1]) * world_half.y +
            std::abs(world_to_light_view_rotate_only[0][2]) * world_half.z,
        std::abs(world_to_light_view_rotate_only[1][0]) * world_half.x +
            std::abs(world_to_light_view_rotate_only[1][1]) * world_half.y +
            std::abs(world_to_light_view_rotate_only[1][2]) * world_half.z,
        std::abs(world_to_light_view_rotate_only[2][0]) * world_half.x +
            std::abs(world_to_light_view_rotate_only[2][1]) * world_half.y +
            std::abs(world_to_light_view_rotate_only[2][2]) * world_half.z
    );
    return Box3D(light_center - light_half, light_center + light_half);
}

void tick_and_log_shadow_cache(
    const RasterConfig&                         ui_config,
    const bool                                  shadow_cache_settings_changed,
    const uint                                  enabled_cascade_layers,
    const StaticArray<bool, CSM_MAX_CASCADES>&  shadow_cache_eligible_flags,
    const StaticArray<bool, CSM_MAX_CASCADES>&  shadow_cache_reuse_flags,
    const StaticArray<float, CSM_MAX_CASCADES>& shadow_cache_move_in_texels,
    const StaticArray<float, CSM_MAX_CASCADES>& shadow_cache_thresholds_in_texels
) {
    static LoopedTimer s_shadow_cache_log_timer(2.0, false);
    if (!s_shadow_cache_log_timer.Tick()) {
        return;
    }

    uint eligible_cascade_count = 0;
    uint reused_cascade_count   = 0;
    for (uint cascade_index = 0; cascade_index < enabled_cascade_layers; cascade_index++) {
        eligible_cascade_count += shadow_cache_eligible_flags[cascade_index] ? 1u : 0u;
        reused_cascade_count += shadow_cache_reuse_flags[cascade_index] ? 1u : 0u;
    }

    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    stream << "[ShadowCache] enabled=" << (ui_config.shadow_cache_enabled ? 1 : 0)
           << " invalidated=" << (shadow_cache_settings_changed ? 1 : 0)
           << " cascades=" << enabled_cascade_layers << " eligible=" << eligible_cascade_count
           << " reused=" << reused_cascade_count
           << " refreshed=" << (enabled_cascade_layers - reused_cascade_count);

    for (uint cascade_index = 0; cascade_index < enabled_cascade_layers; cascade_index++) {
        stream << " | c" << cascade_index << '=';
        if (!shadow_cache_eligible_flags[cascade_index]) {
            stream << "refresh(force)";
            continue;
        }

        if (shadow_cache_reuse_flags[cascade_index]) {
            stream << "reuse(";
        } else {
            stream << "refresh(";
        }

        stream << shadow_cache_move_in_texels[cascade_index] << '/'
               << shadow_cache_thresholds_in_texels[cascade_index] << ')';
    }

    LOG_INFO("{}", stream.str());
}

CSMCascadeCandidate build_csm_cascade_candidate(
    const float3&       light_direction,
    const Camera&       camera,
    const RasterConfig& ui_config,
    const float         frustum_near_ratio,
    const float         frustum_far_ratio,
    const Array<Box3D>& shadow_caster_bounds,
    const bool          shadow_caster_bounds_valid
) {
    CSMCascadeCandidate candidate{};

    const float3 normalized_light_dir = Normalizef(light_direction);
    candidate.light_direction         = normalized_light_dir;
    const float3 light_world_up       = get_stable_light_up(normalized_light_dir);
    const float3 light_right          = Normalizef(Cross(normalized_light_dir, light_world_up));
    const float3 light_up             = Normalizef(Cross(light_right, normalized_light_dir));

    // World Space to Light Space (Light View Space)
    // 假设光源在世界坐标系原点，z轴=平行光反方向，y轴=光源上方向，x轴=光源右方向
    // clang-format off
    const float3x3 world_to_light_view_rotate_only = float3x3(
        light_right.x,      light_right.y,      light_right.z,
        light_up.x,         light_up.y,         light_up.z,
        -normalized_light_dir.x, -normalized_light_dir.y, -normalized_light_dir.z
    );
    // clang-format on
    const float3x3 world_to_light_view_rotate_only_inverse = Inverse(world_to_light_view_rotate_only);

    /**
         * ShadowMap折磨了快两天，终于改对了。这里记录一下所有变换方面的坑：
         * 
         * - Clip Space vs NDC Space
         *   - Clip Space通过 xyz / w 变换到NDC
         *     - VertexShader输出float4顶点的值域为Clip Space，即 xy in [-w, w], z in [0, w]，w in (-inf, inf)
         *     - PixelShader输入float4顶点的值域为NDC，即 xy in [-1, 1], z in [0, 1]，w = 1
         *   - NDC的值域 决定了 Clip Space的值域
         *   - Vulkan中，NDC的值域为：x: [-1, 1], y: [-1, 1], z: [0, 1]
         *     - x轴正方向，为屏幕向右
         *     - y轴正方向，为屏幕向下【注意！在MoerEngine中，y轴正方向，为屏幕向上！【貌似】】
         *     - z轴正方向，为屏幕向内
         *   - 根据上面的结论，Clip Space的值域为：x: [-w, w], y: [-w, w], z: [0, w]
         *   - 换句话说，一个WorldSpace的坐标，经过ViewProjection变换后，值域应该为：x: [-w, w], y: [-w, w], z: [0, w]
         * 
         * - Inverse Depth
         *   - MoerEngine使用了Inverse Depth的Trick
         *   - 含义：正常来说，近平面z值为0，远平面z值为1；而Inverse Depth的做法是：近平面z值为1，远平面z值为0
         *   - 优势：远处的坐标，z值接近0；而浮点数精度在0附近非常非常高；所以用高精度表示无限远，这是符合直觉的
         *   - 影响范围：光栅化管线中，所有涉及Depth的地方，都需要考tInverse Depth
         *   - 需要考虑的内容
         *     - Depth Texture Clear Value应该为0，表示无限远
         *     - Z-test时，大的z值应该覆盖小的z值，和正常的Z-test相反
         *     - Geometry Pass / Shadow Depth Pass中，Projection Matrix应该考虑到反转z轴（即交换near_clip和far_clip的参数）
         *     - **重要**：如果ProjectionMatrix没有考虑inverse depth，那么就要在vertex shader中手动进行变换（还有lighting pass应用矩阵时）
         * 
         * 好文章&好评论区：https://zhuanlan.zhihu.com/p/116731971
         */

    // AABB
    StaticArray<float3, 8> frustum_corners = camera.GetFrustumCorners(frustum_near_ratio, frustum_far_ratio);
    StaticArray<float3, 8> frustum_corner_in_light_space;
    for (uint i = 0; i < 8; i++) {
        frustum_corner_in_light_space[i] = world_to_light_view_rotate_only * frustum_corners[i];
    }
    // - Get AABB
    float3 min = frustum_corner_in_light_space[0];
    float3 max = frustum_corner_in_light_space[0];
    for (uint i = 1; i < 8; i++) {
        min = Min(min, frustum_corner_in_light_space[i]);
        max = Max(max, frustum_corner_in_light_space[i]);
    }

    const CSMCascadeSphere cascade_sphere       = build_cascade_bounding_sphere(frustum_corners);
    const float3           sphere_center_in_light_space =
        world_to_light_view_rotate_only * cascade_sphere.center;
    const float stable_ortho_width = Max(cascade_sphere.radius * 2.0f, 1e-3f);

    // Stable CSM: keep XY projection size fixed for the cascade sphere and only move in texel-sized steps.
    candidate.world_units_per_texel = stable_ortho_width / ui_config.shadow_csm_sm_size;
    auto get_fixed_coord            = [&](float x) {
        return std::floor(x / candidate.world_units_per_texel) * candidate.world_units_per_texel;
    };

    candidate.snapped_light_space_center = float3(
        get_fixed_coord(sphere_center_in_light_space.x),
        get_fixed_coord(sphere_center_in_light_space.y),
        sphere_center_in_light_space.z
    );

    //虚拟光源位置
    const float3 light_pos = world_to_light_view_rotate_only_inverse * candidate.snapped_light_space_center;

    // Get new z min & max in Light View Space
    float light_z_offset            = Dot(light_pos, normalized_light_dir);
    float aabb_min_z_in_light_space = min.z + light_z_offset;
    float aabb_max_z_in_light_space = max.z + light_z_offset;

    candidate.receiver_z_min = aabb_min_z_in_light_space;
    candidate.receiver_z_max = aabb_max_z_in_light_space;

    const float half_ortho_width = stable_ortho_width * 0.5f;
    const float xy_guard         = candidate.world_units_per_texel * 2.0f;
    if (shadow_caster_bounds_valid) {
        for (const Box3D& world_bounds : shadow_caster_bounds) {
            const Box3D light_bounds = transform_bounds_to_snapped_light_space(
                world_bounds, world_to_light_view_rotate_only, candidate.snapped_light_space_center
            );
            if (!light_bounds.IsValid()) {
                continue;
            }

            const bool overlaps_x = light_bounds.max.x >= -half_ortho_width - xy_guard &&
                                    light_bounds.min.x <= half_ortho_width + xy_guard;
            const bool overlaps_y = light_bounds.max.y >= -half_ortho_width - xy_guard &&
                                    light_bounds.min.y <= half_ortho_width + xy_guard;
            if (!overlaps_x || !overlaps_y) {
                continue;
            }

            ++candidate.intersecting_caster_count;
            // In this light space, a blocker has a greater Z than the receiver. The receiver
            // frustum already supplies the downstream bound, so extending it with caster min-Z
            // only wastes depth precision and inflates PCSS distance scaling.
            aabb_max_z_in_light_space = Max(aabb_max_z_in_light_space, light_bounds.max.z);
        }
    }

    // Receiver geometry fixes the stable XY projection. Only primitive bounds intersecting that
    // XY prism extend the light-space Z slab; a small pad absorbs numeric and bounds jitter.
    float z_padding = 0.0f;
    if (shadow_caster_bounds_valid) {
        const float z_span = Max(aabb_max_z_in_light_space - aabb_min_z_in_light_space, 1e-3f);
        z_padding          = Max(0.01f, Max(candidate.world_units_per_texel * 2.0f, z_span * 0.005f));
    } else {
        // The geometry snapshot is optional. Until the first snapshot arrives, retain the legacy
        // receiver-thickness expansion so a transient missing snapshot cannot drop valid casters.
        z_padding                        = Max(max.z - min.z, 1e-3f);
        candidate.used_legacy_z_fallback = true;
    }

    float4x4 light_view_to_light_clip = MakeOrthoMatrixRH(
        -0.5f * stable_ortho_width,
        0.5f * stable_ortho_width,
        -0.5f * stable_ortho_width,
        0.5f * stable_ortho_width,
        aabb_min_z_in_light_space - z_padding,
        aabb_max_z_in_light_space + z_padding
    );
    light_view_to_light_clip[2][2] *= -1.f; // 反转z轴

    // Final Matrix
    // world to light clip0
    const float4x4 light_view_matrix =
        MakeLookatViewMatrixRH(light_pos, light_pos + normalized_light_dir, light_up);

    const float4x4 world_to_light_orth_matrix = light_view_to_light_clip * light_view_matrix;

    // 保存正交矩阵数据，供PCSS方向光软阴影使用
    float ortho_width = stable_ortho_width;
    float z_near_val  = aabb_min_z_in_light_space - z_padding;
    float z_far_val   = aabb_max_z_in_light_space + z_padding;
    float z_range     = z_far_val - z_near_val;

    // x: Width, y: Height, z: ZRange, w: NearPlane
    candidate.scale_data        = float4(ortho_width, ortho_width, z_range, z_near_val);
    candidate.world2shadow_clip = world_to_light_orth_matrix;
    candidate.absolute_light_z_min = z_near_val + candidate.snapped_light_space_center.z;
    candidate.absolute_light_z_max = z_far_val + candidate.snapped_light_space_center.z;

    return candidate; // RVO
}
}; // namespace

namespace Moer::Render::Raster {

ShadowDepthPass::ShadowDepthPass(RasterContext& context) : m_culling_pass(context) {
    // 1. PSO
    GfxPsoCreateInfo pso_info(
        RHIRasterizeInfo::Preset(),
        {}, // Vertex Buffers 通过 Bindless 访问
        {}, // Shadow depth pass 不需要 color attachments
        RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),
        context.textures.depth_linear_sampler.tex->GetFormat()
    );

    ShadowDepthPassPipeline::MutationSet mutation_set{};
    mutation_set.SetMutation<ShadowDepthPassPipeline::SHADOW_DEPTH_PASS>(true);

    Shader& vtx = ShaderManager::Get().CompileShader(
        ST_VERTEX, "pipelines/raster/deferred/geometry/GeometryPassVertex.hlsl", mutation_set
    );
    Shader& frag = ShaderManager::Get().CompileShader(
        ST_FRAGMENT, "pipelines/raster/deferred/geometry/GeometryPassPixel.hlsl", mutation_set
    );

    m_pso = ShaderManager::Get().Raster().Vertex(vtx).Pixel(frag).Build<ShadowDepthPassPipeline>(
        std::move(pso_info)
    );
}

bool ShadowDepthPass::RefreshShadowCasterBounds(RasterContext& context) {
    const SceneUpdateBatch& scene_updates = context.GetSceneUpdates();
    if (!scene_updates.scene_ready) {
        if (!m_shadow_caster_bounds_valid && m_shadow_caster_bounds.empty()) {
            return false;
        }

        m_shadow_caster_bounds.clear();
        m_shadow_caster_bounds_valid = false;
        ++m_shadow_caster_bounds_generation;
        if (m_shadow_caster_bounds_generation == 0u) {
            ++m_shadow_caster_bounds_generation;
        }
        m_log_cascade_bounds_next_render = true;
        LOG_DEBUG(
            "[CSM] Shadow caster bounds invalidated while the scene is not ready: generation={}.",
            m_shadow_caster_bounds_generation
        );
        return true;
    }

    if (!scene_updates.geometry) {
        return false;
    }

    m_shadow_caster_bounds       = scene_updates.geometry->primitive_bounds;
    m_shadow_caster_bounds_valid = true;
    ++m_shadow_caster_bounds_generation;
    if (m_shadow_caster_bounds_generation == 0u) {
        ++m_shadow_caster_bounds_generation;
    }
    if (m_shadow_caster_bounds_generation == 1u) {
        m_shadow_caster_bounds_log_timer.Reset(false);
        m_log_cascade_bounds_next_render = true;
    } else {
        m_log_cascade_bounds_next_render = m_shadow_caster_bounds_log_timer.Tick();
    }
    if (m_log_cascade_bounds_next_render) {
        LOG_INFO(
            "[CSM] Shadow caster bounds rebuilt: generation={}, primitive_bounds={}, leaf_primitives={}, "
            "skipped_invalid={}.",
            m_shadow_caster_bounds_generation,
            m_shadow_caster_bounds.size(),
            scene_updates.geometry->leaf_primitive_count,
            scene_updates.geometry->skipped_invalid_count
        );
    }
    return true;
}

void ShadowDepthPass::PrepareCSMResources(RasterContext& context, const RasterConfig& ui_config) {
    for (uint i = 0; i < enabled_cascade_layers; i++) {
        auto& shadow_map_texture = context.csm_data.shadow_map_textures[i];

        bool b_need_to_create = shadow_map_texture.tex == nullptr ||
                                shadow_map_texture.tex->GetWidth() != ui_config.shadow_csm_sm_size ||
                                shadow_map_texture.tex->GetHeight() != ui_config.shadow_csm_sm_size;

        if (b_need_to_create) {

            if (shadow_map_texture.tex) {
                assert(shadow_map_texture.hdl != 0 && "ShadowMap Texture handle is 0");
                AssetTool::FreeRasterResourceHandle(context.bdls, shadow_map_texture);
            }

            uint2     sm_size(ui_config.shadow_csm_sm_size, ui_config.shadow_csm_sm_size);
            TexConfig tex_cfg =
                TexConfig::Depth(
                    context.textures.depth_linear_sampler.tex->GetFormat() // 使用普通DepthBuffer的格式
                )
                    .Size(sm_size)
                    .Usage(ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT)
                    .SamplerConfig(SF_LINEAR, SAM_CLAMP_TO_EDGE);

            AssetTool::CreateRasterResource<TexDepthTag>(
                shadow_map_texture,
                context.device,
                std::format("ShadowMapTexture_{}", i),
                sm_size,
                tex_cfg,
                false
            );

            AssetTool::AllocateRasterResourceHandle(context.bdls, shadow_map_texture, tex_cfg);

            LOG_DEBUG(
                "Create ShadowMap Texture: {}, size ({}, {}), bindless handle: {}",
                std::format("ShadowMapTexture_{}", i),
                ui_config.shadow_csm_sm_size,
                ui_config.shadow_csm_sm_size,
                shadow_map_texture.hdl
            );

            context.cmd_list.UpdateBindlessArray(context.bdls);
        }
    }
}

void ShadowDepthPass::PreparePointShadowResources(RasterContext& context, const RasterConfig& ui_config) {
    for (uint i = 0; i < RasterContext::PointShadowData::MAX_POINT_SHADOWS; i++) {
        auto& cube_res = context.point_shadow_data.shadow_cubes[i];

        // 检查是否需要创建或重建 (例如分辨率发生变化)
        bool b_need_to_create = cube_res.tex == nullptr ||
                                cube_res.tex->GetWidth() != ui_config.shadow_csm_sm_size ||
                                cube_res.tex->GetHeight() != ui_config.shadow_csm_sm_size;

        if (b_need_to_create) {
            // 清理旧资源
            if (cube_res.tex) {
                context.bdls->UnbindTexture(cube_res.handle);
                cube_res.tex = nullptr;
            }

            cube_res.name = std::format("PointShadowCube_{}", i);

            // 格式：复用 CSM 的深度格式 (通常是 D32_FLOAT)
            // 用途：既要被采样 (SAMPLED)，又要作为深度附件写入 (DEPTH_STENCIL_ATTACHMENT)
            cube_res.tex = context.device.CreateCubeMap(
                cube_res.name.c_str(),
                Extent2D(ui_config.shadow_csm_sm_size, ui_config.shadow_csm_sm_size),
                context.textures.depth_linear_sampler.tex->GetFormat(),
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
            );

            // GetView 必须请求完整的 6 层 (0, 1, 0, 6)，这样 Shader 才能把它当 Cube 采
            cube_res.handle =
                context.bdls->AllocateTexture(cube_res.tex, Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE));

            LOG_DEBUG(
                "Create PointLight ShadowMap: {}, size ({}, {}), bindless handle: {}",
                cube_res.name,
                ui_config.shadow_csm_sm_size,
                ui_config.shadow_csm_sm_size,
                cube_res.handle
            );

            // 5. 更新 Bindless Array (提交描述符更新)
            context.cmd_list.UpdateBindlessArray(context.bdls);
        }
    }
}

std::optional<ecs::CLightDirectional> ShadowDepthPass::GetMainLightDirection(RasterContext& context) {
    const auto& light = context.GetSceneUpdates().main_directional_light;
    if (!light) {
        LOG_ERROR(
            "No directional light found in the scene. Please ensure at least one entity has "
            "CLightDirectional component."
        );
        return std::nullopt;
    }

    return light;
}

std::optional<ecs::CLightPoint> ShadowDepthPass::GetMainPointLight(RasterContext& context) {
    const auto& light = context.GetSceneUpdates().main_point_light;
    if (!light) {
        LOG_ERROR(
            "No point light found in the scene. Please ensure at least one entity has CLightPoint component."
        );
        return std::nullopt;
    }

    return light;
}

void ShadowDepthPass::RenderCSM(RasterContext& context, const RasterConfig& ui_config, const Camera& camera) {
    enabled_cascade_layers = ui_config.shadow_csm_num_of_cascades;
    assert(enabled_cascade_layers <= CSM_MAX_CASCADES);

    PrepareCSMResources(context, ui_config);

    // Light
    auto light = GetMainLightDirection(context);
    if (!light) {
        return;
    }

    const float near_clip = camera.GetNearClip();
    const float far_clip  = camera.GetFarClip();

    const auto& c_light_directional            = *light;
    context.lighting_data.main_light_direction = Normalizef(c_light_directional.d_direction);
    context.csm_data.light_dir                 = context.lighting_data.main_light_direction;

    const uint disabled_cache_cascade_count = static_cast<uint>(
        Max(0, Min(ui_config.shadow_cache_disable_first_n_cascades, static_cast<int>(enabled_cascade_layers)))
    );
    const auto shadow_cache_snapshot = build_shadow_cache_config_snapshot(ui_config);
    const bool shadow_cache_settings_changed =
        !context.csm_data.shadow_cache_config_snapshot_valid ||
        !is_shadow_cache_config_equal(context.csm_data.shadow_cache_config_snapshot, shadow_cache_snapshot);
    if (shadow_cache_settings_changed) {
        invalidate_shadow_cache_entries(context.csm_data);
    }
    context.csm_data.shadow_cache_config_snapshot       = shadow_cache_snapshot;
    context.csm_data.shadow_cache_config_snapshot_valid = true;
    const uint64_t shadow_cache_frame_index             = ++context.csm_data.shadow_cache_frame_counter;

    //lerp csm ratios
    StaticArray<float, CSM_MAX_CASCADES>
        local_cascade_split_points; //actual split points between near_clip and far_clip
    switch (ui_config.shadow_map_mode) {
        case EShadowMapMode::CSM_AUTO: {
            const float shadow_far_clip = get_csm_auto_shadow_far_clip(ui_config, near_clip, far_clip);
            local_cascade_split_points = get_cascade_split_points(
                near_clip, shadow_far_clip, ui_config.shadow_csm_lerp_factor, enabled_cascade_layers
            );
            auto ratios = transform_split_points_to_ratios(
                local_cascade_split_points, near_clip, far_clip, enabled_cascade_layers
            );

            for (uint i = 0; i < enabled_cascade_layers; i++) {
                context.lighting_data.cascade_split_ratios[i] = ratios[i];
            }
            break;
        }
        case EShadowMapMode::CSM:
        default:
            local_cascade_split_points = transform_split_ratios_to_points(
                ui_config.shadow_csm_cover_ratio_of_camera, near_clip, far_clip, enabled_cascade_layers
            );
            for (uint i = 0; i < enabled_cascade_layers; i++) {
                context.lighting_data.cascade_split_ratios[i] = ui_config.shadow_csm_cover_ratio_of_camera[i];
            }
    }

    //混合
    auto local_cascade_blend_start_ratios = transform_split_points_to_ratios(
        get_csm_blend(
            local_cascade_split_points, ui_config.shadow_csm_blend_percentage, enabled_cascade_layers
        ),
        near_clip,
        far_clip,
        enabled_cascade_layers
    );
    for (uint i = 0; i < enabled_cascade_layers; i++) {
        context.lighting_data.cascade_blend_start_ratios[i] = local_cascade_blend_start_ratios[i];
    }

    StaticArray<bool, CSM_MAX_CASCADES>  shadow_cache_eligible_flags{};
    StaticArray<bool, CSM_MAX_CASCADES>  shadow_cache_reuse_flags{};
    StaticArray<float, CSM_MAX_CASCADES> shadow_cache_move_in_texels{};
    StaticArray<float, CSM_MAX_CASCADES> shadow_cache_thresholds_in_texels{};

    for (uint cascade_index = 0; cascade_index < enabled_cascade_layers; cascade_index++) {

        // World to Shadow Clip Matrix
        const float frustum_near_ratio =
            (cascade_index == 0) ? 0.0f : context.lighting_data.cascade_blend_start_ratios[cascade_index - 1];
        const float frustum_far_ratio = context.lighting_data.cascade_split_ratios[cascade_index];

        const auto shadow_candidate = build_csm_cascade_candidate(
            context.lighting_data.main_light_direction,
            camera,
            ui_config,
            frustum_near_ratio,
            frustum_far_ratio,
            m_shadow_caster_bounds,
            m_shadow_caster_bounds_valid
        );
        auto&      shadow_cache_entry = context.csm_data.shadow_cache_entries[cascade_index];
        const bool shadow_cache_eligible =
            ui_config.shadow_cache_enabled && cascade_index >= disabled_cache_cascade_count;
        const float shadow_cache_threshold_in_texels =
            Max(0.0f, ui_config.shadow_cache_camera_move_threshold_in_texels[cascade_index]);
        const float shadow_cache_move_distance_in_texels =
            shadow_cache_entry.valid ? get_shadow_cache_move_in_texels(shadow_candidate, shadow_cache_entry) :
                                       0.0f;
        const bool shadow_cache_projection_compatible =
            shadow_cache_entry.valid &&
            is_shadow_cache_projection_compatible(shadow_candidate, shadow_cache_entry);
        const bool shadow_cache_dirty = shadow_cache_settings_changed || !shadow_cache_entry.valid ||
                                        !shadow_cache_projection_compatible ||
                                        (shadow_cache_eligible && shadow_cache_move_distance_in_texels >
                                                                      shadow_cache_threshold_in_texels);
        const bool reuse_shadow_cache = shadow_cache_eligible && !shadow_cache_dirty;

        shadow_cache_eligible_flags[cascade_index]       = shadow_cache_eligible;
        shadow_cache_reuse_flags[cascade_index]          = reuse_shadow_cache;
        shadow_cache_move_in_texels[cascade_index]       = shadow_cache_move_distance_in_texels;
        shadow_cache_thresholds_in_texels[cascade_index] = shadow_cache_threshold_in_texels;

        if (m_log_cascade_bounds_next_render) {
            LOG_INFO(
                "[CSM] cascade={} primitive_bounds={} xy_intersections={} receiver_z=[{}, {}] final_z=[{}, "
                "{}] fallback={}.",
                cascade_index,
                m_shadow_caster_bounds.size(),
                shadow_candidate.intersecting_caster_count,
                shadow_candidate.receiver_z_min,
                shadow_candidate.receiver_z_max,
                shadow_candidate.scale_data.w,
                shadow_candidate.scale_data.w + shadow_candidate.scale_data.z,
                shadow_candidate.used_legacy_z_fallback ? 1 : 0
            );
        }

        if (reuse_shadow_cache) {
            context.lighting_data.world2shadow_clip[cascade_index] = shadow_cache_entry.world2shadow_clip;
            context.lighting_data.scale_data[cascade_index]        = shadow_cache_entry.scale_data;
            context.csm_data.world2shadow_clip[cascade_index]      = shadow_cache_entry.world2shadow_clip;
            continue;
        }

        context.lighting_data.world2shadow_clip[cascade_index] = shadow_candidate.world2shadow_clip;
        context.lighting_data.scale_data[cascade_index]        = shadow_candidate.scale_data;
        context.csm_data.world2shadow_clip[cascade_index]      = shadow_candidate.world2shadow_clip;

        m_culling_pass.Process(
            context,
            context.lighting_data.world2shadow_clip[cascade_index],
            context.GetGpuSceneRes(),
            context.gpu_culling_buffers.shadow,
            nullptr,
            RasterTool::GetShadowCullingProfileScopeName(cascade_index)
        );

        RenderShadow(
            context,
            ui_config,
            context.lighting_data.world2shadow_clip[cascade_index],
            Rect2D(0, 0, ui_config.shadow_csm_sm_size, ui_config.shadow_csm_sm_size),
            context.csm_data.shadow_map_textures[cascade_index].tex->GetView(),
            std::format("Shadow Depth Pass - {}", cascade_index),
            cascade_index
        );

        store_shadow_cache_entry(shadow_cache_entry, shadow_candidate, shadow_cache_frame_index);
    }

    m_log_cascade_bounds_next_render = false;

    // // 取消注释本段代码，可以在每帧渲染时，输出 Shadow Cache 的状态日志，便于调试和观察 Shadow Cache 的行为
    // tick_and_log_shadow_cache(
    //     ui_config,
    //     shadow_cache_settings_changed,
    //     enabled_cascade_layers,
    //     shadow_cache_eligible_flags,
    //     shadow_cache_reuse_flags,
    //     shadow_cache_move_in_texels,
    //     shadow_cache_thresholds_in_texels
    // );
}

void ShadowDepthPass::Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera) {
    const bool  shadow_caster_bounds_changed = RefreshShadowCasterBounds(context);
    const auto& scene_tick_state             = context.GetSceneUpdates().tick_state;
    const bool  main_light_changed =
        scene_tick_state.updated_light || scene_tick_state.created_light || scene_tick_state.destroyed_light;
    if (shadow_caster_bounds_changed || main_light_changed) {
        invalidate_shadow_cache_entries(context.csm_data);
    }

    if (ui_config.shadow_map_mode != EShadowMapMode::CSM &&
        ui_config.shadow_map_mode != EShadowMapMode::CSM_AUTO) {
        invalidate_shadow_cache_entries(context.csm_data);
        context.csm_data.shadow_cache_config_snapshot_valid = false;
    }

    switch (ui_config.shadow_map_mode) {
        case EShadowMapMode::NONE:
            break;
        case EShadowMapMode::POINT_CUBE:
            RenderPointShadows(context, ui_config, camera);
            break;
        case EShadowMapMode::CSM:
        case EShadowMapMode::CSM_AUTO:
            RenderCSM(context, ui_config, camera);
            break;
        default:
            LOG_ERROR("Shadow map mode {} not supported", static_cast<int>(ui_config.shadow_map_mode));
            break;
    }
    return;
}

void ShadowDepthPass::RenderPointShadows(
    RasterContext&      context,
    const RasterConfig& config,
    const Camera&       camera
) {
    PreparePointShadowResources(context, config);

    auto light = GetMainPointLight(context);
    if (!light) {
        return;
    }

    const uint light_idx = 0; // 目前我们只处理第一个点光源的阴影
    auto&      cube_res  = context.point_shadow_data.shadow_cubes[light_idx];

    // 记录光源信息供 Lighting Pass 使用
    float near_plane    = camera.GetNearClip(); // 近平面 (根据场景尺度调整)
    float far_plane     = camera.GetFarClip();  // TODO:远平面 = 光源半径
    cube_res.near_plane = near_plane;
    cube_res.far_plane  = far_plane;

    // GT 在 Scene sync 后已把 derived world position 拷入当前帧快照。
    cube_res.light_pos = light->d_position;

    // 计算投影矩阵 (90度 FOV, Aspect 1.0)
    // 交换了 near_plane 和 far_plane 以适应 Inverse Depth
    float4x4 proj = MakePerspectiveMatrixRH(90.0f * DEG2RAD, 1.0f, far_plane, near_plane);

    // 定义 6 个面的朝向 (方向, 上向量)
    // 顺序必须符合 CubeMap 标准: +X, -X, +Y, -Y, +Z, -Z
    struct FaceInfo {
        float3 dir;
        float3 up;
    };
    static const FaceInfo faces[] = {
        {{1, 0, 0}, {0, -1, 0}},  // +X (Right) - Vulkan Y-down usually implies Up is -Y for horizontal faces
        {{-1, 0, 0}, {0, -1, 0}}, // -X (Left)
        {{0, 1, 0}, {0, 0, 1}},   // +Y (Top)   - Up is +Z
        {{0, -1, 0}, {0, 0, -1}}, // -Y (Bottom)- Up is -Z
        {{0, 0, 1}, {0, -1, 0}},  // +Z (Front)
        {{0, 0, -1}, {0, -1, 0}}  // -Z (Back)
    };

    // 4. 遍历 6 个面进行渲染
    for (uint face = 0; face < 6; ++face) {
        float3   light_pos = cube_res.light_pos;
        float4x4 view      = MakeLookatViewMatrixRH(light_pos, light_pos + faces[face].dir, faces[face].up);
        float4x4 view_proj = proj * view;

        TextureView face_view = TextureView(cube_res.tex.Get()).Slice(face, 1);

        m_culling_pass.Process(
            context,
            view_proj,
            context.GetGpuSceneRes(),
            context.gpu_culling_buffers.shadow,
            nullptr,
            RasterTool::GetShadowCullingProfileScopeName(face)
        );

        RenderShadow(
            context,
            config,
            view_proj,
            Rect2D(0, 0, config.shadow_csm_sm_size, config.shadow_csm_sm_size),
            face_view,
            std::format("PointShadow L{} F{}", light_idx, face),
            std::nullopt
        );
    }
}

void ShadowDepthPass::RenderShadow(
    RasterContext&      context,
    const RasterConfig& config,
    const float4x4&     view_proj,
    const Rect2D&       rect,
    TextureView         depth_view,
    std::string_view    pass_name,
    std::optional<uint> csm_profile_layer
) {
    GeometryPassBindlessParam param;
    param.world2clip = Transpose(view_proj);

    const auto& gpu_scene_res           = context.GetGpuSceneRes();
    param.instance_buf_hdl              = gpu_scene_res.instance_buf.hdl;
    param.visible_instance_id_buf_hdl   = context.gpu_culling_buffers.shadow.visible_instance_id_buf.hdl;
    param.use_visible_instance_id_remap = 1;
    param.primitive_buf_hdl             = gpu_scene_res.primitive_buf.hdl;
    param.position_buf_hdl              = gpu_scene_res.position_buf.hdl;
    param.packed_normal_buf_hdl         = gpu_scene_res.packed_normal_buf.hdl;
    param.packed_tangent_buf_hdl        = gpu_scene_res.packed_tangent_buf.hdl;
    param.texcoord0_buf_hdl             = gpu_scene_res.texcoord0_buf.hdl;
    param.material_buf_hdl              = gpu_scene_res.material_buf.hdl;

    param.enable_alpha_test             = config.geometry_enable_alpha_test ? 1 : 0;
    param.alpha_test_blend_pixel_cutoff = config.geometry_alpha_test_blend_pixel_cutoff;

    if (csm_profile_layer.has_value()) {
        context.cmd_list.PushScopeWithTimeScope(
            RasterTool::GetShadowDrawProfileScopeName(csm_profile_layer.value())
        );
    }

    auto draw = context.cmd_list.Gfx(m_pso, context.bdls, param);

    const auto& visibility = context.gpu_culling_buffers.shadow;
    draw.DrawIndirect(
        pass_name,
        rect,
        {},
        IndexBuffer{gpu_scene_res.index_buf.buf->GetView(), EIndexElementType::IET_UINT32},
        visibility.draw_cmd_buf->GetView(),
        visibility.GetDrawCountView(),
        visibility.draw_cmd_buf->GetStride(),
        visibility.max_draw_count,
        DepthAttachment(depth_view.GetTexture())
    );

    if (csm_profile_layer.has_value()) {
        context.cmd_list.PopScopeWithTimeScope();
    }
}

} // namespace Moer::Render::Raster
