// #include "core/common/Common.hlsl"
#include "features/debug/Cull.hlsl"
#define MAX_MESHLET_COUNT uint(1024 * 1024 * 8)

struct TaskInput {
  uint instance_count;
  uint counter_buffer_offset;
  float2 hiz_factor;
  float hiz_depth;
  uint recheck_counter_buffer_offset;
};

struct CameraCullData {
  CameraData camera_data;
  float4x4 proj;
  float4 planes[6]; // world space planes
  float near_plane;
  float far_plane;
  float tan_half_fov;
  float aspect_ratio;
};

[[vk::push_constant]] ConstantBuffer<TaskInput> input;

// [[vk::binding(0, 0)]] ConstantBuffer<CameraCullData> cull_data :
// register(b0);

[[vk::binding(0, 0)]] ConstantBuffer<VirtualView> views : register(b0, space0);

[[vk::binding(0, 1)]] StructuredBuffer<InstanceMeshletInfo>
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

[numthreads(64, 1, 1)] void prepass(uint3 dtid
                                    : SV_DispatchThreadID) {
  uint instance_start_offset = dtid.x;

  if (instance_start_offset >= input.instance_count) {
    return;
  }
  InstanceMeshletInfo instance_mesh_info =
      instance_meshlet_info[instance_start_offset];
  Culler cull;
  cull.Init();
  cull.CullFrustum(views, instance_mesh_info.center, instance_mesh_info.extent);

  cull.CullHZB(views.prev_view_proj, hiz_depth, depth_sampler,
               instance_mesh_info.center, instance_mesh_info.extent,
               input.hiz_factor, input.hiz_depth);
  // current visibility
  bool visible = cull.b_visible && !cull.b_occluded;
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
  bool recheck = cull.b_visible && cull.b_occluded;

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

  uint total_count =
      counters_buffer.Load<uint>(input.recheck_counter_buffer_offset);
  if (dtid.x >= total_count) {
    return;
  }
  uint instance_start_offset = recheck_instances[dtid.x];
  InstanceMeshletInfo instance_mesh_info =
      instance_meshlet_info[instance_start_offset];

  Culler cull;
  cull.Init();

  cull.CullHZB(views.view_proj, hiz_depth, depth_sampler,
               instance_mesh_info.center, instance_mesh_info.extent,
               input.hiz_factor, input.hiz_depth);
  bool visible = !cull.b_occluded;
  uint culled_meshlet_count = visible ? instance_mesh_info.meshlet_count : 0;

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
}
// Dispatch meshlet
