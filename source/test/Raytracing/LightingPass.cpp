#include "LightingPass.h"
#include "Configs.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shader/ShaderResourceManager.h"
namespace Moer::Render {

    LightingPass::LightingPass(ShaderManager& _manager, Scene& _scene) : scene(_scene) {

        presample_light_pipeline      = std::move(_manager.Compute<PresampleLightPipeline>("lighting/PresampleLight.hlsl"));
        presample_env_map_pipeline    = std::move(_manager.Compute<PresampleEnvMapPipeline>("lighting/PresampleEnvMap.hlsl"));
        presample_light_grid_pipeline = std::move(_manager.Compute<PresampleLightGridPipeline>("lighting/PresampleLightGrid.hlsl"));
    }

    void LightingPass::Process(
        CommandList& _cmd_list,
        RTContext&   _rt_ctx) {
        //process
        const DI::ReSTIRDIConfig&        restir_di_config         = _rt_ctx.is_ctx.GetReSTIRDIConfig();
        const DI::ReSTIRDIRuntimeConfig& restir_di_runtime_config = _rt_ctx.is_ctx.GetReSTIRDIRuntimeConfig();

        const ImportanceSamplingContext& is_ctx = _rt_ctx.is_ctx;

        ResampleConstants constants{};
        constants.frame_idx            = _rt_ctx.is_ctx.GetFrameIdx();
        constants.main_view            = _rt_ctx.main_view;
        constants.prev_view            = _rt_ctx.prev_view;
        constants.enable_prev_tlas     = true;
        constants.di_params            = is_ctx.GetReSTIRDIRuntimeConfig().common_params;
        constants.local_light_pdf_size = _rt_ctx.local_light_pdf_tex ? _rt_ctx.local_light_pdf_tex->GetExtent().xy : uint2(0);
        constants.env_pdf_size         = _rt_ctx.env_pdf_tex ? _rt_ctx.env_pdf_tex->GetExtent().xy : uint2(0);
        constants.bindless_handles     = _rt_ctx.GetBindlessHandles();

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

        constants.grid_params.common_params.center_x                   = is_ctx.GetGridChangableConfig().center.x;
        constants.grid_params.common_params.center_y                   = is_ctx.GetGridChangableConfig().center.y;
        constants.grid_params.common_params.center_z                   = is_ctx.GetGridChangableConfig().center.z;
        constants.grid_params.common_params.cell_size                  = is_ctx.GetGridChangableConfig().ceil_size;
        constants.grid_params.common_params.jitter                     = is_ctx.GetGridChangableConfig().grid_jitter;
        constants.grid_params.common_params.num_build_samples          = is_ctx.GetGridChangableConfig().num_grid_build_samples;
        constants.grid_params.common_params.local_light_sampling_mode  = is_ctx.GetGridConfig().light_per_ceil;
        constants.grid_params.common_params.local_light_presample_mode = is_ctx.GetGridConfig().grid_mode;
        constants.grid_params.grid_params.cell_x                       = is_ctx.GetGridConfig().grid_size.x;
        constants.grid_params.grid_params.cell_y                       = is_ctx.GetGridConfig().grid_size.y;
        constants.grid_params.grid_params.cell_z                       = is_ctx.GetGridConfig().grid_size.z;

        constants.scene_params            = _rt_ctx.scene_params;
        constants.enable_accumulation     = 1;
        constants.discount_native_samples = 1;
        constants.visualize_cells         = 0;
    }

}// namespace Moer::Render