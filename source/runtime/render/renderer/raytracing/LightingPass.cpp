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

LightingPass::LocalLightSamplingDecision
LightingPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {
    const DI::ReSTIRDIRuntimeConfig& restir_di_runtime_config = _rt_ctx.is_ctx.GetReSTIRDIRuntimeConfig();

    const ImportanceSamplingContext& is_ctx = _rt_ctx.is_ctx;

    constants.frame_idx        = _rt_ctx.is_ctx.GetFrameIdx();
    constants.main_view        = _rt_ctx.main_view;
    constants.prev_view        = _rt_ctx.prev_view;
    constants.enable_prev_tlas = true;
    constants.local_light_pdf_size =
        _rt_ctx.local_light_pdf_tex ? _rt_ctx.local_light_pdf_tex->GetExtent().xy : uint2(0);
    constants.env_pdf_size     = _rt_ctx.env_pdf_tex ? _rt_ctx.env_pdf_tex->GetExtent().xy : uint2(0);
    constants.bindless_handles = _rt_ctx.GetBindlessHandles();

    constants.di_params = restir_di_runtime_config.common_params;
    auto initial_sample_params = is_ctx.GetDIInitialSampleParams();
    LocalLightSamplingDecision sampling_decision{};
    sampling_decision.configured_mode  = initial_sample_params.local_light_sample_mode;
    sampling_decision.local_light_count =
        is_ctx.GetLightBufferParams().local_light_region.light_cnt;
    sampling_decision.adaptive_fallback_applied =
        _rt_ctx.config.enable_adaptive_local_light_sampling &&
        initial_sample_params.local_light_sample_mode == s_di_local_light_sample_mode_grid &&
        sampling_decision.local_light_count < _rt_ctx.config.grid_min_local_light_count;
    if (sampling_decision.adaptive_fallback_applied) {
        initial_sample_params.local_light_sample_mode = s_di_local_light_sample_mode_power_ris;
    }
    sampling_decision.effective_mode = initial_sample_params.local_light_sample_mode;
    constants.restir_di_params.initial_sample_params    = initial_sample_params;
    constants.restir_di_params.temporal_resample_params = is_ctx.GetDITemporalResampleParams();
    constants.restir_di_params.spatial_resample_params  = is_ctx.GetDISpatialResampleParams();
    constants.restir_di_params.reservoir_buffer_params  = restir_di_runtime_config.reservoir_buffer_params;
    constants.restir_di_params.shading_params           = is_ctx.GetDIShadingParams();
    constants.restir_di_params.buffer_indices           = is_ctx.GetReSTIRDIBufferIndices();

    constants.light_buffer_params           = is_ctx.GetLightBufferParams();
    constants.local_light_ris_buffer_params = is_ctx.GetLocalLightRISBufferParams();
    constants.env_light_ris_buffer_params   = is_ctx.GetEnvLightRISBufferParams();

    constants.grid_params             = is_ctx.GetGridParams();
    constants.scene_params            = _rt_ctx.scene_params;
    constants.enable_accumulation     = 1;
    constants.discount_native_samples = 1;
    constants.visualize_cells         = 0;

    constants.denoiser_mode               = _rt_ctx.config.denoiser_mode;
    constants.reblur_diff_hit_dist_params = _rt_ctx.config.reblur_diffuse_hit_dist_params;
    constants.reblur_spec_hit_dist_params = _rt_ctx.config.reblur_specular_hit_dist_params;

    upload_data.resize(sizeof(ResampleConstants));
    std::memcpy(upload_data.data(), &constants, sizeof(ResampleConstants));
    _cmd_list.PushScopeWithTimeScope("LightingPass");
    _cmd_list.CopyFrom(std::move(upload_data), resample_params->GetView());
    bool b_current_frame = _rt_ctx.b_current_frame;

#define DI_BINDING_ARGS(ctx)                                                                         \
    ctx.rt_scene->GetTlas(),                                                                         \
        ctx.rt_scene->GetPrevTlas() ? ctx.rt_scene->GetPrevTlas() : ctx.rt_scene->GetTlas(),         \
        resample_params, ctx.light_reservoir_buf, ctx.frame_rt.diffuse_lighting,                     \
        ctx.frame_rt.specular_lighting, ctx.frame_rt.temporal_sample_pos, ctx.frame_rt.gradients,    \
        b_current_frame ? ctx.frame_rt.restir_luminance : ctx.frame_rt.prev_luminance,               \
        ctx.frame_rt.prev_diffuse_lighting, ctx.ris_buf, ctx.ris_light_data_buf,                     \
        ctx.neighbor_offset_buf, bindless_array

    auto div_ceil = [](uint _a, uint _b) -> uint {
        return (_a + _b - 1) / _b;
    };
    const uint local_light_count     = sampling_decision.local_light_count;
    const bool has_local_candidates = initial_sample_params.num_primary_local_lights > 0;
    const bool uses_grid =
        initial_sample_params.local_light_sample_mode == s_di_local_light_sample_mode_grid;
    const bool needs_power_ris =
        initial_sample_params.local_light_sample_mode == s_di_local_light_sample_mode_power_ris ||
        (uses_grid && constants.grid_params.common_params.local_light_sampling_fallback_mode ==
                          s_di_local_light_sample_mode_power_ris);
    // 所有通过 DI_BINDINGS 声明的 pipeline 都采用相同参数布局。这里只注册一次，
    // 避免为每个阶段重复构建等价参数包。
    ArrayArgReference arg_ref =
        _cmd_list.RegisterArgs(presample_light_pipeline.SetArgs(DI_BINDING_ARGS(_rt_ctx)));

    if (local_light_count && has_local_candidates && needs_power_ris) {
        uint2 dispatch_size = uint2(
            div_ceil(is_ctx.GetLocalLightRISBufferParams().tile_size, DI_PRESAMPLE_GRID_SIZE),
            is_ctx.GetLocalLightRISBufferParams().tile_cnt
        );
        _cmd_list.Compute(presample_light_pipeline, arg_ref)
            .Dispatch(
                uint3(dispatch_size, 1),
                "PresampleLight",
                ProfileSection("ReSTIR DI Presample Local")
            );
    }

    if (is_ctx.GetLightBufferParams().env_light.light_cnt) {
        uint2 dispatch_size = uint2(
            div_ceil(is_ctx.GetEnvLightRISBufferParams().tile_size, DI_PRESAMPLE_GRID_SIZE),
            is_ctx.GetEnvLightRISBufferParams().tile_cnt
        );
        _cmd_list.Compute(presample_env_map_pipeline, arg_ref)
            .Dispatch(
                uint3(dispatch_size, 1),
                "PresampleEnvMap",
                ProfileSection("ReSTIR DI Presample Environment")
            );
    }

    if (local_light_count && has_local_candidates && uses_grid) {
        uint2 dispatch_size =
            uint2(div_ceil(is_ctx.GetGridRuntimeConfig().num_light_slot, DI_PRESAMPLE_GRID_SIZE), 1);
        _cmd_list.Compute(presample_light_grid_pipeline, arg_ref)
            .Dispatch(
                uint3(dispatch_size, 1),
                "PresampleLightGrid",
                ProfileSection("ReSTIR DI Presample Grid")
            );
    }

    // 初始采样、时序复用、空间复用和着色均以屏幕 tile 为调度粒度。
    {
        uint2 dispatch_size = _rt_ctx.frame_rt.diffuse_lighting->GetExtent().xy;
        dispatch_size.x     = div_ceil(dispatch_size.x, DI_SCREEN_TILE_SIZE);
        dispatch_size.y     = div_ceil(dispatch_size.y, DI_SCREEN_TILE_SIZE);
        _cmd_list.Compute(generate_initial_sample_pipeline, arg_ref)
            .Dispatch(
                uint3(dispatch_size, 1),
                "GenerateInitialSample",
                ProfileSection("ReSTIR DI Initial")
            );

        _cmd_list.Compute(temporal_resample_pipeline, arg_ref)
            .Dispatch(
                uint3(dispatch_size, 1),
                "TemporalResample",
                ProfileSection("ReSTIR DI Temporal")
            );

        _cmd_list.Compute(spatial_resample_pipeline, arg_ref)
            .Dispatch(
                uint3(dispatch_size, 1),
                "SpatialResample",
                ProfileSection("ReSTIR DI Spatial")
            );

        _cmd_list.Compute(di_shade_sample_pipeline, arg_ref)
            .Dispatch(
                uint3(dispatch_size, 1),
                "ShadeSample",
                ProfileSection("ReSTIR DI Shade")
            );
    }

    _cmd_list.PopScopeWithTimeScope();
#undef DI_BINDING_ARGS
    return sampling_decision;
}

} // namespace Moer::Render::Raytracing
