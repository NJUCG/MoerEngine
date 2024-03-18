#include "framework/Common.hlsl"
struct MeshletCullInput {
  uint counter_buffer_offset;
  uint draw_cmd_buffer_offset;
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
  float inv_tan_half_fov;
  float aspect_ratio;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraCullData> cull_data : register(b0);

[[vk::binding(0, 1)]] StructuredBuffer<InstanceData> instance_data
    : register(t0, space0);
[[vk::binding(1, 1)]] StructuredBuffer<InstanceMeshletInfo>
    instance_meshlet_info : register(t1, space0);

[[vk::binding(0, 2)]] StructuredBuffer<InstanceMeshletCullInfo>
    instance_meshlet_cull_info : register(t2, space0);

[[vk::binding(1, 2)]] RWStructuredBuffer<InstanceMeshletCullInfo>
    recheck_cull_info : register(u0, space0);

[[vk::binding(0, 3)]] StructuredBuffer<MeshletDesc> meshlet_info_buffer
    : register(t0, space1);
[[vk::binding(1, 3)]] StructuredBuffer<MeshletBound> meshlet_bound_buffer
    : register(t1, space1);

// 0 for processed instance, 1 for processed meshlet
[[vk::binding(0, 4)]] RWByteAddressBuffer counters_buffer
    : register(u0, space1);
[[vk::binding(1, 4)]] RWStructuredBuffer<DrawCommandData> command_buffer
    : register(u1, space1);

[[vk::binding(0, 5)]] Texture2D<float> hiz_depth : register(t0, space2);
[[vk::binding(1, 5)]] SamplerState depth_sampler : register(s0, space0);

[[vk::push_constant]] ConstantBuffer<MeshletCullInput> input;

bool IsFrustumVisible(in MeshletBound bound, in float4x4 world,
                      in float scale) {
  float4 center = mul(world, float4(bound.center, 1.0f));
  float radius = bound.radius * scale;

  [unroll] for (uint i = 0; i < 6; i++) {
    if (dot(center, cull_data.planes[i]) + radius < 0) {
      return false;
    }
  }
  return true;
}

bool IsOcclusionVisible(in MeshletBound bound, in float4x4 world,
                        in float r_scale, in float4x4 vp, uint meshlet_id,
                        uint instance_id) {
  float4 center = mul(vp, mul(world, float4(bound.center, 1.0f)));
  float z = center.w;
  center /= center.w;
  // get center relative radius
  float world_radius =
      bound.radius * r_scale; // assume this is view space radius too

  // cross near plane, and it has passed frustum test
  if (z - world_radius < cull_data.near_plane || z <= 0.f) {
    return true;
  }

  float inv_tan_half_fov = cull_data.inv_tan_half_fov;
  float inv_tan_half_fov_x_aspect = inv_tan_half_fov / cull_data.aspect_ratio;
  float clip_y =
      world_radius * sqrt(inv_tan_half_fov * inv_tan_half_fov + 1.f) / z;
  float clip_x =
      world_radius *
      sqrt(inv_tan_half_fov_x_aspect * inv_tan_half_fov_x_aspect + 1.f) / z;

  float4 rect = float4(center.xy - float2(clip_x, clip_y),
                       center.xy + float2(clip_x, clip_y));
  float min_z = mul(cull_data.proj, float4(0, 0, world_radius - z, 1.f)).z /
                (z - world_radius); // reverse depth

  rect = saturate(rect * 0.5f + 0.5f);                      // to [0, 1]
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
  hiz_mip =
      max(lower_extent.x, lower_extent.y) <= 2.01f ? lower_level : hiz_mip;

  float4 depth_quad =
      float4(hiz_depth.SampleLevel(depth_sampler, rect.xy, hiz_mip),
             hiz_depth.SampleLevel(depth_sampler, rect.zy, hiz_mip),
             hiz_depth.SampleLevel(depth_sampler, rect.xw, hiz_mip),
             hiz_depth.SampleLevel(depth_sampler, rect.zw, hiz_mip));

  depth_quad.xy = min(depth_quad.xy, depth_quad.zw);
  depth_quad.x = min(depth_quad.x, depth_quad.y);
  return min_z >= depth_quad.x;
}

[numthreads(64, 1, 1)] void prepass(uint3 dtid
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

  bool frustum_visible = IsFrustumVisible(bound, data.model2world, data.scale);
  bool occlude_visible = true;

  if (frustum_visible) {
    occlude_visible =
        IsOcclusionVisible(bound, data.model2world, data.scale,
                           cull_data.camera_data.prev_view_proj,
                           cull_info.meshlet_id, cull_info.instance_id);
  }

  bool need_recheck = frustum_visible && !occlude_visible;
  uint need_recheck_count = WaveActiveCountBits(need_recheck);
  uint recheck_id_offset = 0;
  if (WaveIsFirstLane()) {
    counters_buffer.InterlockedAdd(input.recheck_counter_buffer_offset,
                                   need_recheck_count, recheck_id_offset);
  }

  uint recheck_lane_offset = WavePrefixCountBits(need_recheck);
  recheck_id_offset =
      WaveReadLaneFirst(recheck_id_offset) + recheck_lane_offset;

  if (need_recheck) {
    recheck_cull_info[recheck_id_offset] = cull_info;
    return;
  }

  bool visible = frustum_visible && occlude_visible;

  uint wave_meshlet_count = WaveActiveCountBits(visible);
  uint cmd_offset = 0;
  if (WaveIsFirstLane()) {
    counters_buffer.InterlockedAdd(input.draw_cmd_buffer_offset,
                                   wave_meshlet_count, cmd_offset);
  }
  cmd_offset = WaveReadLaneFirst(cmd_offset);
  uint lane_offset = WavePrefixCountBits(visible);
  cmd_offset += lane_offset;

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

    [numthreads(64, 1, 1)] void recheck_pass(uint3 dtid
                                             : SV_DispatchThreadID) {

  uint total_count =
      counters_buffer.Load<uint>(input.recheck_counter_buffer_offset);
  if (dtid.x >= total_count) {
    return;
  }

  InstanceMeshletCullInfo cull_info = instance_meshlet_cull_info[dtid.x];
  InstanceData data = instance_data[cull_info.instance_id];

  MeshletBound bound = meshlet_bound_buffer[cull_info.meshlet_id];
  bool visible = IsOcclusionVisible(
      bound, data.model2world, data.scale, cull_data.camera_data.view_proj,
      cull_info.meshlet_id, cull_info.instance_id);

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