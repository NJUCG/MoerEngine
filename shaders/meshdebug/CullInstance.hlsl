#include "framework/Common.hlsl"
#define MAX_MESHLET_COUNT uint(1024 * 1024 * 8)

struct TaskInput {
  uint instance_count;
  uint counter_buffer_offset;
};

struct CameraCullData {
  CameraData camera_data;
  float4 planes[6]; // world space planes
};

[[vk::push_constant]] ConstantBuffer<TaskInput> input;

[[vk::binding(0, 0)]] ConstantBuffer<CameraCullData> cull_data : register(b0);

[[vk::binding(0, 1)]] StructuredBuffer<InstanceData> instance_data
    : register(t0, space0);
[[vk::binding(1, 1)]] StructuredBuffer<InstanceMeshletInfo>
    instance_meshlet_info : register(t1, space0);

[[vk::binding(0, 2)]] RWStructuredBuffer<InstanceMeshletCullInfo>
    instance_meshlet_cull_info : register(u0, space0);

// 0 for processed instance, 1 for processed meshlet
[[vk::binding(0, 3)]] RWByteAddressBuffer counters_buffer
    : register(u1, space1);

uint IsInsideFrustum(in float4 planes[6], in float3 pos) {
  bool inside = true;
  [unroll] for (uint i = 0; i < 6; i++) {
    // if (dot(float4(pos, 1.0f), planes[i]) < 0) {
    //   return 0;
    // }
    inside &= (dot(float4(pos, 1.0f), planes[i]) > 0);
  }
  return inside ? 1 : 0;
}
bool IsInstanceVisible(in InstanceMeshletInfo instance) {
  // frustum cull use aabb

  float3 min_pos = instance.center - instance.extent;
  float3 max_pos = instance.center + instance.extent;

  // float3 corners[8] = {
  //     float3(min_pos.x, min_pos.y, min_pos.z),
  //     float3(min_pos.x, min_pos.y, max_pos.z),
  //     float3(min_pos.x, max_pos.y, min_pos.z),
  //     float3(min_pos.x, max_pos.y, max_pos.z),
  //     float3(max_pos.x, min_pos.y, min_pos.z),
  //     float3(max_pos.x, min_pos.y, max_pos.z),
  //     float3(max_pos.x, max_pos.y, min_pos.z),
  //     float3(max_pos.x, max_pos.y, max_pos.z),
  // };

  // // convert to vp space
  // float4x4 vp = cull_data.camera_data.view_proj;
  // [unroll] for (uint i = 0; i < 8; i++) {
  //   float4 clip = mul(vp, float4(corners[i], 1.0f));
  //   corners[i] = clip.xyz / clip.w;
  // }

  // // build new aabb, which can also be use for occlusion cull
  // float3 new_min = corners[0];
  // float3 new_max = corners[0];
  // [unroll] for (uint i = 0; i < 8; i++) {
  //   new_min = min(new_min, corners[i]);
  //   new_max = max(new_max, corners[i]);
  // }

  // bool culled = new_max.x < -1 || new_min.x > 1 || new_max.y < -1 ||
  //               new_min.y > 1 || new_max.z < 0 || new_min.z > 1;
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

[numthreads(64, 1, 1)] void main(uint3 dtid
                                 : SV_DispatchThreadID) {
  // first process instance
  uint instance_start_offset = dtid;
  if (instance_start_offset >= input.instance_count) {
    return;
  }
  InstanceMeshletInfo instance_mesh_info =
      instance_meshlet_info[instance_start_offset];

  bool visible = IsInstanceVisible(instance_mesh_info);

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
  }

  // Dispatch meshlet
}