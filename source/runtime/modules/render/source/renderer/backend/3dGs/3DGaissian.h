// #pragma once
// #include "math/Matrix.h"
// #include "shader/Shader.h"

// using SH = float[48];

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(Vertex)
// DEFINE_SHADER_PARAM(Moer::Vector4f, position)
// DEFINE_SHADER_PARAM(Moer::Vector4f, scale_opacity)
// DEFINE_SHADER_PARAM(Moer::Vector4f, rotation)
// DEFINE_SHADER_PARAM(SH, sh)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(VertexAttribute)
// DEFINE_SHADER_PARAM(Moer::Vector4f, conic_opacity)
// DEFINE_SHADER_PARAM(Moer::Vector4f, color_radii)
// DEFINE_SHADER_PARAM(Moer::Vector4ui, aabb)
// DEFINE_SHADER_PARAM(Moer::Vector2f, uv)
// DEFINE_SHADER_PARAM(float, depth)
// DEFINE_SHADER_PARAM(uint32_t, magic)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(Params)
// DEFINE_SHADER_PARAM(Moer::Vector4f, camera_position)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, proj_mat)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view_mat)
// DEFINE_SHADER_PARAM(uint32_t, width)
// DEFINE_SHADER_PARAM(uint32_t, height)
// DEFINE_SHADER_PARAM(float, tan_fovx)
// DEFINE_SHADER_PARAM(float, tan_fovy)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(RenderParameters)
// DEFINE_SHADER_PARAM(uint32_t, width)
// DEFINE_SHADER_PARAM(uint32_t, height)
// END_SHADER_CONSTANT_STRUCT_DEFINITION(RenderParameters)

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(TileBoundaryParameters)
// DEFINE_SHADER_PARAM(uint32_t, num_instances)
// END_SHADER_CONSTANT_STRUCT_DEFINITION(TileBoundaryParameters)

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(PrecompCov3dParameters)
// DEFINE_SHADER_PARAM(float, scale_factor)
// END_SHADER_CONSTANT_STRUCT_DEFINITION(PrecompCov3dParameters)

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(PrefixSumParameters)
// DEFINE_SHADER_PARAM(uint32_t, timestamp)
// END_SHADER_CONSTANT_STRUCT_DEFINITION(PrefixSumParameters)

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(PreProcessSortParameters)
// DEFINE_SHADER_PARAM(uint32_t, tileX)
// END_SHADER_CONSTANT_STRUCT_DEFINITION(PreProcessSortParameters)

// class PreprocessShader : public Shader {
//     DEFINE_SHADER_TYPE(PreprocessShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_CBV(ConstantBuffer, params)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<Vertex>, vertex_buffer)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<float>, cov3ds_buffer)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<VertexAttribute>, vertex_attribute_buffer)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint>, tile_overlap_buffer)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class PrefixSumShader : public Shader {
//     DEFINE_SHADER_TYPE(PrefixSumShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(PrefixSumParameters, params)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint>, src)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint>, dst)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class PreprocessSortShader : public Shader {
//     DEFINE_SHADER_TYPE(PreprocessSortShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(PreProcessSortParameters, params)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<VertexAttribute>, attr)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint32_t>, prefix_sum)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint64_t>, keys)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint>, payloads)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(SortParameters)
// DEFINE_SHADER_PARAM(uint32_t, g_num_instances)
// DEFINE_SHADER_PARAM(uint32_t, g_shift)
// DEFINE_SHADER_PARAM(uint32_t, g_num_workgroups)
// DEFINE_SHADER_PARAM(uint32_t, g_num_blocks_per_workgroup)
// END_SHADER_CONSTANT_STRUCT_DEFINITION(SortParameters)

// class RadixSortShader : public Shader {
//     DEFINE_SHADER_TYPE(RadixSortShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(SortParameters, params)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint64_t>, g_elements_in)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint64_t>, g_elements_out)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint>, g_payload_in)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint>, g_payload_out)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint>, g_histograms)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class SortHistShader : public Shader {
//     DEFINE_SHADER_TYPE(SortHistShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(SortParameters, params)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint64_t>, g_elements_in)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint>, g_histograms)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class SortCheckShader : public Shader {
//     DEFINE_SHADER_TYPE(SortCheckShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(SortParameters, params)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint64_t>, g_elements_in)
//     DEFINE_SHADER_PARAM_SRV(RWStructuredBuffer<uint>, g_histograms)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class TileBoundaryShader : public Shader {
//     DEFINE_SHADER_TYPE(TileBoundaryShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(TileBoundaryParameters, params)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint64_t>, sort_list)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint>, sort_out)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class RenderShader : public Shader {
//     DEFINE_SHADER_TYPE(RenderShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(RenderParameters, params)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<VertexAttribute>, vertex_attribute_buffer)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint>, boundaries)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint>, sorted_vertices)
//     DEFINE_SHADER_PARAM_UAV(RWTexture2D<float4>, output_image)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class PrecompCov3dShader : public Shader {
//     DEFINE_SHADER_TYPE(PrecompCov3dShader, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(PrecompCov3dParameters, params)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<Vertex>, vertex_buffer)
//     DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<float>, cov3ds_buffer)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };
