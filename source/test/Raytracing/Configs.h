#ifndef MOER_RAYTRACING_CONFIGS_H
#define MOER_RAYTRACING_CONFIGS_H

#include "misc/Traits.h"
#include "rhi/RHICommand.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
namespace Moer::Render {

    static constexpr uint s_num_restirdi_reservoir_buffer = 3;

    inline static uint JekinsHash(uint _key) {
        _key = (_key + 0x7ed55d16) + (_key << 12);
        _key = (_key ^ 0xc761c23c) ^ (_key >> 19);
        _key = (_key + 0x165667b1) + (_key << 5);
        _key = (_key + 0xd3a2646c) ^ (_key << 9);
        _key = (_key + 0xfd7046c5) + (_key << 3);
        _key = (_key ^ 0xb55a4f09) ^ (_key >> 16);
        return _key;
    }

    class SimpleSegmentAllocator {
    public:
        SimpleSegmentAllocator();

        uint Allocate(uint _size_elements);
        uint GetTotalSize() const;//in elements

    private:
        uint total_element_cnt;
    };

    struct GridConfig {
        uint3 grid_size{16};
        uint  grid_mode      = s_di_local_light_sample_mode_power_ris;
        uint  light_per_ceil = 512;
    };

    struct GridRuntimeConfig {
        uint num_light_slot;
    };

    struct GridChangableConfig {
        float  cell_size = 25.f;
        float3 center    = float3(0.f);

        float grid_jitter            = 1.f;
        uint  num_grid_build_samples = 8;
    };

    struct BufferSegmentParam {
        uint tile_size;
        uint tile_count;
    };

    namespace DI {
        struct ReSTIRDIConfig {
            uint neighbor_offset_cnt = 8092;
            uint render_width        = 0;
            uint render_height       = 0;
        };
        struct ReSTIRDIRuntimeConfig {
            uint                      resample_mode;
            DI::ReservoirBufferParams reservoir_buffer_params;
            DI::CommonParams          common_params;
        };
    }// namespace DI

    struct ImportanceSamplingConfig {
    };

    inline static DI::ReSTIRDIInitialSampleParams GetDefaultReSTIRDIInitialSampleParams() {
        DI::ReSTIRDIInitialSampleParams params;
        params.num_primary_local_lights    = 8;
        params.num_primary_infinite_lights = 1;
        params.num_primary_env_lights      = 1;
        params.num_primary_brdf_lights     = 1;
        params.brdf_cutoff                 = 0.001f;
        params.enable_initial_visiblity    = 1;
        params.env_map_is                  = 1;
        params.local_light_sample_mode     = s_di_local_light_sample_mode_grid;
        return params;
    }

    inline static DI::ReSTIRDITemporalResampleParams GetDefaultReSTIRDIResampleParams() {
        DI::ReSTIRDITemporalResampleParams params;
        params.enbale_boiling_filter        = 1;
        params.enable_permutation_sample    = 1;
        params.boiling_filter_scale         = 0.1f;
        params.permutation_sample_threshold = 0.9f;
        params.discard_inviable_samples     = 0;
        params.max_history_length           = 16;
        params.bias_correction_mode         = s_di_bias_correction_traced;
        params.depth_threshold              = 10.1f;
        params.normal_threshold             = 0.5f;
        return params;
    }

    inline static DI::ReSTIRDISpatialResampleParams GetDefaultReSTIRDISpatialResampleParams() {
        DI::ReSTIRDISpatialResampleParams params;
        params.bias_correction_mode     = s_di_bias_correction_traced;
        params.depth_threshold          = 0.1f;
        params.normal_threshold         = 0.6f;
        params.num_disocclusion_samples = 8;
        params.num_spatial_samples      = 2;
        params.radius                   = 32.f;
        return params;
    }

    inline static DI::ReSTIRDIShadingParams GetDefaultReSTIRDIShadingParams() {
        DI::ReSTIRDIShadingParams params;
        params.enable_denoiser_input_packing = 0;
        params.enable_final_visiblity        = 1;
        params.final_visiblity_max_age       = 4;
        params.final_visiblity_max_distance  = 16.f;
        params.reuse_final_visiblity         = 1;
        return params;
    }

    inline static DI::ReSTIRDIBufferIndices GetDefaultReSTIRDIBufferIndicesParams() {
        DI::ReSTIRDIBufferIndices params;
        std::memset(&params, 0, sizeof(DI::ReSTIRDIBufferIndices));
        return params;
    }

    struct ImportantSamplingParams {
        BufferSegmentParam local_light_ris_buffer_segment_params{1024, 128};
        BufferSegmentParam env_light_ris_buffer_segment_params{1024, 128};

        GridConfig grid_config{};
        uint2      render_size;

        uint neighbor_offset_cnt = 8092;
    };

    struct ImportanceSamplingContext {

        ImportanceSamplingContext(const ImportantSamplingParams& _param);

        DI::ReSTIRDIInitialSampleParams& GetDIInitialSampleParams() {
            return di_initial_sample_params;
        }

        const DI::ReSTIRDIInitialSampleParams& GetDIInitialSampleParams() const {
            return di_initial_sample_params;
        }

        DI::ReSTIRDITemporalResampleParams& GetDITemporalResampleParams() {
            return di_temporal_resample_params;
        }

        const DI::ReSTIRDITemporalResampleParams& GetDITemporalResampleParams() const {
            return di_temporal_resample_params;
        }

        DI::ReSTIRDISpatialResampleParams& GetDISpatialResampleParams() {
            return di_spatial_resample_settings;
        }

        const DI::ReSTIRDISpatialResampleParams& GetDISpatialResampleParams() const {
            return di_spatial_resample_settings;
        }

        DI::ReSTIRDIShadingParams& GetDIShadingParams() {
            return di_shading_params;
        }

        const DI::ReSTIRDIShadingParams& GetDIShadingParams() const {
            return di_shading_params;
        }

        const DI::ReSTIRDIBufferIndices& GetReSTIRDIBufferIndices() const {
            return di_buffer_indices;
        }

        uint GetNeighborOffsetCnt() const {
            return restir_di_config.neighbor_offset_cnt;
        }
        DI::RISBufferSegmentParams& GetLocalLightRISBufferParams() {
            return local_light_ris_buffer_params;
        }

        const DI::RISBufferSegmentParams& GetLocalLightRISBufferParams() const {
            return local_light_ris_buffer_params;
        }

        DI::RISBufferSegmentParams& GetEnvLightRISBufferParams() {
            return env_light_ris_buffer_params;
        }

        const DI::RISBufferSegmentParams& GetEnvLightRISBufferParams() const {
            return env_light_ris_buffer_params;
        }

        const DI::LightBufferParams& GetLightBufferParams() const {
            return light_buffer_params;
        }

        void SetChangeableGridConfig(const GridChangableConfig& _config) {
            grid_changable_config                       = _config;
            grid_params.common_params.cell_size         = _config.cell_size;
            grid_params.common_params.center_x          = _config.center.x;
            grid_params.common_params.center_y          = _config.center.y;
            grid_params.common_params.center_z          = _config.center.z;
            grid_params.common_params.jitter            = _config.grid_jitter;
            grid_params.common_params.num_build_samples = _config.num_grid_build_samples;
        }

        void SetGridConfig(const GridConfig& _config) {
            grid_config = _config;
            ComputeGridLightSlotCnt();
        }

        void SetReSTIRDIInitialSampleMode(uint _sample_mode) {
            di_initial_sample_params.local_light_sample_mode = _sample_mode;
        }

        //called in prepare lights in each frame
        void SetLightBufferParams(uint _frame_offset, uint _num_finit_lights, uint _num_infinit_prim_lights, uint _num_env_lights);

        GridConfig GetGridConfig() const {
            return grid_config;
        }

        GridRuntimeConfig GetGridRuntimeConfig() const {
            return grid_runtime_config;
        }

        GridChangableConfig GetGridChangableConfig() const {
            return grid_changable_config;
        }

        const Grid::Params& GetGridParams() const {
            return grid_params;
        }

        uint GetGridCeillOffset() const {
            return grid_runtime_config.num_light_slot;
        }

        const DI::ReSTIRDIRuntimeConfig& GetReSTIRDIRuntimeConfig() const {
            return restir_di_runtime_config;
        }

        const DI::ReSTIRDIConfig& GetReSTIRDIConfig() const {
            return restir_di_config;
        }
        const SimpleSegmentAllocator& GetSegmentAllocator() const {
            return segment_allocator;
        }

        uint GetFrameIdx() const {
            return frame_idx;
        }

        void TickFrame(uint _frame_idx);

    private:
        void ComputeGridLightSlotCnt();
        void UpdateReSTIRDIBufferIndices();
        void AdvanceFrameIdx(uint _frame_idx);

    private:
        DI::ReSTIRDIInitialSampleParams    di_initial_sample_params;
        DI::ReSTIRDITemporalResampleParams di_temporal_resample_params;
        DI::ReSTIRDISpatialResampleParams  di_spatial_resample_settings;
        DI::ReSTIRDIShadingParams          di_shading_params;
        DI::ReSTIRDIBufferIndices          di_buffer_indices;

        DI::ReSTIRDIConfig        restir_di_config;
        DI::ReSTIRDIRuntimeConfig restir_di_runtime_config;

        DI::RISBufferSegmentParams local_light_ris_buffer_params;
        DI::RISBufferSegmentParams env_light_ris_buffer_params;
        DI::LightBufferParams      light_buffer_params;

        GridConfig          grid_config;
        GridRuntimeConfig   grid_runtime_config;
        GridChangableConfig grid_changable_config;

        Grid::Params grid_params;

        SimpleSegmentAllocator segment_allocator;

        uint grid_cell_offset = 0;
        uint light_slot_cnt   = 0;

        uint frame_idx = 0;

        uint di_last_frame_output_reservoir    = 0;
        uint di_current_frame_output_reservoir = 0;
    };

}// namespace Moer::Render

#endif