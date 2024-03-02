#include "framework/Common.hlsl"
#define MAX_MESHLET_COUNT uint(1024 * 1024 * 8)

struct TaskInput {
  CameraData camera_data;
  uint instance_count;
  uint counter_buffer_offset;
};

[[vk::push_constant]] ConstantBuffer<TaskInput> input;

[[vk::binding(0, 0)]] StructuredBuffer<InstanceData> instance_data
    : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<InstanceMeshletInfo>
    instance_meshlet_info : register(t1, space0);

[[vk::binding(0, 1)]] RWStructuredBuffer<InstanceMeshletCullInfo>
    instance_meshlet_cull_info : register(u0, space0);

// 0 for processed instance, 1 for processed meshlet
[[vk::binding(0, 3)]] RWByteAddressBuffer counters_buffer
    : register(u1, space1);

bool IsInstanceVisible(in InstanceData instance) {
  // Occlusion culling logic goes here
  // Return true if the instance is visible, false otherwise
  return true;
}

[numthreads(64, 1, 1)] void main(uint3 dtid
                                 : SV_DispatchThreadID) {
  // first process instance
  uint instance_start_offset = dtid;
  if (instance_start_offset >= input.instance_count) {
    return;
  }

  InstanceData instance = instance_data[instance_start_offset];

  bool visible = IsInstanceVisible(instance);

  uint instance_count = WaveActiveCountBits(visible);
  uint lane_offset = WavePrefixCountBits(visible);

  InstanceMeshletInfo instance_mesh_info =
      instance_meshlet_info[instance_start_offset];
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