#include "framework/Common.hlsl"
#define MAX_MESHLET_COUNT uint(1024 * 1024 * 8)

struct TaskInput {
  uint instance_count;
  uint counter_buffer_offset;
  float2 hiz_factor;
  float hiz_depth;
  uint recheck_counter_buffer_offset;
  uint draw_cnt_buffer_offset;
};

struct CameraCullData {
  CameraData camera_data;
  float4 planes[6]; // world space planes
  float4 padding;
};

[[vk::push_constant]] ConstantBuffer<TaskInput> input;

[[vk::binding(0, 0)]] ConstantBuffer<CameraCullData> cull_data : register(b0);

[[vk::binding(0, 1)]] StructuredBuffer<InstanceData> instance_data
    : register(t0, space0);
[[vk::binding(1, 1)]] StructuredBuffer<InstanceMeshletInfo>
    instance_meshlet_info : register(t1, space0);

[[vk::binding(0, 2)]] RWStructuredBuffer<InstanceMeshletCullInfo>
    instance_meshlet_cull_info : register(u0, space0);
[[vk::binding(1, 2)]] RWStructuredBuffer<uint> recheck_instance_id
    : register(u1, space0);
[[vk::binding(1, 2)]] StructuredBuffer<uint> recheck_instances
    : register(t1, space0);
// 0 for processed instance, 1 for processed meshlet
[[vk::binding(0, 3)]] RWByteAddressBuffer counters_buffer
    : register(u1, space1);

[[vk::binding(0, 4)]] Texture2D<float> hiz_depth : register(t2, space1);
[[vk::binding(1, 4)]] SamplerState depth_sampler : register(s0, space1);

static const float3 corners[8] = {
    float3(-1, -1, 0), float3(-1, 1, 0), float3(1, 1, 0), float3(1, -1, 0),
    float3(-1, -1, 1), float3(-1, 1, 1), float3(1, 1, 1), float3(1, -1, 1),
};

bool IsVisibleOccluded(in InstanceMeshletInfo instance, in float4x4 vp) {

  // occulusion test
  float2 min_xy = 0.f, max_xy = 0.f;
  float min_z = 0.f; // use reverse depth, 0 is far
                     // convert to vp space

  [unroll] for (uint i = 0; i < 8; i++) {
    float4 clip =
        mul(vp, float4(corners[i] * instance.extent + instance.center, 1.0f));
    float4 clip_pos = clip / clip.w;
    min_xy = min(min_xy, clip_pos.xy);
    max_xy = max(max_xy, clip_pos.xy);
    min_z = max(min_z, clip_pos.z); // get front z
  }
  float4 rect = float4(min_xy, max_xy);
  rect = saturate(rect * 0.5f + 0.5f); // to [0, 1]

  float2 hiz_size = (rect.zw - rect.xy) * input.hiz_factor; // rect size in hiz
  float hiz_mip = log2(max(hiz_size.x, hiz_size.y));        // mip level

  if (hiz_mip > input.hiz_depth) {
    return true;
  }

  hiz_mip = ceil(hiz_mip);
  // sample lower level if it cross less than 2 pixel rect
  float lower_level = max(0.f, hiz_mip - 1.f);
  float2 scale = exp2(-lower_level) * input.hiz_factor;
  float2 lower_min_xy = floor(rect.xy * scale);
  float2 lower_max_xy = ceil(rect.zw * scale);

  float2 lower_extent = (lower_max_xy - lower_min_xy);
  hiz_mip = max(lower_extent.x, lower_extent.y) <= 2.f ? lower_level : hiz_mip;

  float4 depth_quad =
      float4(hiz_depth.SampleLevel(depth_sampler, rect.xy, hiz_mip),
             hiz_depth.SampleLevel(depth_sampler, rect.zy, hiz_mip),
             hiz_depth.SampleLevel(depth_sampler, rect.xw, hiz_mip),
             hiz_depth.SampleLevel(depth_sampler, rect.zw, hiz_mip));
  depth_quad.xy = min(depth_quad.xy, depth_quad.zw);
  depth_quad.x = min(depth_quad.x, depth_quad.y);
  // return min_z > depth_quad.x;
  return true;
}
bool IsFrustumVisible(in InstanceMeshletInfo instance) {
  // frustum cull use aabb

  float3 min_pos = instance.center - instance.extent;
  float3 max_pos = instance.center + instance.extent;

  bool b_outside = false;
  // plane tests
  for (uint i = 0; i < 6; i++) {
    float3 plane_normal = cull_data.planes[i].xyz;
    float3 candidate_point = float3(plane_normal.x > 0 ? max_pos.x : min_pos.x,
                                    plane_normal.y > 0 ? max_pos.y : min_pos.y,
                                    plane_normal.z > 0 ? max_pos.z : min_pos.z);

    b_outside |= dot(float4(candidate_point, 1.0f), cull_data.planes[i]) < 0;
  }

  return !b_outside;
}

[numthreads(64, 1, 1)] void prepass(uint3 dtid
                                    : SV_DispatchThreadID) {
  uint instance_start_offset = dtid.x;
  if (instance_start_offset >= input.instance_count) {
    return;
  }
  InstanceMeshletInfo instance_mesh_info =
      instance_meshlet_info[instance_start_offset];

  bool vis_frustum = IsFrustumVisible(instance_mesh_info);

  bool vis_occluded = IsVisibleOccluded(instance_mesh_info,
                                        cull_data.camera_data.prev_view_proj);

  bool visible = vis_frustum && vis_occluded;
  uint instance_count = WaveActiveCountBits(visible);
  uint lane_offset = WavePrefixCountBits(visible);

  uint culled_meshlet_count = visible ? instance_mesh_info.meshlet_count : 0;

  uint total_culled_meshlet_count = WaveActiveSum(culled_meshlet_count);

  uint cull_meshlet_offset;

  if (WaveIsFirstLane()) {
    counters_buffer.InterlockedAdd(input.counter_buffer_offset,
                                   total_culled_meshlet_count,
                                   cull_meshlet_offset);
  }
  cull_meshlet_offset = WaveReadLaneFirst(cull_meshlet_offset);
  cull_meshlet_offset += WavePrefixSum(culled_meshlet_count);
  // printf("total count %d\n", vis_frustum ? 1 : culled_meshlet_count);

  if (visible) {
    for (uint i = 0; i < culled_meshlet_count; i++) {
      InstanceMeshletCullInfo cull_info;
      uint meshlet_id = instance_mesh_info.meshlet_offset + i;
      InstanceMeshletInfo instance_mesh_info =
          instance_meshlet_info[instance_start_offset];

      cull_info.instance_id = instance_start_offset;
      cull_info.meshlet_id = meshlet_id;
      instance_meshlet_cull_info[cull_meshlet_offset + i] = cull_info;
    }
    return;
  }
  bool recheck = vis_frustum && !vis_occluded;

  uint recheck_instance_count = WaveActiveCountBits(recheck);

  uint recheck_offset;
  if (WaveIsFirstLane()) {
    counters_buffer.InterlockedAdd(input.recheck_counter_buffer_offset,
                                   recheck_instance_count, recheck_offset);
  }
  recheck_offset = WaveReadLaneFirst(recheck_offset);
  recheck_offset += WavePrefixCountBits(recheck);
  if (recheck) {
    recheck_instance_id[recheck_offset] = instance_start_offset;
  }
}

    [numthreads(64, 1, 1)] void recheck_pass(uint3 dtid
                                             : SV_DispatchThreadID) {
  if (dtid.x == 0) {
    counters_buffer.Store<uint>(input.draw_cnt_buffer_offset, 0);
  }
  uint total_count =
      counters_buffer.Load<uint>(input.recheck_counter_buffer_offset);
  if (dtid.x >= total_count) {
    return;
  }
  uint instance_start_offset = recheck_instances[dtid.x];
  InstanceMeshletInfo instance_mesh_info =
      instance_meshlet_info[instance_start_offset];

  // bool vis_occluded =
  //     IsVisibleOccluded(instance_mesh_info, cull_data.camera_data.view_proj);
  bool vis_occluded = true;
  uint culled_meshlet_count =
      vis_occluded ? instance_mesh_info.meshlet_count : 0;

  // remember to reset counter before this pass
  uint total_culled_meshlet_count = WaveActiveSum(culled_meshlet_count);
  uint cull_meshlet_offset;
  if (WaveIsFirstLane()) {
    counters_buffer.InterlockedAdd(input.counter_buffer_offset,
                                   total_culled_meshlet_count,
                                   cull_meshlet_offset);
  }
  cull_meshlet_offset = WaveReadLaneFirst(cull_meshlet_offset);
  cull_meshlet_offset += WavePrefixSum(culled_meshlet_count);

  if (vis_occluded) {
    for (uint i = 0; i < culled_meshlet_count; i++) {
      InstanceMeshletCullInfo cull_info;
      uint meshlet_id = instance_mesh_info.meshlet_offset + i;
      InstanceMeshletInfo instance_mesh_info =
          instance_meshlet_info[instance_start_offset];

      cull_info.instance_id = instance_start_offset;
      cull_info.meshlet_id = meshlet_id;
      instance_meshlet_cull_info[cull_meshlet_offset + i] = cull_info;
    }
  }
}
// Dispatch meshlet
