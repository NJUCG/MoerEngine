#include "framework/Common.hlsl"
struct MeshletCullInput {
  uint counter_buffer_offset;
  uint draw_cmd_buffer_offset;
};

struct CameraCullData {
  CameraData camera_data;
  float4 planes[6]; // world space planes
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraCullData> cull_data : register(b0);

[[vk::binding(0, 1)]] StructuredBuffer<InstanceData> instance_data
    : register(t0, space0);
[[vk::binding(1, 1)]] StructuredBuffer<InstanceMeshletInfo>
    instance_meshlet_info : register(t1, space0);

[[vk::binding(0, 2)]] StructuredBuffer<InstanceMeshletCullInfo>
    instance_meshlet_cull_info : register(t2, space0);

[[vk::binding(0, 3)]] StructuredBuffer<MeshletDesc> meshlet_info_buffer
    : register(t0, space1);
[[vk::binding(1, 3)]] StructuredBuffer<MeshletBound> meshlet_bound_buffer
    : register(t1, space1);

// 0 for processed instance, 1 for processed meshlet
[[vk::binding(0, 4)]] RWByteAddressBuffer counters_buffer
    : register(u0, space1);
[[vk::binding(1, 4)]] RWStructuredBuffer<DrawCommandData> command_buffer
    : register(u1, space1);

[[vk::push_constant]] ConstantBuffer<MeshletCullInput> input;

bool IsMeshletVisible(in MeshletBound bound, in float4x4 world,
                      in float scale) {
  float4 center = mul(float4(bound.center, 1.0f), world);
  float radius = bound.radius * scale;

  [unroll] for (uint i = 0; i < 6; i++) {
    float3 abs_normal = abs(cull_data.planes[i].xyz);
    if (dot(center, cull_data.planes[i]) < -radius) {
      return false;
    }
    // printf("planes[%d] = %f %f %f %f\n", i, cull_data.planes[i].x,
    //        cull_data.planes[i].y, cull_data.planes[i].z,
    //        cull_data.planes[i].w);
  }
  //   for (uint i = 0; i < 6; i++) {
  //     if (dot(center, cull_data.planes[i]) < -radius) {
  //       return false;
  //     }
  //   }
  // not do back face cull now

  // occclusion cull
  return true;
}

[numthreads(64, 1, 1)] void main(uint3 dtid
                                 : SV_DispatchThreadID) {
  // process meshlets
  uint total_meshlet_count =
      counters_buffer.Load<uint>(input.counter_buffer_offset);
  if (dtid.x >= total_meshlet_count) {
    return;
  }
  InstanceMeshletCullInfo cull_info = instance_meshlet_cull_info[dtid.x];

  InstanceData data = instance_data[cull_info.instance_id];
  MeshletBound bound = meshlet_bound_buffer[cull_info.meshlet_id];

  bool visible = IsMeshletVisible(bound, data.model2world, data.scale);

  uint wave_meshlet_count = WaveActiveCountBits(visible);
  uint cmd_offset = 0;
  if (WaveIsFirstLane()) {
    counters_buffer.InterlockedAdd(input.draw_cmd_buffer_offset,
                                   wave_meshlet_count, cmd_offset);
  }
  uint lane_offset = WavePrefixCountBits(visible);
  cmd_offset = WaveReadLaneFirst(cmd_offset) + lane_offset;

  if (visible) {
    DrawCommandData cmd;
    MeshletDesc meshlet_desc = meshlet_info_buffer[cull_info.meshlet_id];
    InstanceMeshletInfo instance_mesh_info =
        instance_meshlet_info[cull_info.instance_id];

    cmd.index_count = meshlet_desc.index_count;
    cmd.instance_count = 1;
    cmd.first_index =
        meshlet_desc.index_offset + instance_mesh_info.index_offset;
    cmd.vertex_offset =
        meshlet_desc.vertex_offset + instance_mesh_info.vertex_offset;
    cmd.first_instance = cull_info.instance_id;
    command_buffer[cmd_offset] = cmd;
  }
}