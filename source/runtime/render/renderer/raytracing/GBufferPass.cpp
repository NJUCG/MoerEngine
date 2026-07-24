#include "GBufferPass.h"

// 构建当前帧和上一帧的 GBuffer 表面，并打包降噪器属性。

#include "RTResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

#include <cmath>
#include <cstring>

namespace Moer::Render::Raytracing {

GBufferPass::GBufferPass(RenderDevice& device, ShaderManager& manager, BindlessArrayRef bindless_array) :
    bindless_array(std::move(bindless_array)) {

    gbuffer_constants = device.CreateBuffer<Moer::byte>(
        "Raytracing::gbuffer_constants", sizeof(GBufferConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
    RTGBufferMacros gbuffer_macros{};
    gbuffer_macros.SetMutation<RaytracingGBufferPipeline::PRINT_TEST>(true);
    gbuffer_pipeline = manager.Compute<RaytracingGBufferPipeline>(
        "pipelines/raytracing/passes/GBufferRT.hlsl", gbuffer_macros
    );

    constexpr int with_nrd = WITH_NRD;
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    PostProcessGBufferPipeline::MutationSet mutation_set;
    mutation_set.SetMutation<PostProcessGBufferPipeline::WITH_NRD>(with_nrd);
#pragma pop_macro("WITH_NRD")

    gbuffer_postprocess_pipeline = manager.Compute<PostProcessGBufferPipeline>(
        "pipelines/raytracing/passes/PostProcessGBuffer.hlsl", mutation_set
    );
}

GBufferPass::PreparedCommand GBufferPass::Prepare(const RTContext& rt_ctx) const {
    PreparedCommand                  command{};
    const RaytracingBindlessHandles& bindless_handles = rt_ctx.GetBindlessHandles();

    // 场景间接索引表。
    command.params.instance_buf_hdl  = bindless_handles.instance_buf_hdl;
    command.params.primitive_buf_hdl = bindless_handles.primitive_buf_hdl;
    command.params.material_buf_hdl  = bindless_handles.material_buf_hdl;

    // 共享几何数据大缓冲区。
    command.params.position_buf_hdl       = bindless_handles.position_buf_hdl;
    command.params.packed_normal_buf_hdl  = bindless_handles.packed_normal_buf_hdl;
    command.params.packed_tangent_buf_hdl = bindless_handles.packed_tangent_buf_hdl;
    command.params.texcoord0_buf_hdl      = bindless_handles.texcoord0_buf_hdl;
    command.params.index_buf_hdl          = bindless_handles.index_buf_hdl;

    // Raytracing 专用的 mesh 级 BLAS 间接索引。
    command.params.rt_instance_buf_hdl        = bindless_handles.rt_instance_buf_hdl;
    command.params.rt_primitive_table_buf_hdl = bindless_handles.rt_primitive_table_buf_hdl;

    command.constants.main_view = rt_ctx.main_view;
    command.constants.prev_view = rt_ctx.prev_view;
    command.dispatch_groups     = uint3(
        std::ceil(command.constants.main_view.rect.x / 16),
        std::ceil(command.constants.main_view.rect.y / 16),
        1
    );
    return command;
}

GBufferPass::RecordResources GBufferPass::CaptureResources(const RTContext& rt_ctx) const {
    const FrameResources& frame         = rt_ctx.frame_rt;
    const bool            current_frame = rt_ctx.b_current_frame;
    return RecordResources{
        .view_depth = current_frame ? frame.view_depth : frame.prev_view_depth,
        .diffuse_albedo =
            current_frame ? frame.diffuse_albedo : frame.prev_diffuse_albedo,
        .specular_roughness =
            current_frame ? frame.specular_roughness : frame.prev_specular_roughness,
        .normal           = current_frame ? frame.normal : frame.prev_normal,
        .emission         = frame.emission,
        .motion           = frame.motion,
        .clip_depth       = frame.clip_depth,
        .normal_roughness = frame.normal_roughness,
        .tlas             = rt_ctx.rt_scene ? rt_ctx.rt_scene->GetTlas() : RaytracingTlasRef{},
        .bindless_array   = bindless_array
    };
}

void GBufferPass::RecordConstantsUpload(
    CommandList&           cmd_list,
    const PreparedCommand& command
) {
    Array<byte> upload_data(sizeof(GBufferConstants));
    std::memcpy(upload_data.data(), &command.constants, sizeof(GBufferConstants));
    cmd_list.CopyFrom(std::move(upload_data), gbuffer_constants->GetView());
}

void GBufferPass::RecordTraceGBuffer(
    CommandList&           cmd_list,
    const PreparedCommand& command,
    const RecordResources& resources
) {
    cmd_list
        .Compute(
            gbuffer_pipeline,
            command.params,
            gbuffer_constants,
            resources.view_depth,
            resources.diffuse_albedo,
            resources.specular_roughness,
            resources.normal,
            resources.emission,
            resources.motion,
            resources.clip_depth,
            resources.tlas,
            resources.bindless_array
        )
        .Dispatch(command.dispatch_groups, "RaytracingGbuffer");
}

void GBufferPass::RecordPostProcessGBuffer(
    CommandList&           cmd_list,
    const PreparedCommand& command,
    const RecordResources& resources
) {
    cmd_list
        .Compute(
            gbuffer_postprocess_pipeline,
            resources.specular_roughness,
            resources.normal_roughness,
            resources.normal,
            resources.view_depth
        )
        .Dispatch(command.dispatch_groups, "PostProcessGBuffer");
}

void GBufferPass::Process(CommandList& cmd_list, RTContext& rt_ctx) {
    const PreparedCommand command   = Prepare(rt_ctx);
    const RecordResources resources = CaptureResources(rt_ctx);
    cmd_list.PushScopeWithTimeScope("GBufferPass");
    RecordConstantsUpload(cmd_list, command);
    RecordTraceGBuffer(cmd_list, command, resources);
    RecordPostProcessGBuffer(cmd_list, command, resources);
    cmd_list.PopScopeWithTimeScope();
}

void GBufferPass::RecordLegacyTailBridge(
    CommandList&     cmd_list,
    const RTContext& rt_ctx
) const {
    // Active RDG sources own explicit GENERAL-layout exports, while the
    // remaining legacy tail starts a fresh backend tracker. Seed every
    // bindless GBuffer history read explicitly so Lighting cannot observe the
    // preferred GENERAL layout through a sampled-image descriptor.
    const FrameResources& frame = rt_ctx.frame_rt;
    Array<ReadTexture> sampled_inputs{
        {frame.view_depth->GetView(), ETextureState::SAMPLE},
        {frame.diffuse_albedo->GetView(), ETextureState::SAMPLE},
        {frame.specular_roughness->GetView(), ETextureState::SAMPLE},
        {frame.normal->GetView(), ETextureState::SAMPLE},
        {frame.prev_view_depth->GetView(), ETextureState::SAMPLE},
        {frame.prev_diffuse_albedo->GetView(), ETextureState::SAMPLE},
        {frame.prev_specular_roughness->GetView(), ETextureState::SAMPLE},
        {frame.prev_normal->GetView(), ETextureState::SAMPLE},
        {frame.emission->GetView(), ETextureState::SAMPLE},
        {frame.motion->GetView(), ETextureState::SAMPLE},
    };
#if WITH_NRD
    sampled_inputs.emplace_back(
        ReadTexture{
            frame.normal_roughness->GetView(),
            ETextureState::SAMPLE
        }
    );
#endif
    cmd_list.TextureBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Compute,
        std::move(sampled_inputs)
    );
}

bool GBufferPass::AddPasses(
    RenderGraph&                 graph,
    const RTGraphFrameResources& graph_resources,
    const RTContext&             rt_ctx,
    bool                         tlas_built_this_frame,
    bool                         normal_roughness_readable
) {
    const PreparedCommand command   = Prepare(rt_ctx);
    const RecordResources resources = CaptureResources(rt_ctx);
    if (!resources.tlas || resources.tlas->GetUnderlyingBuffer() == nullptr) {
        return false;
    }

    const BufferRef tlas_buffer(resources.tlas->GetUnderlyingBuffer());
    const auto constants =
        ImportRTGraphBuffer(graph, "RT.GBuffer.constants", gbuffer_constants);
    const auto tlas =
        ImportRTGraphBuffer(graph, "RT.GBuffer.tlas", tlas_buffer);

    const auto initial_texture =
        [&](RenderGraph::TextureHandle texture,
            RenderGraph::TextureState  texture_state,
            RenderGraph::AccessMode    access) {
            graph.SetInitialState(
                texture,
                texture_state,
                RenderGraph::QueueRole::Graphics,
                access
            );
        };
    initial_texture(
        graph_resources.current_view_depth,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::AccessMode::Read
    );
    initial_texture(
        graph_resources.current_diffuse_albedo,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::AccessMode::Read
    );
    initial_texture(
        graph_resources.current_specular_roughness,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::AccessMode::Read
    );
    initial_texture(
        graph_resources.current_normal,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::AccessMode::Read
    );
    initial_texture(
        graph_resources.emission,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::AccessMode::Read
    );
    initial_texture(
        graph_resources.motion,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::AccessMode::Read
    );
    initial_texture(
        graph_resources.clip_depth,
        RenderGraph::TextureState::UnorderedAccess,
        RenderGraph::AccessMode::Write
    );
#if WITH_NRD
    // The mixed graph/legacy frame tail restores UAV-capable textures to the
    // Vulkan backend's preferred GENERAL layout. Preserve whether the last
    // meaningful access was an NRD read or a GBuffer write.
    initial_texture(
        graph_resources.normal_roughness,
        normal_roughness_readable ?
            RenderGraph::TextureState::ShaderResource :
            RenderGraph::TextureState::UnorderedAccess,
        normal_roughness_readable ?
            RenderGraph::AccessMode::Read :
            RenderGraph::AccessMode::Write
    );
#endif
    graph.SetInitialState(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.SetInitialState(
        tlas,
        tlas_built_this_frame ?
            RenderGraph::BufferState::AccelerationStructureWrite :
            RenderGraph::BufferState::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        tlas_built_this_frame ?
            RenderGraph::AccessMode::Write :
            RenderGraph::AccessMode::Read
    );

    graph.AddRecordPass(
        "RT.GBuffer.UploadConstants",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(constants, RenderGraph::BufferState::TransferDestination);
        },
        [this, command](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT GBuffer Constants Upload",
                GpuMarkerPalette::Transfer()
            );
            RecordConstantsUpload(cmd_list, command);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.AddRecordPass(
        "RT.GBuffer.Trace",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Compute
                   )
                .Read(constants, RenderGraph::BufferState::ShaderResource)
                .Read(tlas, RenderGraph::BufferState::AccelerationStructureRead)
                .Write(
                    graph_resources.current_view_depth,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(
                    graph_resources.current_diffuse_albedo,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(
                    graph_resources.current_specular_roughness,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(
                    graph_resources.current_normal,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(
                    graph_resources.emission,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(
                    graph_resources.motion,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(
                    graph_resources.clip_depth,
                    RenderGraph::TextureState::UnorderedAccess
                );
        },
        [this, command, resources](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "GBufferPass",
                GpuMarkerPalette::Pass(),
                EGpuMarkerMode::Timestamp
            );
            RecordTraceGBuffer(cmd_list, command, resources);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.AddRecordPass(
        "RT.GBuffer.PostProcess",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                RenderGraph::QueueRole::Graphics,
                RenderGraph::PipelineType::Compute
            );
            builder
                .ReadWrite(
                    graph_resources.current_specular_roughness,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Read(
                    graph_resources.current_normal,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.current_view_depth,
                    RenderGraph::TextureState::Sampled
                );
#if WITH_NRD
            builder.Write(
                graph_resources.normal_roughness,
                RenderGraph::TextureState::UnorderedAccess
            );
#endif
        },
        [this, command, resources](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT GBuffer PostProcess",
                GpuMarkerPalette::Pass()
            );
            RecordPostProcessGBuffer(cmd_list, command, resources);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    const auto export_texture =
        [&](RenderGraph::TextureHandle texture,
            RenderGraph::TextureState  texture_state,
            RenderGraph::AccessMode    access) {
            graph.Export(
                texture,
                texture_state,
                RenderGraph::QueueRole::Graphics,
                access
            );
        };
    for (const auto texture : {
             graph_resources.current_view_depth,
             graph_resources.current_diffuse_albedo,
             graph_resources.current_specular_roughness,
             graph_resources.current_normal,
             graph_resources.emission,
             graph_resources.motion,
         }) {
        export_texture(
            texture,
            RenderGraph::TextureState::ShaderResource,
            RenderGraph::AccessMode::Read
        );
    }
    export_texture(
        graph_resources.clip_depth,
        RenderGraph::TextureState::UnorderedAccess,
        RenderGraph::AccessMode::Write
    );
#if WITH_NRD
    export_texture(
        graph_resources.normal_roughness,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::AccessMode::Read
    );
#endif
    graph.Export(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        tlas,
        RenderGraph::BufferState::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    return true;
}

} // namespace Moer::Render::Raytracing
