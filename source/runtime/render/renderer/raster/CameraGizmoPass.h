#pragma once

#include "RasterResource.h"
#include "rendergraph/RenderGraph.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

namespace Moer::Render::Raster {

class CameraGizmoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(CameraGizmoPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(CameraGizmoParam, param);
    DEFINE_SHADER_ARGS(param);
};

class CameraGizmoPass {
public:
    struct GraphResources {
        RenderGraph::TextureHandle tonemapping_output{};
    };

    struct RecordParameters {
        CameraGizmoParam  shader{};
        TextureWithHandle output{};
    };

    CameraGizmoPass(RasterContext& context) {
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
                         .Vertex("pipelines/raster/deferred/lighting/CameraGizmo.hlsl", "CameraGizmoVS")
                         .Pixel("pipelines/raster/deferred/lighting/CameraGizmo.hlsl", "CameraGizmoPS")
                         .Build<CameraGizmoPipeline>(std::move(pso_info));
    }

    [[nodiscard]] RecordParameters Prepare(
        const RasterContext& context,
        const Camera&        scene_camera,
        const Camera&        main_camera
    ) const {
        RecordParameters parameters{};
        auto& param = parameters.shader;
        param.world2clip = Transpose(scene_camera.GetViewProjectionMatrix());

        const float near_depth = Max(main_camera.GetNearClip(), 0.001f);
        const float far_depth =
            Min(main_camera.GetFarClip(), Max(near_depth * 10.0f, main_camera.GetFarClip() * 0.02f));
        const Vector3f position = main_camera.GetPosition();
        const Vector3f right    = main_camera.GetRight();
        const Vector3f up       = main_camera.GetUp();
        const Vector3f front    = main_camera.GetFront();

        param.camera_position_near =
            float4(position.x, position.y, position.z, near_depth);
        param.camera_right_tan_half_fov =
            float4(right.x, right.y, right.z, main_camera.GetTanHalfFov());
        param.camera_up_aspect =
            float4(up.x, up.y, up.z, main_camera.GetAspectRatio());
        param.camera_front_far =
            float4(front.x, front.y, front.z, Max(far_depth, near_depth * 2.0f));
        parameters.output = context.textures.tonemapping_output;
        return parameters;
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list.Gfx(m_pipeline, parameters.shader)
            .Draw(
                "Main Camera Gizmo Pass",
                parameters.output.GetRect2D(),
                Array<SingleDrawParam>{SingleDrawParam{114, 1, 0, 0, 0}},
                ColorAttachment{
                    parameters.output.tex,
                    EAttachmentAction::AC_LOAD_STORE,
                    float4(0, 0, 0, 0)
                }
            );
    }

    RenderGraph::PreparedPassHandle AddToGraph(
        RenderGraph& graph,
        const RasterContext& context,
        const Camera& scene_camera,
        const Camera& main_camera,
        GraphResources resources
    );

    void Process(RasterContext& context, const Camera& scene_camera, const Camera& main_camera) {
        const RecordParameters parameters = Prepare(context, scene_camera, main_camera);
        Record(context.cmd_list, parameters);
    }

private:
    CameraGizmoPipeline m_pipeline;
};

} // namespace Moer::Render::Raster
