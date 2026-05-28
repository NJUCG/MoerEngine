#include "ClusterBuilder.h"

#include <array>

#include <meshoptimizer.h>

#include "shaderheaders/shared/utils/Packing.h"

namespace Moer::assimp {
namespace {

static_assert(sizeof(ai_real) == sizeof(float), "ClusterBuilder currently assumes ai_real is float");
static_assert(
    sizeof(aiVector3D) == sizeof(float) * 3,
    "ClusterBuilder expects aiVector3D to be tightly packed float3"
);

// 为 cluster 构建一个 AABB，供 CPrimitive 直接复用
Box3D BuildClusterAabb(const Array<float3>& positions) {
    Box3D aabb;

    for (const auto& position : positions) {
        aabb.Expand(position);
    }

    return aabb;
}

// 组装 meshoptimizer 所需的多 stream 输入
Array<meshopt_Stream> BuildStreams(const aiMesh* mesh) {
    Array<meshopt_Stream> streams;
    streams.reserve(4);

    if (mesh->HasPositions()) {
        streams.push_back(meshopt_Stream{mesh->mVertices, sizeof(aiVector3D), sizeof(aiVector3D)});
    }

    if (mesh->HasNormals()) {
        streams.push_back(meshopt_Stream{mesh->mNormals, sizeof(aiVector3D), sizeof(aiVector3D)});
    }

    if (mesh->HasTangentsAndBitangents()) {
        streams.push_back(meshopt_Stream{mesh->mTangents, sizeof(aiVector3D), sizeof(aiVector3D)});
    }

    if (mesh->HasTextureCoords(0)) {
        streams.push_back(meshopt_Stream{mesh->mTextureCoords[0], sizeof(float) * 2, sizeof(aiVector3D)});
    }

    return streams;
}

} // namespace

ClusterBuilder::ClusterBuilder(const ClusterBuildConfig& config) : m_config(config) {}

ClusterBuildResult ClusterBuilder::Build(const ImportedMeshData& input) const {
    ClusterBuildResult result{};

    const aiMesh* mesh = input.source_mesh;
    if (mesh == nullptr || !mesh->HasPositions() || input.flattened_indices.empty()) {
        return result;
    }

    const bool has_normals  = mesh->HasNormals();
    const bool has_tangents = mesh->HasTangentsAndBitangents();
    const bool has_uv0      = mesh->HasTextureCoords(0);

    Array<meshopt_Stream> streams = BuildStreams(mesh);

    Array<uint32> vertex_remap(mesh->mNumVertices);
    const size_t  unique_vertex_count = meshopt_generateVertexRemapMulti(
        vertex_remap.data(),
        input.flattened_indices.data(),
        input.flattened_indices.size(),
        mesh->mNumVertices,
        streams.data(),
        streams.size()
    );

    if (unique_vertex_count == 0) {
        return result;
    }

    Array<uint32> remapped_indices(input.flattened_indices.size());
    meshopt_remapIndexBuffer(
        remapped_indices.data(),
        input.flattened_indices.data(),
        input.flattened_indices.size(),
        vertex_remap.data()
    );
    meshopt_optimizeVertexCache(
        remapped_indices.data(), remapped_indices.data(), remapped_indices.size(), unique_vertex_count
    );

    Array<aiVector3D> unique_positions(unique_vertex_count);
    meshopt_remapVertexBuffer(
        unique_positions.data(), mesh->mVertices, mesh->mNumVertices, sizeof(aiVector3D), vertex_remap.data()
    );

    Array<aiVector3D> unique_normals;
    if (has_normals) {
        unique_normals.resize(unique_vertex_count);
        meshopt_remapVertexBuffer(
            unique_normals.data(), mesh->mNormals, mesh->mNumVertices, sizeof(aiVector3D), vertex_remap.data()
        );
    }

    Array<aiVector3D> unique_tangents;
    if (has_tangents) {
        unique_tangents.resize(unique_vertex_count);
        meshopt_remapVertexBuffer(
            unique_tangents.data(),
            mesh->mTangents,
            mesh->mNumVertices,
            sizeof(aiVector3D),
            vertex_remap.data()
        );
    }

    Array<aiVector3D> unique_uv0;
    if (has_uv0) {
        unique_uv0.resize(unique_vertex_count);
        meshopt_remapVertexBuffer(
            unique_uv0.data(),
            mesh->mTextureCoords[0],
            mesh->mNumVertices,
            sizeof(aiVector3D),
            vertex_remap.data()
        );
    }

    Array<uint32> vertex_fetch_remap(unique_vertex_count);
    const size_t  optimized_vertex_count = meshopt_optimizeVertexFetchRemap(
        vertex_fetch_remap.data(), remapped_indices.data(), remapped_indices.size(), unique_vertex_count
    );

    Array<uint32> optimized_indices(remapped_indices.size());
    meshopt_remapIndexBuffer(
        optimized_indices.data(), remapped_indices.data(), remapped_indices.size(), vertex_fetch_remap.data()
    );

    Array<aiVector3D> optimized_positions(optimized_vertex_count);
    meshopt_remapVertexBuffer(
        optimized_positions.data(),
        unique_positions.data(),
        unique_vertex_count,
        sizeof(aiVector3D),
        vertex_fetch_remap.data()
    );

    Array<aiVector3D> optimized_normals;
    if (has_normals) {
        optimized_normals.resize(optimized_vertex_count);
        meshopt_remapVertexBuffer(
            optimized_normals.data(),
            unique_normals.data(),
            unique_vertex_count,
            sizeof(aiVector3D),
            vertex_fetch_remap.data()
        );
    }

    Array<aiVector3D> optimized_tangents;
    if (has_tangents) {
        optimized_tangents.resize(optimized_vertex_count);
        meshopt_remapVertexBuffer(
            optimized_tangents.data(),
            unique_tangents.data(),
            unique_vertex_count,
            sizeof(aiVector3D),
            vertex_fetch_remap.data()
        );
    }

    Array<aiVector3D> optimized_uv0;
    if (has_uv0) {
        optimized_uv0.resize(optimized_vertex_count);
        meshopt_remapVertexBuffer(
            optimized_uv0.data(),
            unique_uv0.data(),
            unique_vertex_count,
            sizeof(aiVector3D),
            vertex_fetch_remap.data()
        );
    }

    const size_t max_meshlets =
        meshopt_buildMeshletsBound(optimized_indices.size(), m_config.max_vertices, m_config.max_triangles);

    if (max_meshlets == 0) {
        return result;
    }

    Array<meshopt_Meshlet> meshlets(max_meshlets);
    Array<unsigned int>    meshlet_vertices(max_meshlets * m_config.max_vertices);
    Array<unsigned char>   meshlet_triangles(max_meshlets * m_config.max_triangles * 3);

    const size_t meshlet_count = meshopt_buildMeshlets(
        meshlets.data(),
        meshlet_vertices.data(),
        meshlet_triangles.data(),
        optimized_indices.data(),
        optimized_indices.size(),
        reinterpret_cast<const float*>(optimized_positions.data()),
        optimized_positions.size(),
        sizeof(aiVector3D),
        m_config.max_vertices,
        m_config.max_triangles,
        m_config.cone_weight
    );

    result.clusters.reserve(meshlet_count);

    for (size_t meshlet_index = 0; meshlet_index < meshlet_count; ++meshlet_index) {
        const meshopt_Meshlet& meshlet = meshlets[meshlet_index];

        ClusterData cluster{};
        cluster.positions.reserve(meshlet.vertex_count);
        cluster.indices.reserve(meshlet.triangle_count * 3);

        if (has_normals) {
            cluster.packed_normals.reserve(meshlet.vertex_count);
        }
        if (has_tangents) {
            cluster.packed_tangents.reserve(meshlet.vertex_count);
        }
        if (has_uv0) {
            cluster.texcoord0.reserve(meshlet.vertex_count);
        }

        for (size_t local_vertex_index = 0; local_vertex_index < meshlet.vertex_count; ++local_vertex_index) {
            const uint32 global_vertex_index = meshlet_vertices[meshlet.vertex_offset + local_vertex_index];

            const aiVector3D& pos = optimized_positions[global_vertex_index];
            cluster.positions.emplace_back(pos.x, pos.y, pos.z);

            if (has_normals) {
                const aiVector3D& nor = optimized_normals[global_vertex_index];
                cluster.packed_normals.emplace_back(Pack_Normal(float3(nor.x, nor.y, nor.z)));
            }

            if (has_tangents) {
                const aiVector3D& tan = optimized_tangents[global_vertex_index];
                cluster.packed_tangents.emplace_back(Pack_Normal(float3(tan.x, tan.y, tan.z)));
            }

            if (has_uv0) {
                const aiVector3D& uv = optimized_uv0[global_vertex_index];
                cluster.texcoord0.emplace_back(uv.x, uv.y);
            }
        }

        for (size_t triangle_index = 0; triangle_index < meshlet.triangle_count * 3; ++triangle_index) {
            cluster.indices.emplace_back(meshlet_triangles[meshlet.triangle_offset + triangle_index]);
        }

        cluster.aabb = BuildClusterAabb(cluster.positions);
        result.clusters.emplace_back(std::move(cluster));
    }

    return result;
}

} // namespace Moer::assimp