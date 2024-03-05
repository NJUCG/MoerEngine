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

  // uint inside_count = IsInsideFrustum(cull_data.planes, corners[0]);
  // inside_count += IsInsideFrustum(cull_data.planes, corners[1]);
  // inside_count += IsInsideFrustum(cull_data.planes, corners[2]);
  // inside_count += IsInsideFrustum(cull_data.planes, corners[3]);
  // inside_count += IsInsideFrustum(cull_data.planes, corners[4]);
  // inside_count += IsInsideFrustum(cull_data.planes, corners[5]);
  // inside_count += IsInsideFrustum(cull_data.planes, corners[6]);
  // inside_count += IsInsideFrustum(cull_data.planes, corners[7]);

  // return inside_count > 0;
  for (uint i = 0; i < 6; i++) {
    float3 p = min_pos;
    if (cull_data.planes[i].x > 0) {
      p.x = max_pos.x;
    }
    if (cull_data.planes[i].y > 0) {
      p.y = max_pos.y;
    }
    if (cull_data.planes[i].z > 0) {
      p.z = max_pos.z;
    }
    if (dot(float4(p, 1.0f), cull_data.planes[i]) < 0) {
      return false;
    }
  }
  return true;
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