#ifndef FRAMEWORK_COMMON_HLSL
#define FRAMEWORK_COMMON_HLSL
struct CameraData {
  float4x4 view;
  float4x4 view_proj;
  float4x4 prev_view_proj;
  float4 camera_pos;
};

struct InstanceData {
  float4x4 model2world;
  float4x4 inv_model2world;
  float scale;
  float padding;
  uint material_id;
  uint material_type;
};

static const float3 corners[8] = {
    float3(-1, -1, -1), float3(-1, 1, -1), float3(1, 1, -1), float3(1, -1, -1),
    float3(-1, -1, 1),  float3(-1, 1, 1),  float3(1, 1, 1),  float3(1, -1, 1),
};
struct VirtualView {
  float4x4 view;
  float4x4 view_proj;
  float4x4 prev_view_proj;
  float4x4 proj;
  float4 planes[6];
  float3 pos;
  float nearz;
  float3 bound_center;
  float aspect_ratio;
  float3 bound_extent;
  float inv_tan_half_fov;
};

struct InstanceMeshletInfo {
  float3 center;
  uint vertex_offset;
  float3 extent;
  uint vertex_count;
  uint index_offset;
  uint index_count;
  uint meshlet_offset;
  uint meshlet_count;
};
struct InstanceMeshletCullInfo {
  uint meshlet_id;
  uint instance_id;
  uint padding;
  uint padding2;
};

struct DrawCommandData {
  uint index_count;
  uint instance_count;
  uint first_index;
  uint vertex_offset;
  uint first_instance;
  uint padding;
  uint padding2;
  uint padding3;
};

struct MeshletDesc {
  uint vertex_offset;
  uint vertex_count;
  uint index_offset;
  uint index_count;
};
static uint cull_thread_size = 64;
static uint cull_thread_bits = 6;

uint IncrementDispatchCounter(in RWByteAddressBuffer target, uint offset,
                              uint cnt, uint prev_cnt) {
  uint result = 0;
  uint sum_cnt = prev_cnt + cnt;
  uint dispatch_cnt_inc =
      ((sum_cnt + cull_thread_size - 1) >> cull_thread_bits) -
      ((prev_cnt + cull_thread_size - 1) >> cull_thread_bits);

  bool b_inc = prev_cnt == 0 || dispatch_cnt_inc > 0;
  bool first_inc = prev_cnt == 0 && dispatch_cnt_inc > 0;
  dispatch_cnt_inc = first_inc ? dispatch_cnt_inc + 1 : dispatch_cnt_inc;
  if (b_inc) {
    target.InterlockedAdd(offset, dispatch_cnt_inc, result);
    if (first_inc) {
      uint temp = 0;
      target.InterlockedAdd(offset + 4, 1, temp);
      target.InterlockedAdd(offset + 8, 1, temp);
    }
  }
  return result;
}

uint IncrementDispatchCounter(in StructuredBuffer<uint> target, uint offset,
                              uint cnt, uint prev_cnt) {
  uint result = 0;
  uint sum_cnt = prev_cnt + cnt;
  uint dispatch_cnt_inc =
      ((sum_cnt + cull_thread_size - 1) >> cull_thread_bits) -
      ((prev_cnt + cull_thread_size - 1) >> cull_thread_bits);

  bool b_inc = prev_cnt == 0 || dispatch_cnt_inc > 0;
  bool first_inc = prev_cnt == 0 && dispatch_cnt_inc > 0;
  dispatch_cnt_inc = first_inc ? dispatch_cnt_inc + 1 : dispatch_cnt_inc;
  if (b_inc) {
    target.InterlockedAdd(offset, dispatch_cnt_inc, result);
    if (first_inc) {
      uint temp = 0;
      target.InterlockedAdd(offset + 4, 1, temp);
      target.InterlockedAdd(offset + 8, 1, temp);
    }
  }
  return result;
}
struct MeshletBound {
  float3 center;
  float radius;
  /* normal cone axis and cutoff, stored in 8-bit SNORM format; decode using
   * x/127.0 */
  uint packed_cut_off;
  /* dot(center - camera_position, cone_axis) >= cone_cutoff * length(center -
   * camera_position) + radius
   */
  void DecodeCutOff(out float3 cone_axis, out float cut_off) {
    cone_axis =
        float3(float(packed_cut_off & 0x000000FF) * 0.0078740157,
               float((packed_cut_off & 0x0000FF00) >> 8) * 0.0078740157,
               float((packed_cut_off & 0x00FF0000) >> 16) * 0.0078740157);
    cut_off = float((packed_cut_off & 0xFF000000) >> 24) * 0.0078740157;
  }
  uint padding0;
  uint padding1;
  uint padding2;
};

#endif