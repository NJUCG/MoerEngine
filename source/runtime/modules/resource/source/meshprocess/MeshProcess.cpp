#include "meshprocess/MeshProcessor.h"
#include <algorithm>
#include <meshoptimizer.h>

namespace Moer {
    void MeshProcessor::GenerateMeshlets(const MeshProcessInput& input, MeshProcessOutput& output) {

        const uint32_t origin_indices_count = input.index_count;
        const uint32_t origin_vertex_count  = input.vertex_count;
        const uint32_t stride               = input.vertex_stride;

        uint32_t stride_float = stride / sizeof(float);
        //optimize here
        Moer::Array<uint32_t> remap(origin_vertex_count);

        Moer::Array<uint32_t> target_indices(origin_indices_count);

        size_t target_vertex_count = meshopt_generateVertexRemap(&remap[0],
                                                                 input.index_data,
                                                                 origin_indices_count,
                                                                 input.vertex_data,
                                                                 origin_vertex_count,
                                                                 stride);

        Moer::Array<float> target_vertices(target_vertex_count * stride_float);

        meshopt_remapIndexBuffer(&target_indices[0],
                                 input.index_data,
                                 origin_indices_count,
                                 &remap[0]);

        meshopt_remapVertexBuffer(&target_vertices[0],
                                  input.vertex_data,
                                  origin_vertex_count,
                                  stride,
                                  &remap[0]);

        meshopt_optimizeVertexCache(&target_indices[0],
                                    target_indices.data(),
                                    origin_indices_count,
                                    target_vertex_count);

        meshopt_optimizeVertexFetch(&target_vertices[0],
                                    &target_indices[0],
                                    target_indices.size(),
                                    target_vertices.data(),
                                    target_vertex_count,
                                    stride);

        const size_t max_vertices  = 64;
        const size_t max_triangles = 124;
        const float  cone_weight   = 0.0f;

        size_t max_meshlets = meshopt_buildMeshletsBound(target_indices.size(), max_vertices, max_triangles);

        Moer::Array<meshopt_Meshlet> meshlets(max_meshlets);

        Moer::Array<unsigned int> meshlet_vertices(max_meshlets * max_vertices);
        Moer::Array<uint8_t>      meshlet_triangles(max_meshlets * max_triangles * 3);
        meshlet_triangles.resize(max_meshlets * max_triangles * 3);

        size_t meshlet_count = meshopt_buildMeshlets(meshlets.data(),
                                                     &meshlet_vertices[0],
                                                     &meshlet_triangles[0],
                                                     target_indices.data(),
                                                     target_indices.size(),
                                                     &target_vertices[0],
                                                     target_vertex_count,
                                                     stride,
                                                     max_vertices,
                                                     max_triangles,
                                                     cone_weight);

        const meshopt_Meshlet& last = meshlets[meshlet_count - 1];

        meshlet_vertices.resize(last.vertex_offset + last.vertex_count);
        meshlet_triangles.resize(last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3));

        meshlets.resize(meshlet_count);

        output.meshlets.resize(meshlet_count);
        output.primitive_indices.resize(meshlet_triangles.size());
        for (size_t i = 0; i < meshlet_triangles.size(); i++) {
            output.primitive_indices[i] = meshlet_triangles[i];
        }
        std::for_each(meshlets.begin(), meshlets.end(), [&](const meshopt_Meshlet& m) {
            uint32_t index                          = &m - &meshlets[0];
            output.meshlets[index].vertex_offset    = m.vertex_offset;
            output.meshlets[index].vertex_count     = m.vertex_count;
            output.meshlets[index].primitive_offset = m.triangle_offset;
            output.meshlets[index].primitive_count  = m.triangle_count * 3;
        });

        //rebuild original vertices and indices
        Moer::Array<float>& regenerated_vertices = output.meshlet_vertex_data;
        regenerated_vertices.resize(meshlet_vertices.size() * stride_float);

        for (uint32_t i = 0; i < meshlet_vertices.size(); i++) {
            auto* const copy_src = reinterpret_cast<float*>(target_vertices.data() + meshlet_vertices[i] * stride_float);
            std::copy(copy_src, copy_src + stride_float, regenerated_vertices.data() + i * stride_float);
        }

        Moer::Array<Render::MeshletBound>& meshlet_bounds = output.meshlet_bounds;
        meshlet_bounds.resize(meshlet_count);

        std::for_each(meshlets.begin(), meshlets.end(), [&](const meshopt_Meshlet& m) {
            uint32_t index = &m - &meshlets[0];

            auto&& meshlet_bound = meshopt_computeMeshletBounds(&meshlet_vertices[m.vertex_offset],
                                                                &meshlet_triangles[m.triangle_offset],
                                                                m.triangle_count,
                                                                &target_vertices[0],
                                                                target_vertex_count,
                                                                stride);

            meshlet_bounds[index].center          = {meshlet_bound.center[0], meshlet_bound.center[1], meshlet_bound.center[2]};
            meshlet_bounds[index].radius          = meshlet_bound.radius;
            meshlet_bounds[index].cone_axis_s8[0] = meshlet_bound.cone_axis_s8[0];
            meshlet_bounds[index].cone_axis_s8[1] = meshlet_bound.cone_axis_s8[1];
            meshlet_bounds[index].cone_axis_s8[2] = meshlet_bound.cone_axis_s8[2];

            meshlet_bounds[index].cone_cutoff = meshlet_bound.cone_cutoff_s8;
        });

        // output.meshlet_vertex_data.swap(target_vertices);
        // output.primitive_indices.swap(target_indices);
        // output.vertex_indirection.swap(meshlet_vertices);
    }

    MeshProcessOutput MeshProcessor::GenerateMeshlets(const MeshProcessInput& input) {
        MeshProcessOutput output{};
        GenerateMeshlets(input, output);
        output.meshlet_vertex_data = Moer::Array<float>(static_cast<float*>(input.vertex_data), static_cast<float*>(input.vertex_data) + input.vertex_count * input.vertex_stride / 4);
        output.primitive_indices   = Moer::Array<uint32_t>(input.index_data, input.index_data + input.index_count);
        return std::move(output);
    }

    MeshProcessor::MeshProcessor() {
        meshopt_setAllocator(Memory::Malloc, Memory::Free);
    }
}// namespace Moer