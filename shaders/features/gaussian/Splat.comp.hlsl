#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] ConstantBuffer<Moer::GGaussianCamera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<Moer::GProjectedGaussian> projected;
[[vk::binding(2, 0)]] StructuredBuffer<uint4> pairs;
[[vk::binding(3, 0)]] StructuredBuffer<uint> ranges;
[[vk::binding(4, 0)]] StructuredBuffer<uint4> status;
[[vk::binding(5, 0)]] RWTexture2D<float4> color_output;
[[vk::binding(6, 0)]] RWTexture2D<float> transmittance_output;
[[vk::binding(7, 0)]] Texture2D<float> opaque_depth;
[[vk::binding(8, 0)]] RWTexture2D<float4> reference_output;


// One tile cooperatively fetches a batch, then reuses it for all 256 pixels.
// Keep the original per-pixel order and arithmetic; no subgroup-width assumption.
static const uint k_batch_size = 256;
groupshared float4 batch_centers_cutoffs[k_batch_size];
groupshared float4 batch_conics[k_batch_size];
groupshared float3 batch_colors[k_batch_size];
groupshared float3 batch_reference_colors[k_batch_size];
groupshared uint active_pixel_count;

float3 SRGBToLinear(float3 color) {
    color = max(color, 0);
    return select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
}
[numthreads(16,16,1)]
void main(uint3 group_id : SV_GroupID, uint3 local_id : SV_GroupThreadID) {
    uint2 pixel = group_id.xy * 16 + local_id.xy;
    bool is_inside = all(pixel < camera.viewport.xy);
    if (status[0].y != 0) {
        if (is_inside) {
            color_output[pixel] = float4(1,0,1,0);
            transmittance_output[pixel] = 1;

        }
        return;
    }
    uint tile_id = group_id.x + group_id.y * camera.viewport.z;
    uint2 range = uint2(ranges[tile_id * 2], ranges[tile_id * 2 + 1]);
    float3 color = 0;
    float3 reference_color = 0;
    float mesh_depth = 3.402823e+38;
    if (is_inside && param.job.z != 0) {
        float depth = opaque_depth.Load(int3(pixel, 0));
        if (depth > 0) {
            float2 ndc = ((float2(pixel) + 0.5) / float2(camera.viewport.xy) * 2 - 1) * float2(1,-1);
            float4 view = mul(camera.clip_to_view, float4(ndc, depth, 1));
            mesh_depth = -view.z / view.w;
        }
    }
    float transmittance = 1;
    bool is_active = is_inside;
    uint lane_id = local_id.x + local_id.y * 16;
    [loop] for (uint batch_start = range.x; batch_start < range.y; batch_start += k_batch_size) {
        if (lane_id == 0) active_pixel_count = 0;
        GroupMemoryBarrierWithGroupSync();
        if (is_active) InterlockedAdd(active_pixel_count, 1);
        uint pair_id = batch_start + lane_id;
        if (pair_id < range.y) {
            Moer::GProjectedGaussian gaussian = projected[pairs[pair_id].z];
            batch_centers_cutoffs[lane_id] = gaussian.center_depth;
            batch_conics[lane_id] = gaussian.conic_opacity;
            // Decode once per tile batch and share the linear result across pixels.
            batch_colors[lane_id] = param.job.x != 0 ?
                SRGBToLinear(gaussian.color.xyz) * exp2(camera.background.w) : gaussian.color.xyz;
            if (param.job.y != 0) batch_reference_colors[lane_id] = gaussian.color.xyz;
        }
        GroupMemoryBarrierWithGroupSync();
        if (active_pixel_count == 0) break;
        uint batch_count = min(k_batch_size, range.y - batch_start);
        [loop] for (uint gaussian_id = 0; is_active && gaussian_id < batch_count; ++gaussian_id) {
            float4 center_cutoff = batch_centers_cutoffs[gaussian_id];
            if (center_cutoff.z > mesh_depth + 1e-5 * max(1.0, mesh_depth)) {
                is_active = false;
                break;
            }
            float2 delta = center_cutoff.xy - float2(pixel);
            float4 conic_opacity = batch_conics[gaussian_id];
            float3 conic = conic_opacity.xyz;
            float power = -0.5 * (conic.x * delta.x * delta.x + conic.z * delta.y * delta.y) - conic.y * delta.x * delta.y;
            if (power > 0) continue;
            // Conservative log-alpha bound skips exp for certainly invisible samples.
            // Keep the original alpha test below for the FP32 threshold boundary.
            if (power < -center_cutoff.w) continue;
            float alpha = min(0.99, conic_opacity.w * exp(power));
            if (alpha < camera.filter.y) continue;
            float next_transmittance = transmittance * (1 - alpha);
            if (next_transmittance < 0.0001) {
                is_active = false;
                break;
            }
            float3 rgb = batch_colors[gaussian_id];
            if (param.job.y != 0) reference_color += batch_reference_colors[gaussian_id] * (alpha * transmittance);
            color += rgb * (alpha * transmittance);
            transmittance = next_transmittance;
        }
        // Finished and out-of-bounds pixels must still help load/synchronize batches.
        GroupMemoryBarrierWithGroupSync();
    }
    if (!is_inside) return;
    color_output[pixel] = float4(color, 1-transmittance);
    if (param.job.y != 0) reference_output[pixel] = float4(reference_color, 1-transmittance);
    transmittance_output[pixel] = transmittance;
}
