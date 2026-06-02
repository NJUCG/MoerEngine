#include "LightingPass.h"

#include "Configs.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "trace/Trace.h"

namespace Moer::Render::Raytracing {

enum class LightingPass::ELightingDispatch : uint8_t {
    PresampleLight,
    PresampleEnvMap,
    PresampleLightGrid,
    GenerateInitialSample,
    TemporalResample,
    SpatialResample,
    ShadeSample
};

namespace {

struct RGLightingUploadParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(constants);
};

struct RGLightingDispatchParams {
    DEFINE_RG_BUFFER_ACCESS(tlas_buffer, EBufferState::ACCELERATION_STRUCTURE_READ);
    DEFINE_RG_BUFFER_ACCESS(prev_tlas_buffer, EBufferState::ACCELERATION_STRUCTURE_READ);
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(light_reservoir, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(temporal_sample_pos, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(gradients, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(restir_luminance, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(prev_diffuse_lighting, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(ris, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(ris_light_data, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(neighbor_offset, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(light_data, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(light_mapping, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(primitive_to_light, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(local_light_pdf, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(env_pdf, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(current_view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(current_diffuse_albedo, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(current_specular_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(current_normal, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(previous_view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(previous_diffuse_albedo, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(previous_specular_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(previous_normal, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_PARAMETER_ACCESS(
        tlas_buffer,
        prev_tlas_buffer,
        constants,
        light_reservoir,
        diffuse_lighting,
        specular_lighting,
        temporal_sample_pos,
        gradients,
        restir_luminance,
        prev_diffuse_lighting,
        ris,
        ris_light_data,
        neighbor_offset,
        light_data,
        light_mapping,
        primitive_to_light,
        local_light_pdf,
        env_pdf,
        current_view_depth,
        current_diffuse_albedo,
        current_specular_roughness,
        current_normal,
        previous_view_depth,
        previous_diffuse_albedo,
        previous_specular_roughness,
        previous_normal,
        motion
    );
};

void FillLightingDispatchParams(
    RenderGraph&                graph,
    RGLightingDispatchParams&  params,
    RGBuffer*                  constants,
    RGBuffer*                  tlas_buffer,
    RGBuffer*                  prev_tlas_buffer,
    const RTGraphFrameResources& rg,
    const LightingPass::RecordResources& resources
) {
    (void)graph;
    (void)resources;
    params.tlas_buffer                 = RGBufferView{.buffer = tlas_buffer};
    params.prev_tlas_buffer            = RGBufferView{.buffer = prev_tlas_buffer};
    params.constants                   = RGBufferView{.buffer = constants};
    params.light_reservoir             = RGBufferView{.buffer = rg.light_reservoir_buf};
    params.diffuse_lighting            = RTWholeTextureView(rg.diffuse_lighting);
    params.specular_lighting           = RTWholeTextureView(rg.specular_lighting);
    params.temporal_sample_pos         = RTWholeTextureView(rg.temporal_sample_pos);
    params.gradients                   = RTWholeTextureView(rg.gradients);
    params.restir_luminance            = RTWholeTextureView(rg.current_restir_luminance);
    params.prev_diffuse_lighting       = RTWholeTextureView(rg.prev_diffuse_lighting);
    params.ris                         = RGBufferView{.buffer = rg.ris_buf};
    params.ris_light_data              = RGBufferView{.buffer = rg.ris_light_data_buf};
    params.neighbor_offset             = RGBufferView{.buffer = rg.neighbor_offset_buf};
    params.light_data                  = RGBufferView{.buffer = rg.light_data_buf};
    params.light_mapping               = RGBufferView{.buffer = rg.light_mapping_buf};
    params.primitive_to_light          = RGBufferView{.buffer = rg.primitive_to_light_buf};
    params.local_light_pdf             = RTWholeTextureView(rg.local_light_pdf_tex);
    params.env_pdf                     = RTWholeTextureView(rg.env_pdf_tex);
    params.current_view_depth          = RTWholeTextureView(rg.current_view_depth);
    params.current_diffuse_albedo      = RTWholeTextureView(rg.current_diffuse_albedo);
    params.current_specular_roughness  = RTWholeTextureView(rg.current_specular_roughness);
    params.current_normal              = RTWholeTextureView(rg.current_normal);
    params.previous_view_depth         = RTWholeTextureView(rg.previous_view_depth);
    params.previous_diffuse_albedo     = RTWholeTextureView(rg.previous_diffuse_albedo);
    params.previous_specular_roughness = RTWholeTextureView(rg.previous_specular_roughness);
    params.previous_normal             = RTWholeTextureView(rg.previous_normal);
    params.motion                      = RTWholeTextureView(rg.motion);
}

} // namespace

LightingPass::LightingPass(ShaderManager& _manager) {

    presample_light_pipeline =
        std::move(_manager.Compute<PresampleLightPipeline>("pipelines/raytracing/lighting/precompute/PresampleLight.hlsl"));
    presample_env_map_pipeline =
        std::move(_manager.Compute<PresampleEnvMapPipeline>("pipelines/raytracing/lighting/precompute/PresampleEnvmap.hlsl"));
    presample_light_grid_pipeline =
        std::move(_manager.Compute<PresampleLightGridPipeline>("pipelines/raytracing/lighting/precompute/PresampleLightGrid.hlsl"));

    generate_initial_sample_pipeline =
        std::move(_manager.Compute<GenerateInitialSamplePipeline>("pipelines/raytracing/restir_di/GenerateInitialSamples.hlsl")
        );
    temporal_resmaple_pipeline =
        std::move(_manager.Compute<TemporalResmaplePipeline>("pipelines/raytracing/restir_di/TemporalResampling.hlsl"));
    spatial_resample_pipeline =
        std::move(_manager.Compute<SpatialResamplePipeline>("pipelines/raytracing/restir_di/SpatialResampling.hlsl"));

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
        MOER_TEXT("Raytracing::resample_params"), sizeof(ResampleConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
}

LightingPass::PreparedCommand LightingPass::Prepare(const RTContext& _rt_ctx) const {
    TRACE_SCOPE_CAT("Raytracing.Lighting.Prepare", "Frame");
    //process
    const DI::ReSTIRDIRuntimeConfig& restir_di_runtime_config = _rt_ctx.is_ctx.GetReSTIRDIRuntimeConfig();

    const ImportanceSamplingContext& is_ctx = _rt_ctx.is_ctx;

    PreparedCommand command{};
    {
        TRACE_SCOPE_CAT("Raytracing.Lighting.Prepare.BaseConstants", "Frame");
        command.constants.frame_idx        = _rt_ctx.is_ctx.GetFrameIdx();
        command.constants.main_view        = _rt_ctx.main_view;
        command.constants.prev_view        = _rt_ctx.prev_view;
        command.constants.enable_prev_tlas = true;
        command.constants.di_params        = is_ctx.GetReSTIRDIRuntimeConfig().common_params;
        command.constants.local_light_pdf_size =
            _rt_ctx.local_light_pdf_tex ? _rt_ctx.local_light_pdf_tex->GetExtent().xy : uint2(0);
        command.constants.env_pdf_size     = _rt_ctx.env_pdf_tex ? _rt_ctx.env_pdf_tex->GetExtent().xy : uint2(0);
        command.constants.bindless_handles = _rt_ctx.GetBindlessHandles();
    }

    // static constexpr uint offset_of_prev_view           = offsetof(ResampleConstants, prev_view);
    // static constexpr uint offset_of_main_view           = offsetof(ViewParam, near_far);
    {
        TRACE_SCOPE_CAT("Raytracing.Lighting.Prepare.ReSTIR", "Frame");
        command.constants.di_params                                 = restir_di_runtime_config.common_params;
        command.constants.restir_di_params.initial_sample_params    = is_ctx.GetDIInitialSampleParams();
        command.constants.restir_di_params.temporal_resample_params = is_ctx.GetDITemporalResampleParams();
        command.constants.restir_di_params.spatial_resample_params  = is_ctx.GetDISpatialResampleParams();
        command.constants.restir_di_params.reservoir_buffer_params  = restir_di_runtime_config.reservoir_buffer_params;
        command.constants.restir_di_params.shading_params           = is_ctx.GetDIShadingParams();
        command.constants.restir_di_params.buffer_indices           = is_ctx.GetReSTIRDIBufferIndices();
    }

    {
        TRACE_SCOPE_CAT("Raytracing.Lighting.Prepare.SceneLighting", "Frame");
        command.constants.light_buffer_params           = is_ctx.GetLightBufferParams();
        command.constants.local_light_ris_buffer_params = is_ctx.GetLocalLightRISBufferParams();
        command.constants.env_light_ris_buffer_params   = is_ctx.GetEnvLightRISBufferParams();

        command.constants.grid_params             = is_ctx.GetGridParams();
        command.constants.scene_params            = _rt_ctx.scene_params;
        command.constants.enable_accumulation     = 1;
        command.constants.discount_native_samples = 1;
        command.constants.visualize_cells         = 0;

        command.constants.denoiser_mode               = _rt_ctx.config.denoiser_mode;
        command.constants.reblur_diff_hit_dist_params = _rt_ctx.config.reblur_diffuse_hit_dist_params;
        command.constants.reblur_spec_hit_dist_params = _rt_ctx.config.reblur_specular_hit_dist_params;
    }

    auto div_ceil = [](uint _a, uint _b) -> uint {
        return (_a + _b - 1) / _b;
    };

    {
        TRACE_SCOPE_CAT("Raytracing.Lighting.Prepare.DispatchSizes", "Frame");
        command.has_local_lights = is_ctx.GetLightBufferParams().local_light_region.light_cnt != 0;
        command.has_env_lights   = is_ctx.GetLightBufferParams().env_light.light_cnt != 0;
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
        command.presample_grid_dispatch = uint3(
            div_ceil(is_ctx.GetGridRuntimeConfig().num_light_slot, DI_PRESAMPLE_GRID_SIZE),
            1,
            1
        );
        uint2 screen_dispatch = _rt_ctx.frame_rt.diffuse_lighting->GetExtent().xy;
        screen_dispatch.x     = div_ceil(screen_dispatch.x, DI_SCREEN_TILE_SIZE);
        screen_dispatch.y     = div_ceil(screen_dispatch.y, DI_SCREEN_TILE_SIZE);
        command.screen_dispatch = uint3(screen_dispatch, 1);
    }
    return command;
}

LightingPass::RecordResources LightingPass::CaptureResources(const RTContext& _rt_ctx) {
    TRACE_SCOPE_CAT("Raytracing.Lighting.CaptureResources", "Frame");
    const bool b_current_frame = _rt_ctx.b_current_frame;
    RaytracingTlasRef tlas = _rt_ctx.rt_scene ? _rt_ctx.rt_scene->GetTlas() : RaytracingTlasRef{};
    RaytracingTlasRef prev_tlas = _rt_ctx.rt_scene ? _rt_ctx.rt_scene->GetPrevTlas() : RaytracingTlasRef{};
    if (!prev_tlas) {
        prev_tlas = tlas;
    }
    return RecordResources{
        .tlas                    = tlas,
        .prev_tlas               = prev_tlas,
        .light_reservoir_buf     = RTRHI(_rt_ctx.light_reservoir_buf),
        .diffuse_lighting        = RTRHI(_rt_ctx.frame_rt.diffuse_lighting),
        .specular_lighting       = RTRHI(_rt_ctx.frame_rt.specular_lighting),
        .temporal_sample_pos     = RTRHI(_rt_ctx.frame_rt.temporal_sample_pos),
        .gradients               = RTRHI(_rt_ctx.frame_rt.gradients),
        .restir_luminance        = RTRHI(
            b_current_frame ? _rt_ctx.frame_rt.restir_luminance : _rt_ctx.frame_rt.prev_luminance
        ),
        .prev_diffuse_lighting   = RTRHI(_rt_ctx.frame_rt.prev_diffuse_lighting),
        .ris_buf                 = RTRHI(_rt_ctx.ris_buf),
        .ris_light_data_buf      = RTRHI(_rt_ctx.ris_light_data_buf),
        .neighbor_offset_buf     = RTRHI(_rt_ctx.neighbor_offset_buf),
        .bindless_array          = _rt_ctx.GetBindlessArray()
    };
}

void LightingPass::RecordConstantsUpload(CommandList& _cmd_list, const PreparedCommand& _command) {
    Array<byte> upload_data(sizeof(ResampleConstants));
    upload_data.assign((byte*)&_command.constants, (byte*)&_command.constants + sizeof(ResampleConstants));

    _cmd_list.CopyFrom(std::move(upload_data), resample_params->GetView());
}

void LightingPass::RecordDispatch(
    CommandList&           _cmd_list,
    const PreparedCommand& _command,
    const RecordResources& _resources,
    ELightingDispatch      _dispatch
) {

#define DI_BINDING_ARGS(ctx)                                                                              \
    ctx.tlas,                                                                                             \
        ctx.prev_tlas,                                                                                    \
        resample_params, ctx.light_reservoir_buf, ctx.diffuse_lighting, ctx.specular_lighting,            \
        ctx.temporal_sample_pos, ctx.gradients, ctx.restir_luminance, ctx.prev_diffuse_lighting,          \
        ctx.ris_buf, ctx.ris_light_data_buf, ctx.neighbor_offset_buf, ctx.bindless_array

    switch (_dispatch) {
        case ELightingDispatch::PresampleLight:
            _cmd_list.Compute(presample_light_pipeline, DI_BINDING_ARGS(_resources))
                .Dispatch(_command.presample_light_dispatch, MOER_TEXT("PresampleLight"));
            break;
        case ELightingDispatch::PresampleEnvMap:
            _cmd_list.Compute(presample_env_map_pipeline, DI_BINDING_ARGS(_resources))
                .Dispatch(_command.presample_env_dispatch, MOER_TEXT("PresampleEnvMap"));
            break;
        case ELightingDispatch::PresampleLightGrid:
            _cmd_list.Compute(presample_light_grid_pipeline, DI_BINDING_ARGS(_resources))
                .Dispatch(_command.presample_grid_dispatch, MOER_TEXT("PresampleLightGrid"));
            break;
        case ELightingDispatch::GenerateInitialSample:
            _cmd_list.Compute(generate_initial_sample_pipeline, DI_BINDING_ARGS(_resources))
                .Dispatch(_command.screen_dispatch, MOER_TEXT("GenerateInitialSample"));
            break;
        case ELightingDispatch::TemporalResample:
            _cmd_list.Compute(temporal_resmaple_pipeline, DI_BINDING_ARGS(_resources))
                .Dispatch(_command.screen_dispatch, MOER_TEXT("TemporalResample"));
            break;
        case ELightingDispatch::SpatialResample:
            _cmd_list.Compute(spatial_resample_pipeline, DI_BINDING_ARGS(_resources))
                .Dispatch(_command.screen_dispatch, MOER_TEXT("SpatialResample"));
            break;
        case ELightingDispatch::ShadeSample:
            _cmd_list.Compute(di_shade_sample_pipeline, DI_BINDING_ARGS(_resources))
                .Dispatch(_command.screen_dispatch, MOER_TEXT("ShadeSample"));
            break;
    }
#undef DI_BINDING_ARGS
}

void LightingPass::AddPasses(RenderGraph& _graph, const RTGraphFrameResources& _rg, const RTContext& _rt_ctx) {
    TRACE_SCOPE_CAT("Raytracing.Lighting.AddPasses", "Frame");
    auto* command = [&]() {
        TRACE_SCOPE_CAT("Raytracing.Lighting.AddPasses.Prepare", "Frame");
        return _graph.Alloc<PreparedCommand>(Prepare(_rt_ctx));
    }();
    auto* resources = [&]() {
        TRACE_SCOPE_CAT("Raytracing.Lighting.AddPasses.CaptureResources", "Frame");
        return _graph.Alloc<RecordResources>(CaptureResources(_rt_ctx));
    }();
    RGBuffer* constants{};
    RGBuffer* tlas_buffer{};
    RGBuffer* prev_tlas_buffer{};
    {
        TRACE_SCOPE_CAT("Raytracing.Lighting.AddPasses.ImportResources", "Frame");
        constants = _graph.ImportBuffer(MOER_TEXT("RT.Lighting.resample_params"), resample_params, EQueueType::Graphics);
        tlas_buffer = ImportRTTlasBufferIfValid(_graph, MOER_TEXT("RT.Lighting.tlas_buffer"), resources->tlas);
        prev_tlas_buffer = resources->prev_tlas.Get() == resources->tlas.Get() ?
                               tlas_buffer :
                               ImportRTTlasBufferIfValid(
                                   _graph,
                                   MOER_TEXT("RT.Lighting.prev_tlas_buffer"),
                                   resources->prev_tlas
                               );
    }

    auto* upload_params      = _graph.Alloc<RGLightingUploadParams>();
    upload_params->constants = RGBufferView{.buffer = constants};
    {
        TRACE_SCOPE_CAT("Raytracing.Lighting.AddPasses.UploadPass", "Frame");
        _graph.AddPass(
            MOER_TEXT("RT.Lighting.UploadConstants"),
            upload_params,
            ERGPassFlags::Graphics,
            [this, command](RHICommandList& cmd_list, RGContext) {
                RecordConstantsUpload(cmd_list, *command);
            }
        );
    }

    auto add_dispatch = [&](StringView name, ELightingDispatch dispatch) {
        TRACE_SCOPE_CAT("Raytracing.Lighting.AddPasses.DispatchPass", "Frame");
        auto* params = _graph.Alloc<RGLightingDispatchParams>();
        FillLightingDispatchParams(_graph, *params, constants, tlas_buffer, prev_tlas_buffer, _rg, *resources);
        _graph.AddPass(
            name,
            params,
            s_rt_graph_graphics_compute_pass,
            [this, command, resources, dispatch](RHICommandList& cmd_list, RGContext) {
                RecordDispatch(cmd_list, *command, *resources, dispatch);
            }
        );
    };

    if (command->has_local_lights) {
        add_dispatch(MOER_TEXT("RT.Lighting.PresampleLight"), ELightingDispatch::PresampleLight);
    }
    if (command->has_env_lights) {
        add_dispatch(MOER_TEXT("RT.Lighting.PresampleEnvMap"), ELightingDispatch::PresampleEnvMap);
    }
    if (command->has_local_lights) {
        add_dispatch(MOER_TEXT("RT.Lighting.PresampleLightGrid"), ELightingDispatch::PresampleLightGrid);
    }

    add_dispatch(MOER_TEXT("RT.Lighting.GenerateInitialSample"), ELightingDispatch::GenerateInitialSample);
    add_dispatch(MOER_TEXT("RT.Lighting.TemporalResample"), ELightingDispatch::TemporalResample);
    add_dispatch(MOER_TEXT("RT.Lighting.SpatialResample"), ELightingDispatch::SpatialResample);
    add_dispatch(MOER_TEXT("RT.Lighting.ShadeSample"), ELightingDispatch::ShadeSample);
}

} // namespace Moer::Render::Raytracing
