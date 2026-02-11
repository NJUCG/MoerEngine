#pragma once

#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "scene/camera/Camera.h"
#include "scene/light/LightComponentManager.h"
#include "shader/GeometryPassPsoManager.h"

#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "ShadowDepthPass.h"

namespace {

using namespace Moer;
constexpr float DEG2RAD = PI / 180.0f;

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

float4x4 get_world_to_shadow_clip_matrix(
    DirectionalLightComponent* light_direction_optional,
    CameraRef&                 camera,
    const RasterConfig&        ui_config,
    const float                frustum_near_ratio,
    const float                frustum_far_ratio,
    float4&                    outScaleData
) {
    const float3 light_direction = Normalizef(light_direction_optional->GetDirection());
    const float3 light_right     = Normalizef(Cross(light_direction, float3(0.f, 1.f, 0.f)));
    const float3 light_up        = Normalizef(Cross(light_right, light_direction));

    // World Space to Light Space (Light View Space)
    // 假设光源在世界坐标系原点，z轴=平行光反方向，y轴=光源上方向，x轴=光源右方向
    // clang-format off
    const float3x3 world_to_light_view_rotate_only = float3x3(
        light_right.x,      light_right.y,      light_right.z,
        light_up.x,         light_up.y,         light_up.z,
        -light_direction.x, -light_direction.y, -light_direction.z
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
    StaticArray<float3, 8> frustum_corners = camera->GetFrustumCorners(frustum_near_ratio, frustum_far_ratio);
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
    const float world_units_per_texel = max_cross_distance / ui_config.shadow_csm_sm_size;
    auto        get_fixed_coord       = [&](float x) {
        return floorf(x / world_units_per_texel) * world_units_per_texel;
    };

    //虚拟光源位置
    const float3 light_pos =
        world_to_light_view_rotate_only_inverse * Vector3f(
                                                      get_fixed_coord((min.x + max.x) * 0.5f),
                                                      get_fixed_coord((min.y + max.y) * 0.5f),
                                                      get_fixed_coord(min.z - 0.01f)
                                                  );

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

    outScaleData = float4(ortho_width, ortho_width, z_range, z_near_val);

    return world_to_light_orth_matrix; // RVO
}
}; // namespace

namespace Moer::Render::Raster {

ShadowDepthPass::ShadowDepthPass(RasterContext& context) :
    vertex_shader("pipelines/raster/deferred/geometry/GeometryPassCommonVertex.hlsl") {}

void ShadowDepthPass::PrepareCSMResources(RasterContext& context, const RasterConfig& ui_config) {
    for (uint i = 0; i < enabled_cascade_layers; i++) {
        auto& shadow_map_texture = context.csm_data.shadow_map_textures[i];

        bool b_need_to_create = shadow_map_texture.tex == nullptr ||
                                shadow_map_texture.tex->GetWidth() != ui_config.shadow_csm_sm_size ||
                                shadow_map_texture.tex->GetHeight() != ui_config.shadow_csm_sm_size;

        if (b_need_to_create) {

            // 释放已有的Texture
            if (shadow_map_texture.tex) {
                // TODO: 需要销毁申请的Texture吧。我发现RasterTextures.h中的texture也都没有销毁，是否是遗漏了？
                // context.device.DestroyTexture(shadow_map_texture.tex);
                context.bdls->FreeTexture(shadow_map_texture.handle);

                shadow_map_texture.tex = nullptr;
            }

            shadow_map_texture.name = std::format("ShadowMapTexture_{}", i);
            shadow_map_texture.tex  = context.device.CreateDepthBuffer(
                shadow_map_texture.name.c_str(),
                Extent2D(ui_config.shadow_csm_sm_size, ui_config.shadow_csm_sm_size),
                context.textures.depth_linear_sampler.tex->GetFormat(), // 使用普通DepthBuffer的格式
                1,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
            );

            shadow_map_texture.handle = context.bdls->AllocateTexture(
                shadow_map_texture.tex->GetView(), Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE)
            ); // 默认使用 Linear Sampler

            LOG_DEBUG(
                "Create ShadowMap Texture: {}, size ({}, {}), bindless handle: {}",
                shadow_map_texture.name,
                ui_config.shadow_csm_sm_size,
                ui_config.shadow_csm_sm_size,
                shadow_map_texture.handle
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
                context.bdls->FreeTexture(cube_res.handle);
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

DirectionalLightComponent* ShadowDepthPass::GetMainLightDirection(RasterContext& context) {

    auto lights          = context.scene.GetLights();
    auto light_component = LightComponentManager::Get().Get(lights[0]);

    for (int i = 1; i < lights.size(); i++) {
        auto light_entity            = lights[i];
        auto light_component_current = LightComponentManager::Get().Get(light_entity);
        if (light_component_current->GetType() == ELightComponentType::DIRECTIONAL) {
            light_component = light_component_current;
            break;
        }
    }

    if (light_component->GetType() != ELightComponentType::DIRECTIONAL) {
        LOG_WARNING("No Directional Light found in the scene, shadow depth pass will be skipped.");
        return nullptr;
    }
    auto* directional_light = dynamic_cast<DirectionalLightComponent*>(light_component.Get());
    if (directional_light == nullptr) {
        LOG_ERROR("LightComponent is not DirectionalLightComponent! This should not happen, code error.");
        return nullptr;
    }

    return directional_light;
}

PointLightComponent* ShadowDepthPass::GetMainPointLight(RasterContext& context) {

    auto lights          = context.scene.GetLights();
    auto light_component = LightComponentManager::Get().Get(lights[0]);

    for (int i = 1; i < lights.size(); i++) {
        auto light_entity            = lights[i];
        auto light_component_current = LightComponentManager::Get().Get(light_entity);
        if (light_component_current->GetType() == ELightComponentType::POINT) {
            light_component = light_component_current;
            break;
        }
    }

    if (light_component->GetType() != ELightComponentType::POINT) {
        LOG_WARNING("No Point Light found in the scene, shadow depth pass will be skipped.");
        return nullptr;
    }
    auto* point_light = dynamic_cast<PointLightComponent*>(light_component.Get());
    if (point_light == nullptr) {
        LOG_ERROR("LightComponent is not PointLightComponent! This should not happen, code error.");
        return nullptr;
    }

    return point_light;
}

void ShadowDepthPass::RenderCSM(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera) {
    enabled_cascade_layers = ui_config.shadow_csm_num_of_cascades;
    assert(enabled_cascade_layers <= CSM_MAX_CASCADES);

    PrepareCSMResources(context, ui_config);

    // Light
    auto light_direction_optional = GetMainLightDirection(context);
    if (light_direction_optional == nullptr) {
        return;
    }

    // Light Space Transform
    const float near_clip = camera->GetNearClip();
    const float far_clip  = camera->GetFarClip();

    context.csm_data.light_dir = Normalizef(light_direction_optional->GetDirection());

    //lerp csm ratios
    switch (ui_config.shadow_map_mode) {
        case EShadowMapMode::CSM_AUTO: {
            context.csm_data.cascade_split_points = get_cascade_split_points(
                near_clip, far_clip, ui_config.shadow_csm_lerp_factor, enabled_cascade_layers
            );
            context.csm_data.cascade_split_ratios = transform_split_points_to_ratios(
                context.csm_data.cascade_split_points, near_clip, far_clip, enabled_cascade_layers
            );
            break;
        }
        case EShadowMapMode::CSM:
        default:
            context.csm_data.cascade_split_points = transform_split_ratios_to_points(
                ui_config.shadow_csm_cover_ratio_of_camera, near_clip, far_clip, enabled_cascade_layers
            );
            context.csm_data.cascade_split_ratios = ui_config.shadow_csm_cover_ratio_of_camera;
    }

    //混合
    context.csm_data.cascade_blend_start_ratios = transform_split_points_to_ratios(
        get_csm_blend(
            context.csm_data.cascade_split_points,
            ui_config.shadow_csm_blend_percentage,
            enabled_cascade_layers
        ),
        near_clip,
        far_clip,
        enabled_cascade_layers
    );

    for (uint cascade_index = 0; cascade_index < enabled_cascade_layers; cascade_index++) {

        // World to Shadow Clip Matrix
        const float frustum_near_ratio =
            (cascade_index == 0) ? 0.0f : context.csm_data.cascade_blend_start_ratios[cascade_index - 1];
        const float frustum_far_ratio = context.csm_data.cascade_split_ratios[cascade_index];
        context.csm_data.world_to_shadow_clip[cascade_index] = get_world_to_shadow_clip_matrix(
            light_direction_optional,
            camera,
            ui_config,
            frustum_near_ratio,
            frustum_far_ratio,
            context.csm_data.scaleDatas[cascade_index]
        );

        RenderShadow(
            context,
            ui_config,
            context.csm_data.world_to_shadow_clip[cascade_index],
            Rect2D(0, 0, ui_config.shadow_csm_sm_size, ui_config.shadow_csm_sm_size),
            context.csm_data.shadow_map_textures[cascade_index].tex->GetView(),
            std::format("Shadow Depth Pass - {}", cascade_index),
            pipeline_map
        );
    }
}

void ShadowDepthPass::Process(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera) {
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
    CameraRef&          camera
) {
    PreparePointShadowResources(context, config);

    PointLightComponent* light = GetMainPointLight(context);
    if (light == nullptr) {
        return;
    }

    const uint light_idx = 0; // 目前我们只处理第一个点光源的阴影
    auto&      cube_res  = context.point_shadow_data.shadow_cubes[light_idx];

    // 记录光源信息供 Lighting Pass 使用
    float near_plane    = camera->GetNearClip(); // 近平面 (根据场景尺度调整)
    float far_plane     = camera->GetFarClip();  // TODO:远平面 = 光源半径
    cube_res.near_plane = near_plane;
    cube_res.far_plane  = far_plane;
    cube_res.light_pos  = light->GetPosition();

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
        float3   light_pos = light->GetPosition();
        float4x4 view      = MakeLookatViewMatrixRH(light_pos, light_pos + faces[face].dir, faces[face].up);
        float4x4 view_proj = proj * view;

        TextureView face_view = TextureView(cube_res.tex.Get()).Slice(face, 1);

        RenderShadow(
            context,
            config,
            view_proj,
            Rect2D(0, 0, config.shadow_csm_sm_size, config.shadow_csm_sm_size),
            face_view,
            std::format("PointShadow L{} F{}", light_idx, face),
            pipeline_map
        );
    }
}

void ShadowDepthPass::RenderShadow(
    RasterContext&                                              context,
    const RasterConfig&                                         config,
    const float4x4&                                             view_proj,
    const Rect2D&                                               rect,
    TextureView                                                 depth_view,
    std::string_view                                            pass_name,
    Moer::UnorderedMap<VertexFactory, ShadowDepthPassPipeline>& pipeline_map
) {
    GeometryPassBindlessParam param;
    param.instance_data                 = context.gpu_instance_info_handle;
    param.geometry_data                 = context.gpu_geometry_info_handle;
    param.geometry_instance_data        = context.gpu_geometry_instance_handle;
    param.world2clip                    = Transpose(view_proj);
    param.material_buffer               = context.gpu_material_info_handle;
    param.enable_alpha_test             = config.geometry_enable_alpha_test ? 1 : 0;
    param.alpha_test_blend_pixel_cutoff = config.geometry_alpha_test_blend_pixel_cutoff;

    auto arg_idx = context.cmd_list.RegisterArgs(ShadowDepthPassPipeline::SetArgs(context.bdls, param));

    // GetDrawMeshDatasMap 其实一帧只需要调一次，以后可以提到外层
    UnorderedMap<VertexFactory, Array<MeshDrawData>> mesh_draw_datas_map =
        RasterTool::GetDrawMeshDatasMap(context, true);

    for (auto& [factory, _] : mesh_draw_datas_map) {
        if (!pipeline_map.contains(factory)) {
            VertexStream     stream = factory.GetVertexStream();
            GfxPsoCreateInfo pso_info(
                RHIRasterizeInfo::Preset(),
                std::move(stream),
                {},
                RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),
                context.textures.depth_linear_sampler.tex->GetFormat()
            );
            Shader& vtx = vertex_shader.GetShader(const_cast<VertexFactory*>(&factory));
            ShadowDepthPassPipeline::MutationSet mutation_set{};
            mutation_set.SetMutation<ShadowDepthPassPipeline::SHADOW_DEPTH_PASS>(true);
            Shader& frag = ShaderManager::Get().CompileShader(
                ST_FRAGMENT, "pipelines/raster/deferred/geometry/GeometryPassCommonPixel.hlsl", mutation_set
            );
            pipeline_map.emplace(
                factory,
                ShaderManager::Get().Raster().Vertex(vtx).Pixel(frag).Build<ShadowDepthPassPipeline>(
                    std::move(pso_info)
                )
            );
        }
    }

    auto draw_batch = DrawBatch{};
    for (auto& [factory, draw_array] : mesh_draw_datas_map) {
        if (pipeline_map.contains(factory)) {
            draw_batch.Emplace(pipeline_map[factory].handle, arg_idx)
                .RegisterDrawDatas(std::move(draw_array));
        }
    }

    context.cmd_list.Gfx(pass_name, rect, DepthAttachment(depth_view))
        .AcceptDrawBatch(std::move(draw_batch))
        .Dispatch();
}

} // namespace Moer::Render::Raster