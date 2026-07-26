#include "LightingPass.h"

// 协调 ReSTIR DI 各阶段调度，并共享同一组不可变参数。

#include "Configs.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

LightingPass::LightingPass(ShaderManager& _manager, BindlessArrayRef bindless_array) :
    bindless_array(std::move(bindless_array)) {

    presample_light_pipeline      = std::move(_manager.Compute<PresampleLightPipeline>(
        "pipelines/raytracing/lighting/precompute/PresampleLight.hlsl"
    ));
    presample_env_map_pipeline    = std::move(_manager.Compute<PresampleEnvMapPipeline>(
        "pipelines/raytracing/lighting/precompute/PresampleEnvmap.hlsl"
    ));
    presample_light_grid_pipeline = std::move(_manager.Compute<PresampleLightGridPipeline>(
        "pipelines/raytracing/lighting/precompute/PresampleLightGrid.hlsl"
    ));

    generate_initial_sample_pipeline = std::move(_manager.Compute<GenerateInitialSamplePipeline>(
        "pipelines/raytracing/restir_di/GenerateInitialSamples.hlsl"
    ));
    temporal_resample_pipeline       = std::move(
        _manager.Compute<TemporalResamplePipeline>("pipelines/raytracing/restir_di/TemporalResampling.hlsl")
    );
    spatial_resample_pipeline = std::move(
        _manager.Compute<SpatialResamplePipeline>("pipelines/raytracing/restir_di/SpatialResampling.hlsl")
    );

    int with_nrd = WITH_NRD;
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    DIShadeSamplePipeline::MutationSet mutation_set;
    mutation_set.SetMutation<DIShadeSamplePipeline::WITH_NRD>(with_nrd);
#pragma pop_macro("WITH_NRD")

    di_shade_sample_pipeline = std::move(
        _manager.Compute<DIShadeSamplePipeline>("pipelines/raytracing/restir_di/Shading.hlsl", mutation_set)
    );

    auto& device    = RenderDevice::Get();
    resample_params = device.CreateBuffer<byte>(
        "Raytracing::resample_params", sizeof(ResampleConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

LightingPass::PreparedCommand
LightingPass::Prepare(const RTContext& rt_ctx, const DI::LightBufferParams& light_buffer_params) const {
    const DI::ReSTIRDIRuntimeConfig& restir_di_runtime_config = rt_ctx.is_ctx.GetReSTIRDIRuntimeConfig();
    const ImportanceSamplingContext& is_ctx                   = rt_ctx.is_ctx;

    PreparedCommand command{};
    auto&           constants  = command.constants;
    constants.frame_idx        = is_ctx.GetFrameIdx();
    constants.main_view        = rt_ctx.main_view;
    constants.prev_view        = rt_ctx.prev_view;
    constants.enable_prev_tlas = true;
    constants.local_light_pdf_size =
        rt_ctx.local_light_pdf_tex ? rt_ctx.local_light_pdf_tex->GetExtent().xy : uint2(0);
    constants.env_pdf_size     = rt_ctx.env_pdf_tex ? rt_ctx.env_pdf_tex->GetExtent().xy : uint2(0);
    constants.bindless_handles = rt_ctx.GetBindlessHandles();

    constants.di_params                         = restir_di_runtime_config.common_params;
    auto initial_sample_params                  = is_ctx.GetDIInitialSampleParams();
    initial_sample_params.env_map_is            = light_buffer_params.env_light.light_cnt;
    command.sampling_decision.configured_mode   = initial_sample_params.local_light_sample_mode;
    command.sampling_decision.local_light_count = light_buffer_params.local_light_region.light_cnt;
    command.sampling_decision.adaptive_fallback_applied =
        rt_ctx.config.enable_adaptive_local_light_sampling &&
        initial_sample_params.local_light_sample_mode == s_di_local_light_sample_mode_grid &&
        command.sampling_decision.local_light_count < rt_ctx.config.grid_min_local_light_count;
    if (command.sampling_decision.adaptive_fallback_applied) {
        initial_sample_params.local_light_sample_mode = s_di_local_light_sample_mode_power_ris;
    }
    command.sampling_decision.effective_mode = initial_sample_params.local_light_sample_mode;

    constants.restir_di_params.initial_sample_params    = initial_sample_params;
    constants.restir_di_params.temporal_resample_params = is_ctx.GetDITemporalResampleParams();
    constants.restir_di_params.spatial_resample_params  = is_ctx.GetDISpatialResampleParams();
    constants.restir_di_params.reservoir_buffer_params  = restir_di_runtime_config.reservoir_buffer_params;
    constants.restir_di_params.shading_params           = is_ctx.GetDIShadingParams();
    constants.restir_di_params.buffer_indices           = is_ctx.GetReSTIRDIBufferIndices();

    constants.light_buffer_params           = light_buffer_params;
    constants.local_light_ris_buffer_params = is_ctx.GetLocalLightRISBufferParams();
    constants.env_light_ris_buffer_params   = is_ctx.GetEnvLightRISBufferParams();
    constants.grid_params                   = is_ctx.GetGridParams();
    constants.scene_params                  = rt_ctx.scene_params;
    constants.enable_accumulation           = 1;
    constants.discount_native_samples       = 1;
    constants.visualize_cells               = 0;
    constants.denoiser_mode                 = rt_ctx.config.denoiser_mode;
    constants.reblur_diff_hit_dist_params   = rt_ctx.config.reblur_diffuse_hit_dist_params;
    constants.reblur_spec_hit_dist_params   = rt_ctx.config.reblur_specular_hit_dist_params;

    const auto div_ceil = [](uint value, uint divisor) -> uint {
        return (value + divisor - 1) / divisor;
    };
    const bool has_local_candidates = initial_sample_params.num_primary_local_lights > 0;
    const bool uses_grid = initial_sample_params.local_light_sample_mode == s_di_local_light_sample_mode_grid;
    const bool needs_power_ris =
        initial_sample_params.local_light_sample_mode == s_di_local_light_sample_mode_power_ris ||
        (uses_grid && constants.grid_params.common_params.local_light_sampling_fallback_mode ==
                          s_di_local_light_sample_mode_power_ris);
    command.record_presample_light =
        command.sampling_decision.local_light_count != 0 && has_local_candidates && needs_power_ris;
    command.record_presample_env = light_buffer_params.env_light.light_cnt != 0;
    command.record_presample_grid =
        command.sampling_decision.local_light_count != 0 && has_local_candidates && uses_grid;

    command.presample_light_dispatch = uint3(
        div_ceil(is_ctx.GetLocalLightRISBufferParams().tile_size, DI_PRESAMPLE_GRID_SIZE),
        is_ctx.GetLocalLightRISBufferParams().tile_cnt,
        1
    );
    command.presample_env_dispatch = uint3(
        div_ceil(is_ctx.GetEnvLightRISBufferParams().tile_size, DI_PRESAMPLE_GRID_SIZE),
        is_ctx.GetEnvLightRISBufferParams().tile_cnt,
        1
    );
    command.presample_grid_dispatch =
        uint3(div_ceil(is_ctx.GetGridRuntimeConfig().num_light_slot, DI_PRESAMPLE_GRID_SIZE), 1, 1);
    const uint2 lighting_extent = rt_ctx.frame_rt.diffuse_lighting->GetExtent().xy;
    command.screen_dispatch     = uint3(
        div_ceil(lighting_extent.x, DI_SCREEN_TILE_SIZE), div_ceil(lighting_extent.y, DI_SCREEN_TILE_SIZE), 1
    );
    return command;
}

LightingPass::RecordResources LightingPass::CaptureResources(const RTContext& rt_ctx) const {
    const bool        current_frame = rt_ctx.b_current_frame;
    RaytracingTlasRef tlas          = rt_ctx.rt_scene ? rt_ctx.rt_scene->GetTlas() : RaytracingTlasRef{};
    RaytracingTlasRef prev_tlas     = rt_ctx.rt_scene ? rt_ctx.rt_scene->GetPrevTlas() : RaytracingTlasRef{};
    if (!prev_tlas) {
        prev_tlas = tlas;
    }

    return RecordResources{
        .tlas                   = tlas,
        .prev_tlas              = prev_tlas,
        .constants_buf          = resample_params,
        .light_reservoir_buf    = rt_ctx.light_reservoir_buf,
        .ris_buf                = rt_ctx.ris_buf,
        .ris_light_data_buf     = rt_ctx.ris_light_data_buf,
        .neighbor_offset_buf    = rt_ctx.neighbor_offset_buf,
        .light_mapping_buf      = rt_ctx.light_mapping_buf,
        .light_data_buf         = rt_ctx.light_data_buf,
        .primitive_to_light_buf = rt_ctx.primitive_to_light_buf,
        .diffuse_lighting       = rt_ctx.frame_rt.diffuse_lighting,
        .specular_lighting      = rt_ctx.frame_rt.specular_lighting,
        .temporal_sample_pos    = rt_ctx.frame_rt.temporal_sample_pos,
        .gradients              = rt_ctx.frame_rt.gradients,
        .restir_luminance = current_frame ? rt_ctx.frame_rt.restir_luminance : rt_ctx.frame_rt.prev_luminance,
        .prev_diffuse_lighting = rt_ctx.frame_rt.prev_diffuse_lighting,
        .local_light_pdf       = rt_ctx.local_light_pdf_tex,
        .env_pdf               = rt_ctx.env_pdf_tex,
        .env_map               = rt_ctx.env_map,
        .bindless_array        = bindless_array
    };
}

void LightingPass::RecordConstantsUpload(
    CommandList&           cmd_list,
    const PreparedCommand& command,
    const RecordResources& resources
) const {
    Array<byte> upload_data(sizeof(ResampleConstants));
    std::memcpy(upload_data.data(), &command.constants, sizeof(ResampleConstants));
    cmd_list.CopyFrom(std::move(upload_data), resources.constants_buf->GetView());
}

void LightingPass::RecordDispatch(
    CommandList&           cmd_list,
    const PreparedCommand& command,
    const RecordResources& resources,
    ELightingDispatch      dispatch
) {
#define DI_BINDING_ARGS(ctx)                                                                     \
    ctx.tlas, ctx.prev_tlas, ctx.constants_buf, ctx.light_reservoir_buf, ctx.diffuse_lighting,   \
        ctx.specular_lighting, ctx.temporal_sample_pos, ctx.gradients, ctx.restir_luminance,     \
        ctx.prev_diffuse_lighting, ctx.ris_buf, ctx.ris_light_data_buf, ctx.neighbor_offset_buf, \
        ctx.bindless_array

    switch (dispatch) {
        case ELightingDispatch::PresampleLight:
            cmd_list.Compute(presample_light_pipeline, DI_BINDING_ARGS(resources))
                .Dispatch(
                    command.presample_light_dispatch,
                    "PresampleLight",
                    ProfileSection("ReSTIR DI Presample Local")
                );
            break;
        case ELightingDispatch::PresampleEnvMap:
            cmd_list.Compute(presample_env_map_pipeline, DI_BINDING_ARGS(resources))
                .Dispatch(
                    command.presample_env_dispatch,
                    "PresampleEnvMap",
                    ProfileSection("ReSTIR DI Presample Environment")
                );
            break;
        case ELightingDispatch::PresampleLightGrid:
            cmd_list.Compute(presample_light_grid_pipeline, DI_BINDING_ARGS(resources))
                .Dispatch(
                    command.presample_grid_dispatch,
                    "PresampleLightGrid",
                    ProfileSection("ReSTIR DI Presample Grid")
                );
            break;
        case ELightingDispatch::GenerateInitialSample:
            cmd_list.Compute(generate_initial_sample_pipeline, DI_BINDING_ARGS(resources))
                .Dispatch(
                    command.screen_dispatch, "GenerateInitialSample", ProfileSection("ReSTIR DI Initial")
                );
            break;
        case ELightingDispatch::TemporalResample:
            cmd_list.Compute(temporal_resample_pipeline, DI_BINDING_ARGS(resources))
                .Dispatch(command.screen_dispatch, "TemporalResample", ProfileSection("ReSTIR DI Temporal"));
            break;
        case ELightingDispatch::SpatialResample:
            cmd_list.Compute(spatial_resample_pipeline, DI_BINDING_ARGS(resources))
                .Dispatch(command.screen_dispatch, "SpatialResample", ProfileSection("ReSTIR DI Spatial"));
            break;
        case ELightingDispatch::ShadeSample:
            cmd_list.Compute(di_shade_sample_pipeline, DI_BINDING_ARGS(resources))
                .Dispatch(command.screen_dispatch, "ShadeSample", ProfileSection("ReSTIR DI Shade"));
            break;
    }
#undef DI_BINDING_ARGS
}

LightingPass::LocalLightSamplingDecision LightingPass::Process(
    CommandList&                 cmd_list,
    RTContext&                   rt_ctx,
    const DI::LightBufferParams& light_buffer_params
) {
    const PreparedCommand command   = Prepare(rt_ctx, light_buffer_params);
    const RecordResources resources = CaptureResources(rt_ctx);

    cmd_list.PushScopeWithTimeScope("LightingPass");
    RecordConstantsUpload(cmd_list, command, resources);
    if (command.record_presample_light) {
        RecordDispatch(cmd_list, command, resources, ELightingDispatch::PresampleLight);
    }
    if (command.record_presample_env) {
        RecordDispatch(cmd_list, command, resources, ELightingDispatch::PresampleEnvMap);
    }
    if (command.record_presample_grid) {
        RecordDispatch(cmd_list, command, resources, ELightingDispatch::PresampleLightGrid);
    }
    RecordDispatch(cmd_list, command, resources, ELightingDispatch::GenerateInitialSample);
    RecordDispatch(cmd_list, command, resources, ELightingDispatch::TemporalResample);
    RecordDispatch(cmd_list, command, resources, ELightingDispatch::SpatialResample);
    RecordDispatch(cmd_list, command, resources, ELightingDispatch::ShadeSample);
    cmd_list.PopScopeWithTimeScope();
    return command.sampling_decision;
}

bool LightingPass::AddPasses(
    RenderGraph&                 graph,
    const RTGraphFrameResources& graph_resources,
    const RTContext&             rt_ctx,
    const DI::LightBufferParams& light_buffer_params,
    bool                         prepare_lights_in_graph,
    LocalLightSamplingDecision&  sampling_decision
) {
    const PreparedCommand command            = Prepare(rt_ctx, light_buffer_params);
    const RecordResources resources          = CaptureResources(rt_ctx);
    const auto            has_backing_buffer = [](const RaytracingTlasRef& tlas) {
        return tlas && tlas->GetUnderlyingBuffer() != nullptr;
    };
    if (!has_backing_buffer(resources.tlas) || !has_backing_buffer(resources.prev_tlas) ||
        !graph_resources.frame_setup.ready.IsValid() ||
        !graph_resources.frame_setup.current_tlas.IsValid() ||
        !graph_resources.frame_setup.previous_tlas.IsValid() ||
        !resources.constants_buf || !resources.light_reservoir_buf || !resources.ris_buf ||
        !resources.ris_light_data_buf || !resources.neighbor_offset_buf || !resources.light_mapping_buf ||
        !resources.light_data_buf || !resources.primitive_to_light_buf || !resources.diffuse_lighting ||
        !resources.specular_lighting || !resources.temporal_sample_pos || !resources.gradients ||
        !resources.restir_luminance || !resources.prev_diffuse_lighting || !resources.bindless_array ||
        (command.record_presample_light && !resources.local_light_pdf) ||
        (command.record_presample_env && !resources.env_pdf) ||
        (command.constants.scene_params.enable_env_map != 0 && !resources.env_map)) {
        return false;
    }

    const auto      constants = ImportRTGraphBuffer(graph, "RT.Lighting.constants", resources.constants_buf);
    const auto      tlas      = graph_resources.frame_setup.current_tlas;
    const auto      prev_tlas = graph_resources.frame_setup.previous_tlas;
    const auto light_reservoir =
        ImportRTGraphBuffer(graph, "RT.Lighting.light_reservoir", resources.light_reservoir_buf);
    const auto ris = ImportRTGraphBuffer(graph, "RT.Lighting.ris", resources.ris_buf);
    const auto ris_light_data =
        ImportRTGraphBuffer(graph, "RT.Lighting.ris_light_data", resources.ris_light_data_buf);
    const auto neighbor_offset =
        ImportRTGraphBuffer(graph, "RT.Lighting.neighbor_offset", resources.neighbor_offset_buf);
    const auto light_mapping =
        ImportRTGraphBuffer(graph, "RT.Lighting.light_mapping", resources.light_mapping_buf);
    const auto light_data = ImportRTGraphBuffer(graph, "RT.Lighting.light_data", resources.light_data_buf);
    const auto primitive_to_light =
        ImportRTGraphBuffer(graph, "RT.Lighting.primitive_to_light", resources.primitive_to_light_buf);

    RenderGraph::TextureHandle local_light_pdf{};
    if (resources.local_light_pdf) {
        local_light_pdf =
            ImportRTGraphTexture(graph, "RT.Lighting.local_light_pdf", resources.local_light_pdf);
    }
    RenderGraph::TextureHandle env_pdf{};
    if (resources.env_pdf) {
        env_pdf = ImportRTGraphTexture(graph, "RT.Lighting.env_pdf", resources.env_pdf);
    }
    const RenderGraph::TextureHandle env_map = graph_resources.env_map;

    graph.SetInitialState(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    for (const auto texture : {
             graph_resources.previous_view_depth,
             graph_resources.previous_diffuse_albedo,
             graph_resources.previous_specular_roughness,
             graph_resources.previous_normal,
             graph_resources.diffuse_lighting,
             graph_resources.specular_lighting,
         }) {
        graph.SetInitialState(
            texture,
            RenderGraph::TextureState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }
    for (const auto buffer : {
             light_reservoir,
             ris,
             ris_light_data,
         }) {
        graph.SetInitialState(
            buffer,
            RenderGraph::BufferState::UnorderedAccess,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::ReadWrite
        );
    }
    graph.SetInitialState(
        neighbor_offset,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    if (!prepare_lights_in_graph) {
        for (const auto buffer : {
                 light_mapping,
                 light_data,
                 primitive_to_light,
             }) {
            graph.SetInitialState(
                buffer,
                RenderGraph::BufferState::UnorderedAccess,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Write
            );
        }
        if (local_light_pdf.IsValid()) {
            graph.SetInitialState(
                local_light_pdf,
                RenderGraph::TextureState::UnorderedAccess,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Write
            );
        }
    }
    if (env_pdf.IsValid()) {
        graph.SetInitialState(
            env_pdf,
            RenderGraph::TextureState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }
    if (env_map.IsValid()) {
        graph.SetInitialState(
            env_map,
            RenderGraph::TextureState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    graph.AddRecordPass(
        "RT.Lighting.UploadConstants",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(constants, RenderGraph::BufferState::TransferDestination);
        },
        [this, command, resources](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list, "Pass: RT Lighting Constants Upload", GpuMarkerPalette::Transfer()
            );
            RecordConstantsUpload(cmd_list, command, resources);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    const auto add_dispatch = [&](std::string_view name, ELightingDispatch dispatch) {
        graph.AddRecordPass(
            name,
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                    .Read(constants, RenderGraph::BufferState::ShaderResource)
                    .Read(tlas, RenderGraph::BufferState::AccelerationStructureRead)
                    .Read(prev_tlas, RenderGraph::BufferState::AccelerationStructureRead)
                    .Read(graph_resources.frame_setup.ready)
                    .ReadWrite(light_reservoir, RenderGraph::BufferState::UnorderedAccess)
                    .ReadWrite(ris, RenderGraph::BufferState::UnorderedAccess)
                    .ReadWrite(ris_light_data, RenderGraph::BufferState::UnorderedAccess)
                    .Read(neighbor_offset, RenderGraph::BufferState::ShaderResource)
                    .Read(light_mapping, RenderGraph::BufferState::ShaderResource)
                    .Read(light_data, RenderGraph::BufferState::ShaderResource)
                    .Read(primitive_to_light, RenderGraph::BufferState::ShaderResource)
                    .ReadWrite(graph_resources.diffuse_lighting, RenderGraph::TextureState::UnorderedAccess)
                    .ReadWrite(graph_resources.specular_lighting, RenderGraph::TextureState::UnorderedAccess)
                    .Read(graph_resources.current_view_depth, RenderGraph::TextureState::Sampled)
                    .Read(graph_resources.current_diffuse_albedo, RenderGraph::TextureState::Sampled)
                    .Read(graph_resources.current_specular_roughness, RenderGraph::TextureState::Sampled)
                    .Read(graph_resources.current_normal, RenderGraph::TextureState::Sampled)
                    .Read(graph_resources.previous_view_depth, RenderGraph::TextureState::Sampled)
                    .Read(graph_resources.previous_diffuse_albedo, RenderGraph::TextureState::Sampled)
                    .Read(graph_resources.previous_specular_roughness, RenderGraph::TextureState::Sampled)
                    .Read(graph_resources.previous_normal, RenderGraph::TextureState::Sampled)
                    .Read(graph_resources.motion, RenderGraph::TextureState::Sampled);
                if (local_light_pdf.IsValid()) {
                    builder.Read(local_light_pdf, RenderGraph::TextureState::Sampled);
                }
                if (env_pdf.IsValid()) {
                    builder.Read(env_pdf, RenderGraph::TextureState::Sampled);
                }
                if (env_map.IsValid()) {
                    builder.Read(env_map, RenderGraph::TextureState::Sampled);
                }
            },
            [this, command, resources, dispatch](CommandList& cmd_list) {
                RecordDispatch(cmd_list, command, resources, dispatch);
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    };

    if (command.record_presample_light) {
        add_dispatch("RT.Lighting.PresampleLight", ELightingDispatch::PresampleLight);
    }
    if (command.record_presample_env) {
        add_dispatch("RT.Lighting.PresampleEnvMap", ELightingDispatch::PresampleEnvMap);
    }
    if (command.record_presample_grid) {
        add_dispatch("RT.Lighting.PresampleLightGrid", ELightingDispatch::PresampleLightGrid);
    }
    add_dispatch("RT.Lighting.GenerateInitialSample", ELightingDispatch::GenerateInitialSample);
    add_dispatch("RT.Lighting.TemporalResample", ELightingDispatch::TemporalResample);
    add_dispatch("RT.Lighting.SpatialResample", ELightingDispatch::SpatialResample);
    add_dispatch("RT.Lighting.ShadeSample", ELightingDispatch::ShadeSample);

    const auto export_texture = [&](RenderGraph::TextureHandle texture,
                                    RenderGraph::TextureState  state,
                                    RenderGraph::AccessMode    access) {
        graph.Export(texture, state, RenderGraph::QueueRole::Graphics, access);
    };
    for (const auto texture : {
             graph_resources.previous_view_depth,
             graph_resources.previous_diffuse_albedo,
             graph_resources.previous_specular_roughness,
             graph_resources.previous_normal,
             graph_resources.diffuse_lighting,
             graph_resources.specular_lighting,
         }) {
        export_texture(texture, RenderGraph::TextureState::ShaderResource, RenderGraph::AccessMode::Read);
    }
    if (local_light_pdf.IsValid()) {
        export_texture(
            local_light_pdf, RenderGraph::TextureState::ShaderResource, RenderGraph::AccessMode::Read
        );
    }
    if (env_pdf.IsValid()) {
        export_texture(env_pdf, RenderGraph::TextureState::ShaderResource, RenderGraph::AccessMode::Read);
    }
    if (env_map.IsValid()) {
        export_texture(env_map, RenderGraph::TextureState::ShaderResource, RenderGraph::AccessMode::Read);
    }

    graph.Export(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    for (const auto buffer : {
             light_reservoir,
             ris,
             ris_light_data,
         }) {
        graph.Export(
            buffer,
            RenderGraph::BufferState::UnorderedAccess,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::ReadWrite
        );
    }
    for (const auto buffer : {
             neighbor_offset,
             light_mapping,
             light_data,
             primitive_to_light,
         }) {
        graph.Export(
            buffer,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    sampling_decision = command.sampling_decision;
    return true;
}

} // namespace Moer::Render::Raytracing
