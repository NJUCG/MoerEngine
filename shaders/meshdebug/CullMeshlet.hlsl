#include "framework/Common.hlsl"
#define MAX_MESHLET_COUNT uint(1024 * 1024 * 8)

struct TaskInput {
  CameraData camera_data;
  uint instance_count;
  uint meshlet_count;
};
struct InstanceMeshletCullInfo {
  uint meshlet_id;
  uint instance_id;
};
[[vk::push_constant]] ConstantBuffer<TaskInput> input;

StructuredBuffer<InstanceData> instance_data : register(t0, space0);
StructuredBuffer<InstanceMeshletInfo> instance_meshlet_info
    : register(t1, space0);

StructuredBuffer<MeshletDesc> meshlet_info_buffer : register(t0, space1);
StructuredBuffer<MeshletBound> meshlet_bound_buffer : register(t1, space1);

StructuredBuffer<InstanceMeshletCullInfo> instance_meshlet_cull_info
    : register(t2, space0);

// 0 for processed instance, 1 for processed meshlet
RWByteAddressBuffer counters_buffer : register(u0, space1);

bool IsInstanceVisible(in InstanceData instance) {
  // Occlusion culling logic goes here
  // Return true if the instance is visible, false otherwise
  return true;
}

[numthreads(64, 1, 1)] void main(uint3 dispatch_thread_id
                                 : SV_DispatchThreadID) {
  uint meshlet_cull_instance = dispatch_thread_id.x;
  InstanceMeshletCullInfo cull_info =
      instance_meshlet_cull_info[meshlet_cull_instance];

  uint instance_id = cull_info.instance_id;
  uint meshlet_id = cull_info.meshlet_id;

  MeshletBound meshlet_info = meshlet_bound_buffer[meshlet_id];
  // fetch and cull instance
  while (true) {
    // first process instance
    uint instance_start_offset;

    uint laneCount = WaveGetLaneCount();
    if (WaveIsFirstLane()) {
      counters_buffer.InterlockedAdd(0, laneCount, instance_start_offset);
    }
    instance_start_offset =
        WaveReadFirst(instance_start_offset) + WaveReadLaneIndex();

    if (instance_start_offset >= input.instance_count) {
      break;
    }

    InstanceData instance = instance_data[instance_start_offset];

    bool visible = IsInstanceVisible(instance);

    uint instance_count = WaveActiveCountBits(visible);
    uint lane_offset = WavePrefixCountBits(visible);

    uint culled_meshlet_count =
        visible ? instance_meshlet_info[instance_start_offset].meshlet_count
                : 0;

    uint total_culled_meshlet_count = WaveActiveSum(culled_meshlet_count);

    uint cull_meshlet_offset;
    if (WaveIsFirstLane()) {
      counters_buffer.InterlockedAdd(8, total_culled_meshlet_count,
                                     cull_meshlet_offset);
    }
    cull_meshlet_offset = WaveReadLaneFirst(cull_meshlet_offset);
    cull_meshlet_offset += WavePrefixSum(culled_meshlet_count);

    if (visible) {
      for (uint i = 0; i < culled_meshlet_count; i++) {
        InstanceMeshletCullInfo cull_info;
        cull_info.instance_id = instance_start_offset;
        cull_info.meshlet_id = i;
        instance_meshlet_cull_info[cull_meshlet_offset + i] = cull_info;
        // process meshlet
      }
    }
  }
  // global sync
  while (true) {
    // process meshlet
    uint cull_meshlet_offset = 0;
    if (WaveIsFirstLane()) {
      counters_buffer.InterlockedAdd(12, WaveGetLaneCount(),
                                     cull_meshlet_offset);
    }

    cull_meshlet_offset =
        WaveReadLaneFirst(cull_meshlet_offset) + WaveReadLaneIndex();
  }

  // Dispatch meshlet
}