#include "ShadowDepthPass.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "misc/Timer.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/LogicalComponents.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

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

ShadowCacheConfigSnapshot build_shadow_cache_config_snapshot(const RasterConfig& ui_config) {
    ShadowCacheConfigSnapshot snapshot{};
    snapshot.shadow_map_mode                       = static_cast<int>(ui_config.shadow_map_mode);
    snapshot.shadow_sampling_mode                  = ui_config.shadow_sampling_mode;
    snapshot.shadow_csm_num_of_cascades            = ui_config.shadow_csm_num_of_cascades;
    snapshot.shadow_csm_sm_size                    = ui_config.shadow_csm_sm_size;
    snapshot.shadow_csm_lerp_factor                = ui_config.shadow_csm_lerp_factor;
    snapshot.shadow_csm_blend_percentage           = ui_config.shadow_csm_blend_percentage;
    snapshot.shadow_csm_blend_option               = ui_config.shadow_csm_blend_option;
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
    shadow_cache_entry.last_update_frame          = frame_index;
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

    LOG_INFO(MOER_TEXT("{}"), stream.str());
}

CSMCascadeCandidate build_csm_cascade_candidate(
    const float3&       light_direction,
    const Camera&       camera,
    const RasterConfig& ui_config,
    const float         frustum_near_ratio,
    const float         frustum_far_ratio
) {
    CSMCascadeCandidate candidate{};

    const float3 normalized_light_dir = Normalizef(light_direction);
    const float3 light_right          = Normalizef(Cross(normalized_light_dir, float3(0.f, 1.f, 0.f)));
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
    // - Get 最长对角线
    float max_cross_distance = Max(
        Lengthf(frustum_corner_in_light_space[4] - frustum_corner_in_light_space[6]), // 远平面对角线
        Lengthf(
            frustum_corner_in_light_space[0] - frustum_corner_in_light_space[6]
        ) // 近平面和远平面的最长对角线
    );

    // - Get AABB
    float3 min = frustum_corner_in_light_space[0];
    float3 max = frustum_corner_in_light_space[0];
    for (uint i = 1; i < 8; i++) {
        min = Min(min, frustum_corner_in_light_space[i]);
        max = Max(max, frustum_corner_in_light_space[i]);
    }

    // 最小跳跃单位，避免shadow swimming
    // Reference: https://zhuanlan.zhihu.com/p/116731971
    candidate.world_units_per_texel = max_cross_distance / ui_config.shadow_csm_sm_size;
    auto get_fixed_coord            = [&](float x) {
        return floorf(x / candidate.world_units_per_texel) * candidate.world_units_per_texel;
    };

    candidate.snapped_light_space_center = float3(
        get_fixed_coord((min.x + max.x) * 0.5f),
        get_fixed_coord((min.y + max.y) * 0.5f),
        get_fixed_coord(min.z - 0.01f)
    );

    //虚拟光源位置
    const float3 light_pos = world_to_light_view_rotate_only_inverse * candidate.snapped_light_space_center;

    // Get new z min & max in Light View Space
    float light_z_offset            = Dot(light_pos, light_direction);
    float aabb_min_z_in_light_space = min.z + light_z_offset;
    float aabb_max_z_in_light_space = max.z + light_z_offset;

    // 突发奇想的一个trick，用于修复以下问题：
    //   LightView2LightClip矩阵，会剔除摄像机视锥后方的一些Mesh。但是这些Mesh也需要产生阴影！
    // 这个Trick，可以便捷地解决这个问题。如果这个问题还会出现的话，只需要把下面的这个1.0f常数调大就可以了
    // 带来的缺点，就是z轴精度会降低（毕竟值域变大了）；但是我们有Inverse Depth，所以这个并不重要！
    float z_delta = (max.z - min.z) * 1.0f;

    float4x4 light_view_to_light_clip = MakeOrthoMatrixRH(
        -0.5f * max_cross_distance,
        0.5f * max_cross_distance,
        -0.5f * max_cross_distance,
        0.5f * max_cross_distance,
        aabb_min_z_in_light_space - z_delta,
        aabb_max_z_in_light_space + z_delta
    );
    light_view_to_light_clip[2][2] *= -1.f; // 反转z轴

    // Final Matrix
    // world to light clip0
    const float4x4 light_view_matrix =
        MakeLookatViewMatrixRH(light_pos, light_pos + light_direction, light_up);

    const float4x4 world_to_light_orth_matrix = light_view_to_light_clip * light_view_matrix;

    // 保存正交矩阵数据，供PCSS方向光软阴影使用
    float ortho_width = max_cross_distance;
    float z_near_val  = aabb_min_z_in_light_space - z_delta;
    float z_far_val   = aabb_max_z_in_light_space + z_delta;
    float z_range     = z_far_val - z_near_val;

    // x: Width, y: Height, z: ZRange, w: NearPlane
    candidate.scale_data        = float4(ortho_width, ortho_width, z_range, z_near_val);
    candidate.world2shadow_clip = world_to_light_orth_matrix;

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
                MOER_TEXT("Create ShadowMap Texture: {}, size ({}, {}), bindless handle: {}"),
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
                MOER_TEXT("Create PointLight ShadowMap: {}, size ({}, {}), bindless handle: {}"),
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

std::optional<entt::entity> ShadowDepthPass::GetMainLightDirectionEntity(RasterContext& context) {
    auto& r = context.scene.r();

    // 首先尝试查找有 CTagMainLight + CLightDirectional 的实体
    auto main_light_view = r.view<ecs::CLightDirectional, ecs::CTagMainLight>();
    auto main_it         = main_light_view.begin();

    if (main_it != main_light_view.end()) {
        return *main_it;
    }

    // 如果没有找到带 CTagMainLight 的，则查找第一个 CLightDirectional
    auto fallback_view = r.view<ecs::CLightDirectional>();
    auto fallback_it   = fallback_view.begin();

    if (fallback_it != fallback_view.end()) {
        return *fallback_it;
    }

    return std::nullopt;
}

std::optional<ecs::CLightDirectional> ShadowDepthPass::GetMainLightDirection(RasterContext& context) {
    auto entity_opt = GetMainLightDirectionEntity(context);
    if (!entity_opt.has_value()) {
        LOG_ERROR(
            MOER_TEXT("No directional light found in the scene. Please ensure at least one entity has ")
            "CLightDirectional component."
        );
        return std::nullopt;
    }

    auto& r = context.scene.r();
    return r.get<ecs::CLightDirectional>(entity_opt.value());
}

std::optional<entt::entity> ShadowDepthPass::GetMainPointLightEntity(RasterContext& context) {
    auto& r = context.scene.r();

    // 首先尝试查找有 CTagMainLight + CLightPoint 的实体
    auto main_light_view = r.view<ecs::CLightPoint, ecs::CTagMainLight>();
    auto main_it         = main_light_view.begin();

    if (main_it != main_light_view.end()) {
        return *main_it;
    }

    // 如果没有找到带 CTagMainLight 的，则查找第一个 CLightPoint
    auto fallback_view = r.view<ecs::CLightPoint>();
    auto fallback_it   = fallback_view.begin();

    if (fallback_it != fallback_view.end()) {
        return *fallback_it;
    }

    return std::nullopt;
}

std::optional<ecs::CLightPoint> ShadowDepthPass::GetMainPointLight(RasterContext& context) {
    auto entity_opt = GetMainPointLightEntity(context);
    if (!entity_opt.has_value()) {
        LOG_ERROR(
            MOER_TEXT("No point light found in the scene. Please ensure at least one entity has CLightPoint component.")
        );
        return std::nullopt;
    }

    auto& r = context.scene.r();
    return r.get<ecs::CLightPoint>(entity_opt.value());
}

void ShadowDepthPass::RenderCSM(RasterContext& context, const RasterConfig& ui_config, const Camera& camera) {
    enabled_cascade_layers = ui_config.shadow_csm_num_of_cascades;
    assert(enabled_cascade_layers <= CSM_MAX_CASCADES);

    const bool use_gpu_culling = ui_config.enable_frustum_culling;

    PrepareCSMResources(context, ui_config);

    // Light
    auto light_entity_opt = GetMainLightDirectionEntity(context);
    if (!light_entity_opt.has_value()) {
        return;
    }

    auto& r            = context.scene.r();
    auto  light_entity = light_entity_opt.value();

    const float near_clip = camera.GetNearClip();
    const float far_clip  = camera.GetFarClip();

    // 从 CTransform 计算方向（默认方向为 (0, 0, -1)）
    const auto& c_transform = r.get<ecs::CTransform>(light_entity);
    context.lighting_data.main_light_direction =
        Normalizef(c_transform.rotation.Rotate(float3(0.f, 0.f, -1.f)));
    context.csm_data.light_dir = context.lighting_data.main_light_direction;

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
            local_cascade_split_points = get_cascade_split_points(
                near_clip, far_clip, ui_config.shadow_csm_lerp_factor, enabled_cascade_layers
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
            frustum_far_ratio
        );
        auto&      shadow_cache_entry = context.csm_data.shadow_cache_entries[cascade_index];
        const bool shadow_cache_eligible =
            ui_config.shadow_cache_enabled && cascade_index >= disabled_cache_cascade_count;
        const float shadow_cache_threshold_in_texels =
            Max(0.0f, ui_config.shadow_cache_camera_move_threshold_in_texels[cascade_index]);
        const float shadow_cache_move_distance_in_texels =
            shadow_cache_entry.valid ? get_shadow_cache_move_in_texels(shadow_candidate, shadow_cache_entry) :
                                       0.0f;
        const bool shadow_cache_dirty = shadow_cache_settings_changed || !shadow_cache_entry.valid ||
                                        (shadow_cache_eligible && shadow_cache_move_distance_in_texels >
                                                                      shadow_cache_threshold_in_texels);
        const bool reuse_shadow_cache = shadow_cache_eligible && !shadow_cache_dirty;

        shadow_cache_eligible_flags[cascade_index]       = shadow_cache_eligible;
        shadow_cache_reuse_flags[cascade_index]          = reuse_shadow_cache;
        shadow_cache_move_in_texels[cascade_index]       = shadow_cache_move_distance_in_texels;
        shadow_cache_thresholds_in_texels[cascade_index] = shadow_cache_threshold_in_texels;

        if (reuse_shadow_cache) {
            context.lighting_data.world2shadow_clip[cascade_index] = shadow_cache_entry.world2shadow_clip;
            context.lighting_data.scale_data[cascade_index]        = shadow_cache_entry.scale_data;
            context.csm_data.world2shadow_clip[cascade_index]      = shadow_cache_entry.world2shadow_clip;
            continue;
        }

        context.lighting_data.world2shadow_clip[cascade_index] = shadow_candidate.world2shadow_clip;
        context.lighting_data.scale_data[cascade_index]        = shadow_candidate.scale_data;
        context.csm_data.world2shadow_clip[cascade_index]      = shadow_candidate.world2shadow_clip;

        if (use_gpu_culling) {
            m_culling_pass.Process(
                context,
                context.lighting_data.world2shadow_clip[cascade_index],
                context.scene.gpu_scene_res(),
                context.gpu_culling_buffers.shadow,
                nullptr,
                RasterTool::GetShadowCullingProfileScopeName(cascade_index)
            );
        }

        RenderShadow(
            context,
            ui_config,
            context.lighting_data.world2shadow_clip[cascade_index],
            Rect2D(0, 0, ui_config.shadow_csm_sm_size, ui_config.shadow_csm_sm_size),
            context.csm_data.shadow_map_textures[cascade_index].tex->GetView(),
            use_gpu_culling,
            std::format("Shadow Depth Pass - {}", cascade_index),
            cascade_index
        );

        store_shadow_cache_entry(shadow_cache_entry, shadow_candidate, shadow_cache_frame_index);
    }

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
            LOG_ERROR(MOER_TEXT("Shadow map mode {} not supported"), static_cast<int>(ui_config.shadow_map_mode));
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

    auto light_entity_opt = GetMainPointLightEntity(context);
    if (!light_entity_opt.has_value()) {
        return;
    }

    auto& r            = context.scene.r();
    auto  light_entity = light_entity_opt.value();

    const uint light_idx = 0; // 目前我们只处理第一个点光源的阴影
    auto&      cube_res  = context.point_shadow_data.shadow_cubes[light_idx];

    // 记录光源信息供 Lighting Pass 使用
    float near_plane    = camera.GetNearClip(); // 近平面 (根据场景尺度调整)
    float far_plane     = camera.GetFarClip();  // TODO:远平面 = 光源半径
    cube_res.near_plane = near_plane;
    cube_res.far_plane  = far_plane;

    // 从 CTransform 获取位置（translation 在矩阵的第 4 列的 xyz 分量）
    const auto& c_transform = r.get<ecs::CTransform>(light_entity);
    cube_res.light_pos      = float3(
        c_transform.d_world_transform[0].w,
        c_transform.d_world_transform[1].w,
        c_transform.d_world_transform[2].w
    );

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

        RenderShadow(
            context,
            config,
            view_proj,
            Rect2D(0, 0, config.shadow_csm_sm_size, config.shadow_csm_sm_size),
            face_view,
            false,
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
    bool                use_gpu_culling,
    std::string_view    pass_name,
    std::optional<uint> csm_profile_layer
) {
    GeometryPassBindlessParam param;
    param.world2clip = Transpose(view_proj);

    const auto& gpu_scene_res           = context.scene.gpu_scene_res();
    param.instance_buf_hdl              = gpu_scene_res.instance_buf.hdl;
    param.visible_instance_id_buf_hdl   = 0;
    param.use_visible_instance_id_remap = 0;
    param.primitive_buf_hdl             = gpu_scene_res.primitive_buf.hdl;
    param.position_buf_hdl              = gpu_scene_res.position_buf.hdl;
    param.packed_normal_buf_hdl         = gpu_scene_res.packed_normal_buf.hdl;
    param.packed_tangent_buf_hdl        = gpu_scene_res.packed_tangent_buf.hdl;
    param.texcoord0_buf_hdl             = gpu_scene_res.texcoord0_buf.hdl;
    param.material_buf_hdl              = gpu_scene_res.material_buf.hdl;

    param.enable_alpha_test             = config.geometry_enable_alpha_test ? 1 : 0;
    param.alpha_test_blend_pixel_cutoff = config.geometry_alpha_test_blend_pixel_cutoff;

    if (use_gpu_culling) {
        param.visible_instance_id_buf_hdl   = context.gpu_culling_buffers.shadow.visible_instance_id_buf.hdl;
        param.use_visible_instance_id_remap = 1;
    }

    if (csm_profile_layer.has_value()) {
        context.cmd_list.PushScopeWithTimeScope(
            RasterTool::GetShadowDrawProfileScopeName(csm_profile_layer.value())
        );
    }

    auto draw = context.cmd_list.Gfx(m_pso, context.bdls, param);

    if (use_gpu_culling) {
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
        return;
    }

    draw.DrawIndirect(
        pass_name,
        rect,
        {},
        IndexBuffer{gpu_scene_res.index_buf.buf->GetView(), EIndexElementType::IET_UINT32},
        gpu_scene_res.draw_cmd_buf.buf->GetView(),
        gpu_scene_res.draw_cmd_buf.buf->GetNumElement(),
        gpu_scene_res.draw_cmd_buf.buf->GetStride(),
        DepthAttachment(depth_view.GetTexture())
    );

    if (csm_profile_layer.has_value()) {
        context.cmd_list.PopScopeWithTimeScope();
    }
}

} // namespace Moer::Render::Raster