#include "framework/Common.hlsl"

#define FRUSTUM_CULLING_ENABLED 1
#define BACKFACE_CULLING_ENABLED 1
#define OCCULSION_CULLING_ENABLED 1

struct MeshletDesc {
  uint vertex_offset;
  uint index_offset;
  uint packed_data;

  uint GetPrimitiveCount() { return packed_data & 0xFF; }
  uint GetVertexCount() { return (packed_data >> 8) & 0xFF; }
  uint GetIndexCount() { return (packed_data & 0xFF) * 3; }
};

struct MeshletBound {
  float3 center;
  float radius;
  float3 cone_axis;
  float cone_angle;
};

struct DrawCommandData {
  uint32_t index_count;
  uint32_t instance_count;
  uint32_t first_index;
  int32_t vertex_offset;
  uint32_t first_instance;
};

[[vk::push_constant]] ConstantBuffer<CameraData> camera_data;

// StructuredBuffer<InstanceData> InstanceData;

StructuredBuffer<MeshletDesc> meshlet_info_buffer : register(t0, space0);

StructuredBuffer<MeshletBound> meshlet_bound_buffer : register(t1, space0);

RWByteAddressBuffer draw_count_buffer : register(u0, space1);

RWByteAddressBuffer draw_indirect_buffer : register(u1, space1);

// Function to check if a vertex is inside the frustum
bool IsMeshletInsideFrustum(in MeshletBound meshlet_info) {
  // Frustum culling logic goes here
  // Return true if the vertex is inside the frustum, false otherwise
  return true;
}

// Function to check if a vertex is facing away from the camera
bool IsMeshletFacingAwayFromCamera(in MeshletBound meshlet_info) {
  // Backface culling logic goes here
  // Return true if the vertex is facing away from the camera, false otherwise
  return true;
}

bool IsMeshletVisible(in uint meshlet_id) {
  bool visible = true;

  MeshletBound meshlet_info = meshlet_bound_buffer[meshlet_id];

#if FRUSTUM_CULLING_ENABLED
  visible &= !IsMeshletInsideFrustum(meshlet_info);
#endif

  // Check if the vertex is facing away from the camera
#if BACKFACE_CULLING_ENABLED
  visible &= !IsMeshletFacingAwayFromCamera(meshlet_info);
#endif
  return visible;
}

// Compute shader entry point
[numthreads(64, 1, 1)] void main(uint3 dispatchThreadId
                                 : SV_DispatchThreadID) {
  // Get the index of the vertex to process
  uint meshlet_id = dispatchThreadId.x;

  MeshletDesc meshlet_desc = meshlet_info_buffer[meshlet_id];

  bool visible = IsMeshletVisible(meshlet_id);
  uint meshlet_count = WaveActiveCountBits(visible);
  uint lane_offset = WavePrefixCountBits(visible);

  uint cmd_offset;
  if (WaveIsFirstLane()) {
    draw_count_buffer.InterlockedAdd(0, meshlet_count, cmd_offset);
  }
  cmd_offset = WaveReadLaneFirst(cmd_offset);
  cmd_offset += lane_offset;

  if (visible) {
    DrawCommandData cmd;

    cmd.index_count = meshlet_desc.GetIndexCount();
    cmd.instance_count = 1;
    cmd.first_index = meshlet_desc.index_offset;
    cmd.vertex_offset = meshlet_desc.vertex_offset;
    cmd.first_instance = 0;
    draw_indirect_buffer.Store(cmd_offset * sizeof(DrawCommandData), cmd);
  }
}
