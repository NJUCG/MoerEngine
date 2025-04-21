#pragma once

#include "math/Function.h"
#include "scene/Camera.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "scene/light/LightComponentManager.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

#include "GeometryPass.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "ui/EditorUI.h"
#include "ui/raster_ui/RasterConfig.h"

namespace Moer::Render::Raster {

class ShadowDepthPass {
public:
    ShadowDepthPass(RasterContext& context) {}

    void CreateCsmData(RasterContext& context, const RasterConfig& ui_config) {
        // 检查并创建所有ShadowMap
        for (uint i = 0; i < ui_config.shadow_csm_num_of_cascades; i++) {
            auto& shadow_map_texture = context.shadow_map_textures[i];

            bool b_need_to_create = shadow_map_texture.tex == nullptr ||
                                    shadow_map_texture.tex->GetWidth() != ui_config.shadow_csm_sm_size ||
                                    shadow_map_texture.tex->GetHeight() != ui_config.shadow_csm_sm_size;

            // 创建ShadowMap
            if (b_need_to_create) {

                // 释放已有的Texture
                if (shadow_map_texture.tex) {
                    // TODO: 需要销毁申请的Texture吧。我发现RasterTextures.h中的texture也都没有销毁，是否是遗漏了？
                    // context.device.DestroyTexture(shadow_map_texture.tex);
                    context.bdls->FreeTexture(shadow_map_texture.handle);

                    shadow_map_texture.tex = nullptr;
                }

                // Name
                shadow_map_texture.name = std::format("ShadowMapTexture_{}", i);
                // Texture
                shadow_map_texture.tex = context.device.CreateDepthBuffer(
                    shadow_map_texture.name.c_str(),
                    Extent2D(ui_config.shadow_csm_sm_size, ui_config.shadow_csm_sm_size),
                    PF_D32_SFLOAT_S8_UINT,
                    1,
                    ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
                );
                // Handle
                // 默认使用 Linear Sampler
                shadow_map_texture.handle = context.bdls->AllocateTexture(
                    shadow_map_texture.tex->GetView(), Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE)
                );

                LOG_DEBUG(
                    "Create ShadowMap Texture: {}, size ({}, {}), bindless handle: {}",
                    shadow_map_texture.name,
                    ui_config.shadow_csm_sm_size,
                    ui_config.shadow_csm_sm_size,
                    shadow_map_texture.handle
                );

                // 草，texture采样一直全黑，研究了一晚上，发现忘记UpdateBindlessArray了😭
                context.cmd_list.UpdateBindlessArray(context.bdls);
            }
        }
    }

    std::optional<float3> GetMainLightDirection(RasterContext& context) {
        auto light_entity    = context.scene.GetMainLight();
        auto light_component = LightComponentManager::Get().Get(light_entity);
        if (light_component->GetType() != ELightComponentType::DIRECTIONAL) {
            context.scene.ForEach([&](Entity _entity) {
                auto light_component_tmp = LightComponentManager::Get().Get(light_entity);
                if (light_component_tmp->GetType() == ELightComponentType::DIRECTIONAL) {
                    light_component = light_component_tmp;
                    return;
                }
            });
        }
        if (light_component->GetType() != ELightComponentType::DIRECTIONAL) {
            LOG_WARNING("No Directional Light found in the scene, shadow depth pass will be skipped.");
            return std::nullopt;
        }
        auto* directional_light = dynamic_cast<const DirectionalLightComponent*>(light_component.Get());
        if (directional_light == nullptr) {
            LOG_ERROR("LightComponent is not DirectionalLightComponent! This should not happen, code error.");
            return std::nullopt;
        }

        return directional_light->GetDirection();
    }

    void ProcessCsm(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera) {

        // CsmData
        CreateCsmData(context, ui_config);

        // Light
        auto light_direction_optional = GetMainLightDirection(context);
        if (light_direction_optional.has_value() == false) { return; }

        const float3 light_direction = Normalizef(light_direction_optional.value());
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

        float3 test_vec_1 = camera->GetPosition() + camera->GetFront();
        float3 test_vec_2 = camera->GetPosition() + camera->GetFront() * 10.f + camera->GetRight() * 230.f +
                            camera->GetUp() * 230.f;
        float3 test_vec_3 = camera->GetPosition() + camera->GetFront() * 50.f - camera->GetRight() * 30.f -
                            camera->GetUp() * 30.f;
        float3 test_vec_4 = camera->GetPosition() + camera->GetFront() * 100.f;

        auto get_world_to_shadow_clip_matrix = [&](uint cascade_index) {
            // Cover ratio of camera frustum
            // Method 1: 每次视锥取 a[i-1] ~ a[i]
            // const float cover_ratio_near =
            //     cascade_index == 0 ? 0.f : ui_config.shadow_csm_cover_ratio_of_camera[cascade_index - 1];
            // Method 2：每次视锥都取 0 ~ a[i]
            const float cover_ratio_near = 0.f;
            const float cover_ratio_far  = ui_config.shadow_csm_cover_ratio_of_camera[cascade_index];

            // AABB
            // - Get Frustum
            StaticArray<float3, 8> frustum_corners =
                camera->GetFrustumCorners(cover_ratio_near, cover_ratio_far);
            // - Transform to Light Space
            StaticArray<float3, 8> frustum_corners_pre;
            for (uint i = 0; i < 8; i++) {
                frustum_corners_pre[i] = world_to_light_view_rotate_only * frustum_corners[i];
            }
            // - Get 最长对角线
            float max_cross_distance =
                Max(Lengthf(frustum_corners_pre[4] - frustum_corners_pre[6]), // 远平面对角线
                    Lengthf(frustum_corners_pre[0] - frustum_corners_pre[6])  // 近平面和远平面的最长对角线
                );
            // - Get AABB
            float3 min = frustum_corners_pre[0];
            float3 max = frustum_corners_pre[0];
            for (uint i = 1; i < 8; i++) {
                min = Min(min, frustum_corners_pre[i]);
                max = Max(max, frustum_corners_pre[i]);
            }

            // Light Pos
            // Reference: https://zhuanlan.zhihu.com/p/116731971
            const float world_units_per_texel = max_cross_distance / ui_config.shadow_csm_sm_size;
            auto        get_fixed_coord       = [&](float x) {
                return floorf(x / world_units_per_texel) * world_units_per_texel;
            };

            const float3 light_pos =
                world_to_light_view_rotate_only_inverse * Vector3f(
                                                              get_fixed_coord((min.x + max.x) * 0.5f),
                                                              get_fixed_coord((min.y + max.y) * 0.5f),
                                                              get_fixed_coord(min.z - 0.01f)
                                                          );

            // world to light clip
            const float4x4 world_to_light_view =
                MakeLookatViewMatrixRH(light_pos, light_pos + light_direction, light_up);

            /*
            上面这个函数等价于下面这段代码：
                // clang-format off
                const float4x4 world_to_light_view_rotate = float4x4(
                    light_right.x,      light_right.y,      light_right.z,      0.f,
                    light_up.x,         light_up.y,         light_up.z,         0.f,
                    -light_direction.x, -light_direction.y, -light_direction.z, 0.f,
                    0.f,                0.f,                0.f,                1.f
                );
                const float4x4 world_to_light_view_translate = float4x4(
                    1.f, 0.f, 0.f, -light_pos.x,
                    0.f, 1.f, 0.f, -light_pos.y,
                    0.f, 0.f, 1.f, -light_pos.z,
                    0.f, 0.f, 0.f, 1.f
                );
                // clang-format on
                const float4x4 world_to_light_view = world_to_light_view_rotate * world_to_light_view_translate;
            */

            // - Transform to Light Space
            StaticArray<float3, 8> frustum_corners_lst;
            for (uint i = 0; i < 8; i++) {
                float4 v4              = world_to_light_view * float4(frustum_corners[i], 1.0f);
                frustum_corners_lst[i] = v4.xyz / v4.w;
            }
            // - Get AABB
            min = frustum_corners_lst[0];
            max = frustum_corners_lst[0];
            for (uint i = 1; i < 8; i++) {
                min = Min(min, frustum_corners_lst[i]);
                max = Max(max, frustum_corners_lst[i]);
            }

            // 突发奇想的一个trick，用于修复以下问题：
            //   LightView2LightClip矩阵，会剔除摄像机视锥后方的一些Mesh。但是这些Mesh也需要产生阴影！
            // 这个Trick，可以便捷地解决这个问题。如果这个问题还会出现的话，只需要把下面的这个1.0f常数调大就可以了
            // 带来的缺点，就是z轴精度会降低（毕竟值域变大了）；但是我们有Inverse Depth，所以这个并不重要！
            float z_delta = (max.z - min.z) * 0.5f;

            float4x4 light_view_to_light_clip = MakeOrthoMatrixRH(
                -0.5f * max_cross_distance,
                0.5f * max_cross_distance,
                -0.5f * max_cross_distance,
                0.5f * max_cross_distance,
                min.z - z_delta,
                max.z + z_delta
            );
            light_view_to_light_clip[2][2] *= -1.f; // 反转z轴

            const float4x4 world_to_light_clip = light_view_to_light_clip * world_to_light_view;

            /*
            // 下面这段代码，用一个简单的world_to_light_clip矩阵来替代上面的复杂的自适应矩阵

            const float value_1 = 30.f;
            const float value_2 = 30.f;

            const float3   light_pos_2 = -value_1 * light_direction;
            const float4x4 world_to_light_view_2 =
                MakeLookatViewMatrixRH(light_pos_2, float3(0.f, 0.f, 0.f), light_up);
            const float4x4 light_view_to_light_clip_2 =
                MakeOrthoMatrixRH(-value_2, value_2, -value_2, value_2, 0.01f, value_1 * 1.5f);

            const float4x4 world_to_light_clip = light_view_to_light_clip_2 * world_to_light_view_2;
            */

            /*
            // 下面这段代码，用于测试：light_view_to_light_clip变换之后，越远的点，z值越大；越近的点，z值越小

            LOG_DEBUG("Light View to Light Clip Matrix: {}", light_view_to_light_clip.ToString());

            float3 test_vec_1 = float3((min.x + max.x) / 2.0f, (min.y + max.y) / 2.0f, min.z + 1.f);
            float3 test_vec_2 = float3((min.x + max.x) / 2.0f, (min.y + max.y) / 2.0f, min.z + 5.f);
            float3 test_vec_3 = float3((min.x + max.x) / 2.0f, (min.y + max.y) / 2.0f, min.z + 10.f);
            float3 test_vec_4 = float3((min.x + max.x) / 2.0f, (min.y + max.y) / 2.0f, min.z + 50.f);

            float4 test_vec_1_x = light_view_to_light_clip * float4(test_vec_1, 1.0f);
            float4 test_vec_2_x = light_view_to_light_clip * float4(test_vec_2, 1.0f);
            float4 test_vec_3_x = light_view_to_light_clip * float4(test_vec_3, 1.0f);
            float4 test_vec_4_x = light_view_to_light_clip * float4(test_vec_4, 1.0f);

            LOG_DEBUG("AABB Min: ({}, {}, {});", min.x, min.y, min.z);
            LOG_DEBUG("AABB Max: ({}, {}, {});", max.x, max.y, max.z);
            LOG_DEBUG(
                "Test Vec 1: ({}, {}, {}) => ({}, {}, {})",
                test_vec_1.x,
                test_vec_1.y,
                test_vec_1.z,
                test_vec_1_x.x / test_vec_1_x.w,
                test_vec_1_x.y / test_vec_1_x.w,
                test_vec_1_x.z / test_vec_1_x.w
            );
            LOG_DEBUG(
                "Test Vec 2: ({}, {}, {}) => ({}, {}, {})",
                test_vec_2.x,
                test_vec_2.y,
                test_vec_2.z,
                test_vec_2_x.x / test_vec_2_x.w,
                test_vec_2_x.y / test_vec_2_x.w,
                test_vec_2_x.z / test_vec_2_x.w
            );
            LOG_DEBUG(
                "Test Vec 3: ({}, {}, {}) => ({}, {}, {})",
                test_vec_3.x,
                test_vec_3.y,
                test_vec_3.z,
                test_vec_3_x.x / test_vec_3_x.w,
                test_vec_3_x.y / test_vec_3_x.w,
                test_vec_3_x.z / test_vec_3_x.w
            );
            LOG_DEBUG(
                "Test Vec 4: ({}, {}, {}) => ({}, {}, {})",
                test_vec_4.x,
                test_vec_4.y,
                test_vec_4.z,
                test_vec_4_x.x / test_vec_4_x.w,
                test_vec_4_x.y / test_vec_4_x.w,
                test_vec_4_x.z / test_vec_4_x.w
            );
            */

            /*
            LOG_DEBUG(" === ");
            std::string log_string = "";
            log_string += "Before Trans.\n";
            for (uint i = 0; i < 8; i++) {
                log_string += std::format(
                    "\tFrustum Corner {}: ({}, {}, {});\n",
                    i,
                    frustum_corners[i].x,
                    frustum_corners[i].y,
                    frustum_corners[i].z
                );
            }
            log_string += "After Trans.\n";
            for (uint i = 0; i < 8; i++) {
                log_string += std::format(
                    "\tFrustum Corner {}: ({}, {}, {});\n",
                    i,
                    frustum_corners_pre[i].x,
                    frustum_corners_pre[i].y,
                    frustum_corners_pre[i].z
                );
            }
            log_string += "After True Trans.\n";
            for (uint i = 0; i < 8; i++) {
                log_string += std::format(
                    "\tFrustum Corner {}: ({}, {}, {});\n",
                    i,
                    frustum_corners_lst[i].x,
                    frustum_corners_lst[i].y,
                    frustum_corners_lst[i].z
                );
            }
            log_string += std::format("Light Pos: ({}, {}, {});\n", light_pos.x, light_pos.y, light_pos.z);

            const float3 light_pos_in_light_view = (world_to_light_view * float4(light_pos, 1.0f)).xyz;
            const float3 light_pos_in_light_clip = (world_to_light_clip * float4(light_pos, 1.0f)).xyz;

            log_string += std::format(
                "Light Pos in Light View: ({}, {}, {});\n",
                light_pos_in_light_view.x,
                light_pos_in_light_view.y,
                light_pos_in_light_view.z
            );
            log_string += std::format(
                "Light Pos in Light Clip: ({}, {}, {});\n",
                light_pos_in_light_clip.x,
                light_pos_in_light_clip.y,
                light_pos_in_light_clip.z
            );
            LOG_DEBUG("{}", log_string);

            LOG_DEBUG("AABB Min: ({}, {}, {});", min.x, min.y, min.z);
            LOG_DEBUG("AABB Max: ({}, {}, {});", max.x, max.y, max.z);

            {
                LOG_DEBUG("World to Light View:");

                float4 test_vec_1_x = world_to_light_view * float4(test_vec_1, 1.0f);
                float4 test_vec_2_x = world_to_light_view * float4(test_vec_2, 1.0f);
                float4 test_vec_3_x = world_to_light_view * float4(test_vec_3, 1.0f);
                float4 test_vec_4_x = world_to_light_view * float4(test_vec_4, 1.0f);

                LOG_DEBUG(
                    "Test Vec 1: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_1.x,
                    test_vec_1.y,
                    test_vec_1.z,
                    test_vec_1_x.x / test_vec_1_x.w,
                    test_vec_1_x.y / test_vec_1_x.w,
                    test_vec_1_x.z / test_vec_1_x.w
                );
                LOG_DEBUG(
                    "Test Vec 2: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_2.x,
                    test_vec_2.y,
                    test_vec_2.z,
                    test_vec_2_x.x / test_vec_2_x.w,
                    test_vec_2_x.y / test_vec_2_x.w,
                    test_vec_2_x.z / test_vec_2_x.w
                );
                LOG_DEBUG(
                    "Test Vec 3: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_3.x,
                    test_vec_3.y,
                    test_vec_3.z,
                    test_vec_3_x.x / test_vec_3_x.w,
                    test_vec_3_x.y / test_vec_3_x.w,
                    test_vec_3_x.z / test_vec_3_x.w
                );
                LOG_DEBUG(
                    "Test Vec 4: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_4.x,
                    test_vec_4.y,
                    test_vec_4.z,
                    test_vec_4_x.x / test_vec_4_x.w,
                    test_vec_4_x.y / test_vec_4_x.w,
                    test_vec_4_x.z / test_vec_4_x.w
                );
            }
            {
                LOG_DEBUG("Light View to Light Clip Matrix: {}", light_view_to_light_clip.ToString());
                LOG_DEBUG("World to Light Clip:");

                float4 test_vec_1_x = world_to_light_clip * float4(test_vec_1, 1.0f);
                float4 test_vec_2_x = world_to_light_clip * float4(test_vec_2, 1.0f);
                float4 test_vec_3_x = world_to_light_clip * float4(test_vec_3, 1.0f);
                float4 test_vec_4_x = world_to_light_clip * float4(test_vec_4, 1.0f);

                LOG_DEBUG(
                    "Test Vec 1: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_1.x,
                    test_vec_1.y,
                    test_vec_1.z,
                    test_vec_1_x.x / test_vec_1_x.w,
                    test_vec_1_x.y / test_vec_1_x.w,
                    test_vec_1_x.z / test_vec_1_x.w
                );
                LOG_DEBUG(
                    "Test Vec 2: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_2.x,
                    test_vec_2.y,
                    test_vec_2.z,
                    test_vec_2_x.x / test_vec_2_x.w,
                    test_vec_2_x.y / test_vec_2_x.w,
                    test_vec_2_x.z / test_vec_2_x.w
                );
                LOG_DEBUG(
                    "Test Vec 3: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_3.x,
                    test_vec_3.y,
                    test_vec_3.z,
                    test_vec_3_x.x / test_vec_3_x.w,
                    test_vec_3_x.y / test_vec_3_x.w,
                    test_vec_3_x.z / test_vec_3_x.w
                );
                LOG_DEBUG(
                    "Test Vec 4: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_4.x,
                    test_vec_4.y,
                    test_vec_4.z,
                    test_vec_4_x.x / test_vec_4_x.w,
                    test_vec_4_x.y / test_vec_4_x.w,
                    test_vec_4_x.z / test_vec_4_x.w
                );
            }
            {
                LOG_DEBUG("View Transform:");

                auto world_to_clip = camera->GetViewMatrix();

                float4 test_vec_1_x = world_to_clip * float4(test_vec_1, 1.0f);
                float4 test_vec_2_x = world_to_clip * float4(test_vec_2, 1.0f);
                float4 test_vec_3_x = world_to_clip * float4(test_vec_3, 1.0f);
                float4 test_vec_4_x = world_to_clip * float4(test_vec_4, 1.0f);

                LOG_DEBUG(
                    "Test Vec 1: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_1.x,
                    test_vec_1.y,
                    test_vec_1.z,
                    test_vec_1_x.x / test_vec_1_x.w,
                    test_vec_1_x.y / test_vec_1_x.w,
                    test_vec_1_x.z / test_vec_1_x.w
                );
                LOG_DEBUG(
                    "Test Vec 2: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_2.x,
                    test_vec_2.y,
                    test_vec_2.z,
                    test_vec_2_x.x / test_vec_2_x.w,
                    test_vec_2_x.y / test_vec_2_x.w,
                    test_vec_2_x.z / test_vec_2_x.w
                );
                LOG_DEBUG(
                    "Test Vec 3: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_3.x,
                    test_vec_3.y,
                    test_vec_3.z,
                    test_vec_3_x.x / test_vec_3_x.w,
                    test_vec_3_x.y / test_vec_3_x.w,
                    test_vec_3_x.z / test_vec_3_x.w
                );
                LOG_DEBUG(
                    "Test Vec 4: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_4.x,
                    test_vec_4.y,
                    test_vec_4.z,
                    test_vec_4_x.x / test_vec_4_x.w,
                    test_vec_4_x.y / test_vec_4_x.w,
                    test_vec_4_x.z / test_vec_4_x.w
                );
            }
            {
                LOG_DEBUG("View Projection Transform:");

                auto world_to_clip = camera->GetViewProjectionMatrix();

                float4 test_vec_1_x = world_to_clip * float4(test_vec_1, 1.0f);
                float4 test_vec_2_x = world_to_clip * float4(test_vec_2, 1.0f);
                float4 test_vec_3_x = world_to_clip * float4(test_vec_3, 1.0f);
                float4 test_vec_4_x = world_to_clip * float4(test_vec_4, 1.0f);

                LOG_DEBUG(
                    "Test Vec 1: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_1.x,
                    test_vec_1.y,
                    test_vec_1.z,
                    test_vec_1_x.x / test_vec_1_x.w,
                    test_vec_1_x.y / test_vec_1_x.w,
                    test_vec_1_x.z / test_vec_1_x.w
                );
                LOG_DEBUG(
                    "Test Vec 2: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_2.x,
                    test_vec_2.y,
                    test_vec_2.z,
                    test_vec_2_x.x / test_vec_2_x.w,
                    test_vec_2_x.y / test_vec_2_x.w,
                    test_vec_2_x.z / test_vec_2_x.w
                );
                LOG_DEBUG(
                    "Test Vec 3: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_3.x,
                    test_vec_3.y,
                    test_vec_3.z,
                    test_vec_3_x.x / test_vec_3_x.w,
                    test_vec_3_x.y / test_vec_3_x.w,
                    test_vec_3_x.z / test_vec_3_x.w
                );
                LOG_DEBUG(
                    "Test Vec 4: ({}, {}, {}) => ({}, {}, {})",
                    test_vec_4.x,
                    test_vec_4.y,
                    test_vec_4.z,
                    test_vec_4_x.x / test_vec_4_x.w,
                    test_vec_4_x.y / test_vec_4_x.w,
                    test_vec_4_x.z / test_vec_4_x.w
                );
            }
            */

            return world_to_light_clip; // RVO
        };

        // Light Space Transform

        GeometryPassBindlessParam param;
        param.instance_data          = context.gpu_instance_info_handle;
        param.geometry_data          = context.gpu_geometry_info_handle;
        param.geometry_instance_data = context.gpu_geometry_instance_handle;

        shadow_depth_pass_names.resize(ui_config.shadow_csm_num_of_cascades);

        for (uint i = 0; i < ui_config.shadow_csm_num_of_cascades; i++) {
            // Name
            shadow_depth_pass_names[i] = std::format("Shadow Depth Pass - {}", i);
            // World to Shadow Clip Matrix
            context.world_to_shadow_clip[i] = get_world_to_shadow_clip_matrix(i);
        }

        for (uint i = 0; i < ui_config.shadow_csm_num_of_cascades; i++) {
            auto mesh_draw_datas_map = GeometryPass::GetMeshDrawDatasMap(context);

            param.world2clip = Transpose(context.world_to_shadow_clip[i]);

            DepthAttachment depth_attachment(context.shadow_map_textures[i].tex->GetView().GetTexture());

            context.cmd_list.GfxWithoutPso<ShadowDepthPassPipeline>(context.bdls, param)
                .DrawShadowDepthPass(
                    shadow_depth_pass_names[i],
                    Rect2D(0, 0, ui_config.shadow_csm_sm_size, ui_config.shadow_csm_sm_size),
                    std::move(mesh_draw_datas_map),
                    depth_attachment
                );
        }
    }

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
    void Process(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera) {
        if (ui_config.shadow_map_mode == 0) {
            return;
        } else if (ui_config.shadow_map_mode == 1) {
            ProcessCsm(context, ui_config, camera);
        } else if (ui_config.shadow_map_mode == 2) {
            // ProcessVsm(context, ui_config, camera);
        } else {
            LOG_ERROR("Shadow map mode {} not supported", ui_config.shadow_map_mode);
            return;
        }
    }

public:
    Array<std::string> shadow_depth_pass_names;
};

} // namespace Moer::Render::Raster