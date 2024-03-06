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
};

struct DrawCommandData {
  uint index_count;
  uint instance_count;
  uint first_index;
  uint vertex_offset;
  uint first_instance;
};

struct MeshletDesc {
  uint vertex_offset;
  uint vertex_count;
  uint index_offset;
  uint index_count;
};

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
};
#endif