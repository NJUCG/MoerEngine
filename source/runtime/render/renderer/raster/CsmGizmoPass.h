#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include "renderer/EditorConfig.h"

#include <cstring>

namespace Moer::Render::Raster {

class CsmGizmoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(CsmGizmoPipeline);

    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(CsmGizmoParam, param);
    DEFINE_SHADER_ARGS(bdls, param);
};

class CsmGizmoPass {
public:
    CsmGizmoPass(RasterContext& context) {
        GfxPsoCreateInfo pso_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset<Blend::ALPHA_BLEND>(
                context.textures.tonemapping_output.tex->GetFormat()
            )},
            RHIDepthStencilStateInfo::Preset(),
            PF_UNDEFINED,
            EPrimitiveTopology::TRIANGLE_LIST
        );

        m_pipeline = context.manager.Raster()
                         .Vertex("pipelines/raster/deferred/lighting/CsmGizmo.hlsl", "CsmGizmoVS")
                         .Pixel("pipelines/raster/deferred/lighting/CsmGizmo.hlsl", "CsmGizmoPS")
                         .Build<CsmGizmoPipeline>(std::move(pso_info));

        m_cascade_data_buffer.buf = context.device.CreateBuffer<byte>(
            "Raster::CsmGizmoCascadeData",
            static_cast<uint>(sizeof(CsmGizmoCascadeData) * CSM_MAX_CASCADES),
            EBufferUsageFlags::UNORDERED_ACCESS
        );
        m_cascade_data_buffer.hdl = context.bdls->AllocateBuffer(m_cascade_data_buffer.buf->GetView());
    }

    void Process(
        RasterContext&               context,
        const RasterConfig&          raster_config,
        const SceneViewGizmoConfig&  gizmo_config,
        const Camera&                scene_camera,
        const Camera&                main_camera
    ) {
        if (!gizmo_config.show_csm ||
            (raster_config.shadow_map_mode != EShadowMapMode::CSM &&
             raster_config.shadow_map_mode != EShadowMapMode::CSM_AUTO)) {
            return;
        }

        uint draw_flags = 0u;
        if (gizmo_config.show_csm_split_frustums) {
            draw_flags |= RASTER_CSM_GIZMO_DRAW_SPLIT_FRUSTUM;
        }
        if (gizmo_config.show_csm_bounding_spheres) {
            draw_flags |= RASTER_CSM_GIZMO_DRAW_BOUNDING_SPHERE;
        }
        if (draw_flags == 0u || m_cascade_data_buffer.hdl == 0u) {
            return;
        }

        const uint cascade_count =
            Min(static_cast<uint>(raster_config.shadow_csm_num_of_cascades), static_cast<uint>(CSM_MAX_CASCADES));
        if (cascade_count == 0u) {
            return;
        }

        StaticArray<CsmGizmoCascadeData, CSM_MAX_CASCADES> cascade_data{};
        uint                                               valid_cascade_count = 0u;
        for (uint cascade_index = 0u; cascade_index < cascade_count; ++cascade_index) {
            if ((gizmo_config.csm_cascade_visibility_mask & (1u << cascade_index)) == 0u) {
                continue;
            }

            const float near_ratio = cascade_index == 0u ?
                                         0.0f :
                                         Clamp(context.lighting_data.cascade_blend_start_ratios[cascade_index - 1u],
                                               0.0f,
                                               1.0f);
            const float far_ratio = Clamp(context.lighting_data.cascade_split_ratios[cascade_index], 0.0f, 1.0f);
            if (far_ratio <= near_ratio + 1e-5f) {
                continue;
            }

            cascade_data[valid_cascade_count] =
                BuildCascadeData(main_camera, near_ratio, far_ratio, cascade_index);
            valid_cascade_count++;
        }

        if (valid_cascade_count == 0u) {
            return;
        }

        const size_t cascade_data_size = sizeof(CsmGizmoCascadeData) * valid_cascade_count;
        Array<byte>  cascade_data_upload(cascade_data_size);
        std::memcpy(cascade_data_upload.data(), cascade_data.data(), cascade_data_size);
        context.cmd_list.CopyFrom(
            std::move(cascade_data_upload),
            m_cascade_data_buffer.buf->GetView(),
            "Raster::CsmGizmoCascadeData"
        );

        CsmGizmoParam param{};
        param.world2clip = Transpose(scene_camera.GetViewProjectionMatrix());
        param.csm_config = uint4(m_cascade_data_buffer.hdl, valid_cascade_count, draw_flags, 0u);

        const Vector3f scene_camera_position = scene_camera.GetPosition();
        param.camera_position_thickness      = float4(
            scene_camera_position.x,
            scene_camera_position.y,
            scene_camera_position.z,
            Max(gizmo_config.line_thickness, 0.001f)
        );

        const uint vertex_count =
            (gizmo_config.show_csm_split_frustums ? k_frustum_vertex_count : 0u) +
            (gizmo_config.show_csm_bounding_spheres ? k_sphere_vertex_count : 0u);

        context.cmd_list.Gfx(m_pipeline, context.bdls, param)
            .Draw(
                "CSM Gizmo Pass",
                context.textures.tonemapping_output.GetRect2D(),
                Array<SingleDrawParam>{SingleDrawParam{vertex_count, valid_cascade_count, 0, 0, 0}},
                ColorAttachment{
                    context.textures.tonemapping_output.tex,
                    EAttachmentAction::AC_LOAD_STORE,
                    float4(0, 0, 0, 0)
                }
            );

    }

private:
    static constexpr uint k_frustum_edge_count   = 12u;
    static constexpr uint k_sphere_segment_count = 32u;
    static constexpr uint k_sphere_circle_count  = 3u;
    static constexpr uint k_vertices_per_edge    = 6u;
    static constexpr uint k_frustum_vertex_count = k_frustum_edge_count * k_vertices_per_edge;
    static constexpr uint k_sphere_vertex_count =
        k_sphere_circle_count * k_sphere_segment_count * k_vertices_per_edge;

    static CsmGizmoCascadeData BuildCascadeData(
        const Camera& camera,
        float         near_ratio,
        float         far_ratio,
        uint          cascade_index
    ) {
        CsmGizmoCascadeData data{};
        data.color = GetCsmGizmoCascadeColor(cascade_index);

        const auto corners = camera.GetFrustumCorners(near_ratio, far_ratio);
        float3     center  = float3(0.0f, 0.0f, 0.0f);
        for (uint corner_index = 0u; corner_index < 8u; ++corner_index) {
            const float3 corner = corners[corner_index];
            data.frustum_corners[corner_index] = float4(corner.x, corner.y, corner.z, 1.0f);
            center += corner;
        }
        center *= 1.0f / 8.0f;

        float radius = 0.0f;
        for (uint corner_index = 0u; corner_index < 8u; ++corner_index) {
            const float3 corner = corners[corner_index];
            radius = Max(radius, Lengthf(corner - center));
        }

        data.sphere_center_radius = float4(center.x, center.y, center.z, radius);
        return data;
    }

    CsmGizmoPipeline m_pipeline;
    BufferWithHandle m_cascade_data_buffer;
};

} // namespace Moer::Render::Raster
