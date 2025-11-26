// #ifndef MOER_DEFERRED_CULL_H
// #define MOER_DEFERRED_CULL_H
// #include "math/Matrix.h"
// #include "misc/MacroUtils.h"
// #include "shader/Shader.h"

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CameraData)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view_proj)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, prev_view_proj)
// DEFINE_SHADER_PARAM(Moer::Vector4f, camera_pos)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()

// class TestDeferredTriangleShaderVert : public Shader {
//     DEFINE_SHADER_TYPE(TestDeferredTriangleShaderVert, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class TestDeferredTriangleShaderFrag : public Shader {
//     DEFINE_SHADER_TYPE(TestDeferredTriangleShaderFrag, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     // DEFINE_SHADER_PARAM_SAMPLER(SamplerState, defaultSampler)
//     // DEFINE_SHADER_PARAM_SRV(Texture2D, baseColorMap)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// struct ALIGN_BEGIN(256) CameraCullData {
//     CameraData       camera_data;
//     Moer::Matrix4x4f proj;
//     Moer::Vector4f   frustum_planes[6];
//     float            near_plane;
//     float            far_plane;
//     float            inv_tan_half_fov;
//     float            aspect_ratio;
// } ALIGN_END(256);

// struct ALIGN_BEGIN(256) VirtualView {
//     Moer::Matrix4x4f view;
//     Moer::Matrix4x4f view_proj;
//     Moer::Matrix4x4f prev_view_proj;
//     Moer::Matrix4x4f proj;
//     Moer::Vector4f   planes[6];
//     Moer::Vector3f   pos;
//     float            nearz;
//     Moer::Vector3f   bound_center;
//     float            aspect_ratio;
//     Moer::Vector3f   bound_extent;
//     float            inv_tan_half_fov;
// } ALIGN_END(256);
// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CullInstanceInput)
// DEFINE_SHADER_PARAM(uint32_t, instance_count)
// DEFINE_SHADER_PARAM(uint32_t, meshlet_count_offset)
// DEFINE_SHADER_PARAM(Moer::Vector2f, hiz_factor)
// DEFINE_SHADER_PARAM(float, hiz_depth)
// DEFINE_SHADER_PARAM(uint32_t, recheck_counter_buffer_offset)

// END_SHADER_CONSTANT_STRUCT_DEFINITION(CullInstanceInput)
// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CullMeshletInput)
// DEFINE_SHADER_PARAM(uint32_t, meshlet_count_offset)
// DEFINE_SHADER_PARAM(uint32_t, draw_count_offset)
// DEFINE_SHADER_PARAM(Moer::Vector2f, hiz_factor)
// DEFINE_SHADER_PARAM(float, hiz_depth)
// DEFINE_SHADER_PARAM(uint32_t, recheck_counter_buffer_offset)
// END_SHADER_CONSTANT_STRUCT_DEFINITION(CullMeshletInput)

// MUTATION_BOOL(PrePass);

// class CullInstancePrePassShader : public Shader {
// public:
//     DEFINE_SHADER_TYPE(CullInstancePrePassShader, Global, RENDER_API, ...)
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(CullInstanceInput, input)
//     DEFINE_SHADER_PARAM_CBV(ConstantBuffer<VirtualView>, views)
//     // DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)

//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceMeshInfo>, instance_meshlet_info)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint64_t>, instance_meshlet_cull_info)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint32_t>, recheck_instance_id)

//     DEFINE_SHADER_PARAM_UAV(RWByteAddressBuffer, counters_buffer)
//     DEFINE_SHADER_PARAM_SRV(Texture2D<float>, hiz_depth)
//     DEFINE_SHADER_PARAM_SAMPLER(SamplerState, depth_sampler)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class CullInstanceRecheckShader : public Shader {
// public:
//     DEFINE_SHADER_TYPE(CullInstanceRecheckShader, Global, RENDER_API, ...)
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(CullInstanceInput, input)
//     DEFINE_SHADER_PARAM_CBV(ConstantBuffer<VirtualView>, views)
//     // DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)

//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceMeshInfo>, instance_meshlet_info)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint64_t>, instance_meshlet_cull_info)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint32_t>, recheck_instances)

//     DEFINE_SHADER_PARAM_UAV(RWByteAddressBuffer, counters_buffer)
//     DEFINE_SHADER_PARAM_SRV(Texture2D<float>, hiz_depth)
//     DEFINE_SHADER_PARAM_SAMPLER(SamplerState, depth_sampler)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class CullMeshletPrepassShader : public Shader {
// public:
//     DEFINE_SHADER_TYPE(CullMeshletPrepassShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(CullMeshletInput, input)

//     DEFINE_SHADER_PARAM_CBV(ConstantBuffer<VirtualView>, views)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MeshletDesc>, meshlet_info_buffer)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MeshletBound>, meshlet_bound_buffer)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceMeshInfo>, instance_meshlet_info)

//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint64_t>, instance_meshlet_cull_info)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint64_t>, recheck_cull_info)

//     DEFINE_SHADER_PARAM_UAV(RWByteAddressBuffer, counters_buffer)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<DrawCommand>, command_buffer)
//     DEFINE_SHADER_PARAM_SRV(Texture2D<float>, hiz_depth)
//     DEFINE_SHADER_PARAM_SAMPLER(SamplerState, depth_sampler)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class CullMeshletRecheckShader : public Shader {
// public:
//     DEFINE_SHADER_TYPE(CullMeshletRecheckShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(CullMeshletInput, input)

//     DEFINE_SHADER_PARAM_CBV(ConstantBuffer<VirtualView>, views)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MeshletDesc>, meshlet_info_buffer)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MeshletBound>, meshlet_bound_buffer)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceMeshInfo>, instance_meshlet_info)

//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint64_t>, instance_meshlet_cull_info)

//     DEFINE_SHADER_PARAM_UAV(RWByteAddressBuffer, counters_buffer)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<DrawCommand>, command_buffer)
//     DEFINE_SHADER_PARAM_SRV(Texture2D<float>, hiz_depth)
//     DEFINE_SHADER_PARAM_SAMPLER(SamplerState, depth_sampler)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class TestGBufferShaderVert : public Shader {
//     DEFINE_SHADER_TYPE(TestGBufferShaderVert, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class TestGBufferShaderFrag : public Shader {
//     DEFINE_SHADER_TYPE(TestGBufferShaderFrag, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//     // DEFINE_SHADER_PARAM_SAMPLER(SamplerState, defaultSampler)
//     // DEFINE_SHADER_PARAM_SRV(Texture2D, baseColorMap)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// // IMPLEMENT_SHADER_TYPE(CullInstanceShader, "meshdebug/CullInstance.hlsl", "main", ST_COMPUTE);
// // IMPLEMENT_SHADER_TYPE(CullMeshletShader, "meshdebug/CullMeshlet.hlsl", "main", ST_COMPUTE);

// #endif