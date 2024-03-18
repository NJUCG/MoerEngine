#ifndef FRAMEWORK_COMMON_HLSL
#define FRAMEWORK_COMMON_HLSL
struct CameraData {
  float4x4 view;
  float4x4 view_proj;
  float4x4 prev_view_proj;
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
  float3 cone_axis;
  float cone_angle;
};

#endif