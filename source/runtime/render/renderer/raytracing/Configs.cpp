#include "Configs.h"

// 维护帧间 ReSTIR 储层索引与光源采样缓冲区范围。

#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"

namespace Moer::Render {
ImportanceSamplingContext::ImportanceSamplingContext(const ImportanceSamplingParams& _param) :
    restir_di_config{},
    local_light_ris_buffer_params{},
    env_light_ris_buffer_params{},
    light_buffer_params{},
    grid_config{},
    grid_runtime_config{},
    grid_changeable_config{},
    segment_allocator{},
    di_initial_sample_params{} {

    local_light_ris_buffer_params.buffer_offset = segment_allocator.Allocate(
        _param.local_light_ris_buffer_segment_params.tile_size *
        _param.local_light_ris_buffer_segment_params.tile_count
    );
    local_light_ris_buffer_params.tile_size = _param.local_light_ris_buffer_segment_params.tile_size;
    local_light_ris_buffer_params.tile_cnt  = _param.local_light_ris_buffer_segment_params.tile_count;

    env_light_ris_buffer_params.buffer_offset = segment_allocator.Allocate(
        _param.env_light_ris_buffer_segment_params.tile_size *
        _param.env_light_ris_buffer_segment_params.tile_count
    );
    env_light_ris_buffer_params.tile_size = _param.env_light_ris_buffer_segment_params.tile_size;
    env_light_ris_buffer_params.tile_cnt  = _param.env_light_ris_buffer_segment_params.tile_count;

    grid_config = _param.grid_config;

    restir_di_config.render_width        = _param.render_size.x;
    restir_di_config.render_height       = _param.render_size.y;
    restir_di_config.neighbor_offset_cnt = _param.neighbor_offset_cnt;

    {
        di_initial_sample_params     = GetDefaultReSTIRDIInitialSampleParams();
        di_temporal_resample_params  = GetDefaultReSTIRDIResampleParams();
        di_spatial_resample_settings = GetDefaultReSTIRDISpatialResampleParams();
        di_shading_params            = GetDefaultReSTIRDIShadingParams();
        di_buffer_indices            = GetDefaultReSTIRDIBufferIndicesParams();

        uint render_block_width =
            (restir_di_config.render_width + s_di_reservoir_block_size - 1) / s_di_reservoir_block_size;
        uint render_block_height =
            (restir_di_config.render_height + s_di_reservoir_block_size - 1) / s_di_reservoir_block_size;

        restir_di_runtime_config.reservoir_buffer_params.block_row_pitch =
            render_block_width * (s_di_reservoir_block_size * s_di_reservoir_block_size);
        restir_di_runtime_config.reservoir_buffer_params.block_array_pitch =
            restir_di_runtime_config.reservoir_buffer_params.block_row_pitch * render_block_height;
        restir_di_runtime_config.common_params.neighbor_offset_mask = _param.neighbor_offset_cnt - 1;
    }

    ComputeGridLightSlotCnt();
    // 网格使用本地光源与环境光 RIS 区段之后的一段持久区间。
    grid_cell_offset = segment_allocator.Allocate(grid_runtime_config.num_light_slot);
}
void ImportanceSamplingContext::ComputeGridLightSlotCnt() {
    grid_runtime_config.num_light_slot = grid_config.grid_size.x * grid_config.grid_size.y *
                                         grid_config.grid_size.z * grid_config.GetLightsPerCell();
}

void ImportanceSamplingContext::UpdateReSTIRDIBufferIndices() {

    di_buffer_indices.initial_sample_output_buff_idx =
        (di_last_frame_output_reservoir + 1) % s_num_restirdi_reservoir_buffer;
    di_buffer_indices.temperal_resample_input_buff_idx = di_last_frame_output_reservoir;

    di_buffer_indices.temperal_resample_output_buff_idx =
        (di_buffer_indices.temperal_resample_input_buff_idx + 1) % s_num_restirdi_reservoir_buffer;
    di_buffer_indices.spatial_resample_input_buff_idx = di_buffer_indices.temperal_resample_output_buff_idx;
    di_buffer_indices.spatial_resample_output_buff_idx =
        (di_buffer_indices.spatial_resample_input_buff_idx + 1) % s_num_restirdi_reservoir_buffer;
    di_buffer_indices.shading_input_buff_idx = di_buffer_indices.spatial_resample_output_buff_idx;
    di_current_frame_output_reservoir        = di_buffer_indices.shading_input_buff_idx;
}

void ImportanceSamplingContext::SetLightBufferParams(
    uint _frame_offset,
    uint _local_light_region_light_cnt,
    uint _infinite_light_region_light_cnt,
    uint _env_light_light_cnt
) {
    light_buffer_params.local_light_region.first_light_idx = _frame_offset;
    light_buffer_params.local_light_region.light_cnt       = _local_light_region_light_cnt;
    light_buffer_params.infinite_light_region.first_light_idx =
        light_buffer_params.local_light_region.light_cnt +
        light_buffer_params.local_light_region.first_light_idx;
    light_buffer_params.infinite_light_region.light_cnt = _infinite_light_region_light_cnt;
    light_buffer_params.env_light.light_idx = light_buffer_params.infinite_light_region.first_light_idx +
                                              light_buffer_params.infinite_light_region.light_cnt;
    light_buffer_params.env_light.light_cnt = _env_light_light_cnt;
}

void ImportanceSamplingContext::AdvanceFrameIdx(uint _frame_idx) {

    //ReSTIR DI
    {
        frame_idx                                 = _frame_idx;
        di_last_frame_output_reservoir            = di_current_frame_output_reservoir;
        di_temporal_resample_params.random_number = JenkinsHash(frame_idx);
        UpdateReSTIRDIBufferIndices();

        di_initial_sample_params.env_map_is = light_buffer_params.env_light.light_cnt;
        grid_params.grid_params.cell_x      = grid_config.grid_size.x;
        grid_params.grid_params.cell_y      = grid_config.grid_size.y;
        grid_params.grid_params.cell_z      = grid_config.grid_size.z;

        grid_params.common_params.center_x          = grid_changeable_config.center.x;
        grid_params.common_params.center_y          = grid_changeable_config.center.y;
        grid_params.common_params.center_z          = grid_changeable_config.center.z;
        grid_params.common_params.cell_size         = grid_changeable_config.cell_size;
        grid_params.common_params.jitter            = grid_changeable_config.grid_jitter;
        grid_params.common_params.num_build_samples = grid_changeable_config.num_grid_build_samples;
        grid_params.common_params.lights_per_cell   = grid_config.GetLightsPerCell();
        grid_params.common_params.local_light_sampling_fallback_mode = s_di_local_light_sample_mode_power_ris;
        grid_params.common_params.local_light_sample_mode            = grid_config.grid_mode;
        grid_params.common_params.ris_buffer_offset                  = grid_cell_offset;
    }
}

void ImportanceSamplingContext::TickFrame(uint _frame_idx) {
    AdvanceFrameIdx(_frame_idx);
}

SimpleSegmentAllocator::SimpleSegmentAllocator() : total_element_count(0) {}

uint SimpleSegmentAllocator::Allocate(uint _size_elements) {
    const uint offset = total_element_count;
    total_element_count += _size_elements;
    return offset;
}

uint SimpleSegmentAllocator::GetTotalSize() const {
    return total_element_count;
}

} // namespace Moer::Render
