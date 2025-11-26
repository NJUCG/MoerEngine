#include <framework/DI/Bindings.hlsli>
#include <framework/DI/ReSampleFunctions.hlsli>
#include <hwrt/GBufferUtils.hlsli>

[numthreads(DI_SCREEN_TILE_SIZE, DI_SCREEN_TILE_SIZE, 1)] void
main(uint2 dtid : SV_DISPATCHTHREADID, uint2 gtid : SV_GROUPTHREADID, uint2 _gid : SV_GROUPID){
    bool use_prev_sample = false;
    int2 select_pos = -1;
    int2 select_prev_pos = -1;
    float2 select_diff_spec_luminance = 0;

    for(int yy = 0; yy < DI_GRAD_FACTOR; yy++)
    for(int xx = 0; xx < DI_GRAD_FACTOR; xx++){
        int2 src_res_pos = dtid * DI_GRAD_FACTOR + int2(xx, yy);
        int2 src_pos =  src_res_pos;
        if(any(src_res_pos >= resample_params.main_view.rect)){
            continue;
        }

        int2 temporal_pos = rw_temporal_sample_pos[src_res_pos];
        TextureHandle restir_luminance = TextureHandle(resample_params.bindless_handles.restir_luminance);
        
        float2 prev_luminance = restir_luminance.GetTexture2D<float2>()[temporal_pos];
        float2 cur_luminance = rw_restir_luminance[src_res_pos];
        
        float cur_max_lum = max(cur_luminance.x, cur_luminance.y);
        float prev_max_lum = max(prev_luminance.x, prev_luminance.y);

        float select_max_lum = max(select_diff_spec_luminance.x, select_diff_spec_luminance.y);

        if(cur_max_lum > select_max_lum && cur_max_lum > prev_max_lum){
            select_diff_spec_luminance = cur_luminance;
            select_pos = src_pos;
            select_prev_pos = temporal_pos;
            use_prev_sample = false;
        }else if(prev_max_lum > select_max_lum){
            select_diff_spec_luminance = prev_luminance;
            select_pos = src_pos;
            select_prev_pos = temporal_pos;
            use_prev_sample = true;
        }
    }
    float4 gradient = 0.f;
    if(any(select_diff_spec_luminance > 0.f)){
        int2 select_cur_or_prev_pos = use_prev_sample ? select_prev_pos : select_pos;
        Moer::DI::Reservoir res = Moer::DI::LoadReservoir(
            resample_params.restir_di_params.reservoir_buffer_params, select_cur_or_prev_pos,
            use_prev_sample ?
            resample_params.restir_di_params.buffer_indices.temperal_resample_input_buff_idx :
            resample_params.restir_di_params.buffer_indices.shading_input_buff_idx);
    
        int selected_mapped_light_idx = Moer::DI::GetMappedLightIndex(res.GetLightIndex());

        if(selected_mapped_light_idx > 0){
            Moer::DI::Surface surface = Moer::GetGBufferSurface(select_pos);

            TextureHandle motion_handle = TextureHandle(resample_params.bindless_handles.motion);
            float3 motion = motion_handle.GetTexture2D<float3>()[select_pos].xyz;
            
            motion = Moer::MotionToPixelSpace(resample_params.main_view, resample_params.prev_view, select_pos, motion);
            float3 prev_world_pos = Moer::GetPrevWorldPos(resample_params.prev_view, select_pos, surface.v_z, motion);
            float3 world_motion = surface.x - prev_world_pos;

            if(use_prev_sample){
                surface = Moer::GetGBufferSurface(select_prev_pos, true);
                surface.x += world_motion;
            }else{
                surface.x = prev_world_pos;
            }

            Moer::PolymorphicLightInfo light_info = Moer::LoadLightInfo(selected_mapped_light_idx);
            Moer::DI::LightSample l_sample = surface.SamplePolymorphicLight(light_info, res.GetUV());

            float3 diffuse = 0.f;
            float3 specular = 0.f;
            float light_dist = 0.f;

            Moer::ShadeSurface(res, surface, l_sample, !use_prev_sample, false, diffuse, specular, light_dist);
            float2 new_diff_spec_lum = float2(STL::Color::Luminance(diffuse * surface.diffuse_albedo), STL::Color::Luminance(specular));

            new_diff_spec_lum = f16tof32(f32tof16(new_diff_spec_lum));

            gradient.xy = abs(select_diff_spec_luminance - new_diff_spec_lum);
            gradient.zw = max(select_diff_spec_luminance, new_diff_spec_lum);
        }else{
            gradient.xy = select_diff_spec_luminance;
            gradient.zw = select_diff_spec_luminance;
        }
    }

    rw_gradients[int3(dtid, 0)] = min(gradient * DI_GRAD_STORAGE_SCALE, DI_GRAD_MAX_VALUE);

}
