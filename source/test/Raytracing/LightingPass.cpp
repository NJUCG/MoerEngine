#include "LightingPass.h"
#include "Configs.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shader/ShaderResourceManager.h"
namespace Moer::Render {

    LightingPass::LightingPass(ShaderManager& _manager, Scene& _scene) : scene(_scene) {

        presample_light_pipeline      = std::move(_manager.Compute<PresampleLightPipeline>("lighting/PresampleLight.hlsl"));
        presample_env_map_pipeline    = std::move(_manager.Compute<PresampleEnvMapPipeline>("lighting/PresampleEnvMap.hlsl"));
        presample_light_grid_pipeline = std::move(_manager.Compute<PresampleLightGridPipeline>("lighting/PresampleLightGrid.hlsl"));

        generate_initial_sample_pipeline = std::move(_manager.Compute<GenerateInitialSamplePipeline>("hwrt/ReSTIRDI/GenerateInitialSamples.hlsl"));

        auto& device    = RenderDevice::Get();
        resample_params = device.CreateBuffer<byte>(sizeof(ResampleConstants), EBufferUsageFlags::CONSTANT_BUFFER);
        resample_params->SetName("DI::resample_params");
    }

    void LightingPass::Process(
        CommandList& _cmd_list,
        RTContext&   _rt_ctx) {
        //process
        const DI::ReSTIRDIConfig&        restir_di_config         = _rt_ctx.is_ctx.GetReSTIRDIConfig();
        const DI::ReSTIRDIRuntimeConfig& restir_di_runtime_config = _rt_ctx.is_ctx.GetReSTIRDIRuntimeConfig();

        const ImportanceSamplingContext& is_ctx = _rt_ctx.is_ctx;

        constants.frame_idx            = _rt_ctx.is_ctx.GetFrameIdx();
        constants.main_view            = _rt_ctx.main_view;
        constants.prev_view            = _rt_ctx.prev_view;
        constants.enable_prev_tlas     = true;
        constants.di_params            = is_ctx.GetReSTIRDIRuntimeConfig().common_params;
        constants.local_light_pdf_size = _rt_ctx.local_light_pdf_tex ? _rt_ctx.local_light_pdf_tex->GetExtent().xy : uint2(0);
        constants.env_pdf_size         = _rt_ctx.env_pdf_tex ? _rt_ctx.env_pdf_tex->GetExtent().xy : uint2(0);
        constants.bindless_handles     = _rt_ctx.GetBindlessHandles();

        // static constexpr uint offset_of_prev_view           = offsetof(ResampleConstants, prev_view);
        // static constexpr uint offset_of_main_view           = offsetof(ViewParam, near_far);
        constants.di_params                                 = restir_di_runtime_config.common_params;
        constants.restir_di_params.initial_sample_params    = is_ctx.GetDIInitialSampleParams();
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

        _cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)&constants, sizeof(ResampleConstants)), resample_params->GetView());

#define DI_BINDING_ARGS(ctx)               \
    ctx.rt_scene->GetTlas(),               \
        ctx.rt_scene->GetPrevTlas(),       \
        resample_params,                   \
        ctx.light_reservoir_buf,           \
        ctx.frame_rt.diffuse_lighting,     \
        ctx.frame_rt.specular_lighting,    \
        ctx.frame_rt.gradients,            \
        ctx.frame_rt.restir_luminance,     \
        ctx.frame_rt.odd_diffuse_lighting, \
        ctx.ris_buf,                       \
        ctx.ris_light_data_buf,            \
        scene.GetBindlessArray()

        auto div_ceil = [](uint _a, uint _b) -> uint {
            return (_a + _b - 1) / _b;
        };

        if (is_ctx.GetLightBufferParams().local_light_region.light_cnt) {
            uint2 dispatch_size = uint2(div_ceil(is_ctx.GetLocalLightRISBufferParams().tile_size, 256), is_ctx.GetLocalLightRISBufferParams().tile_cnt);
            _cmd_list.Compute(presample_light_pipeline,
                              DI_BINDING_ARGS(_rt_ctx))
                .Dispatch(uint3(dispatch_size, 1), "PresampleLight");
        }

        if (is_ctx.GetLightBufferParams().env_light.light_cnt) {
            uint2 dispatch_size = uint2(div_ceil(is_ctx.GetEnvLightRISBufferParams().tile_size, 256), is_ctx.GetEnvLightRISBufferParams().tile_cnt);
            _cmd_list.Compute(presample_env_map_pipeline,
                              DI_BINDING_ARGS(_rt_ctx))
                .Dispatch(uint3(dispatch_size, 1), "PresampleEnvMap");
        }

        if (is_ctx.GetLightBufferParams().local_light_region.light_cnt) {
            uint2 dispatch_size = uint2(div_ceil(is_ctx.GetGridRuntimeConfig().num_light_slot, 256), 1);
            _cmd_list.Compute(presample_light_grid_pipeline,
                              DI_BINDING_ARGS(_rt_ctx))
                .Dispatch(uint3(dispatch_size, 1), "PresampleLightGrid");
        }

        //direct lighting
        {
            uint2 dispatch_size = _rt_ctx.frame_rt.diffuse_lighting->GetExtent().xy;
            dispatch_size.x     = div_ceil(dispatch_size.x, DI_SCREEN_TILE_SIZE);
            dispatch_size.y     = div_ceil(dispatch_size.y, DI_SCREEN_TILE_SIZE);

            _cmd_list.Compute(generate_initial_sample_pipeline,
                              DI_BINDING_ARGS(_rt_ctx))
                .Dispatch(uint3(dispatch_size, 1), "GenerateInitialSample");
        }
#undef DI_BINDING_ARGS
    }

}// namespace Moer::Render