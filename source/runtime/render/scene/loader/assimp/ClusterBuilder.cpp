#include "ClusterBuilder.h"

#include <vector>

#include <meshoptimizer.h>

#define MOER_CLUSTERLOD_IMPLEMENTATION
#include "MoerClusterLod.h"

#include "log/LogSystem.h"
#include "shaderheaders/shared/utils/Packing.h"

namespace Moer::assimp {
namespace {

static_assert(sizeof(ai_real) == sizeof(float), "ClusterBuilder currently assumes ai_real is float");
static_assert(
    sizeof(aiVector3D) == sizeof(float) * 3,
    "ClusterBuilder expects aiVector3D to be tightly packed float3"
);

Box3D BuildClusterAabb(const Array<float3>& positions) {
    Box3D aabb;

    for (const auto& position : positions) {
        aabb.Expand(position);
    }

    return aabb;
}

// 从原始 aiMesh 读取 cluster 的所有顶点属性
ClusterData ExtractClusterData(
    const clodCluster& cluster,
    const aiMesh*      mesh,
    bool               has_normals,
    bool               has_tangents,
    bool               has_uv0
) {
    ClusterData data{};
    data.positions.reserve(cluster.vertex_count);
    data.indices.reserve(cluster.index_count);

    Array<unsigned int>  local_vertices(cluster.vertex_count);
    Array<unsigned char> local_triangles(cluster.index_count);
    clodLocalIndices(local_vertices.data(), local_triangles.data(), cluster.indices, cluster.index_count);

    for (size_t i = 0; i < cluster.vertex_count; ++i) {
        const uint32 global_idx = local_vertices[i];

        const aiVector3D& pos = mesh->mVertices[global_idx];
        data.positions.emplace_back(pos.x, pos.y, pos.z);

        if (has_normals) {
            const aiVector3D& nor = mesh->mNormals[global_idx];
            data.packed_normals.emplace_back(Pack_Normal(float3(nor.x, nor.y, nor.z)));
        }
        if (has_tangents) {
            const aiVector3D& tan = mesh->mTangents[global_idx];
            data.packed_tangents.emplace_back(Pack_Normal(float3(tan.x, tan.y, tan.z)));
        }
        if (has_uv0) {
            const aiVector3D& uv = mesh->mTextureCoords[0][global_idx];
            data.texcoord0.emplace_back(uv.x, uv.y);
        }
    }

    for (size_t i = 0; i < cluster.index_count; ++i)
        data.indices.emplace_back(static_cast<uint32>(local_triangles[i]));

    data.aabb = BuildClusterAabb(data.positions);
    return data;
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

    // 配置 clodBuild
    clodConfig config = clodDefaultConfig(m_config.max_triangles);

    // 构造逐顶点属性数组 [nx, ny, nz, u, v]，用于属性感知简化决策
    static constexpr size_t ATTR_COUNT = 5; // normal(3) + uv(2)
    Array<float> vertex_attrs(mesh->mNumVertices * ATTR_COUNT, 0.f);
    float attr_weights[ATTR_COUNT] = {0.5f, 0.5f, 0.5f, 0.2f, 0.2f};

    for (uint32 i = 0; i < mesh->mNumVertices; ++i) {
        float* dst = &vertex_attrs[i * ATTR_COUNT];
        if (has_normals) {
            dst[0] = mesh->mNormals[i].x;
            dst[1] = mesh->mNormals[i].y;
            dst[2] = mesh->mNormals[i].z;
        }
        if (has_uv0) {
            dst[3] = mesh->mTextureCoords[0][i].x;
            dst[4] = mesh->mTextureCoords[0][i].y;
        }
    }

    clodMesh clod_mesh{};
    clod_mesh.indices                  = input.flattened_indices.data();
    clod_mesh.index_count              = input.flattened_indices.size();
    clod_mesh.vertex_count             = mesh->mNumVertices;
    clod_mesh.vertex_positions         = reinterpret_cast<const float*>(mesh->mVertices);
    clod_mesh.vertex_positions_stride  = sizeof(aiVector3D);
    clod_mesh.vertex_attributes        = vertex_attrs.data();
    clod_mesh.vertex_attributes_stride = ATTR_COUNT * sizeof(float);
    clod_mesh.attribute_weights        = attr_weights;
    clod_mesh.attribute_count          = ATTR_COUNT;
    clod_mesh.attribute_protect_mask   = (1u << 3) | (1u << 4); // UV seam 保护

    // 收集 clodBuild 的输出
    struct BuildContext {
        ClusterBuildResult* result;
        const aiMesh*       mesh;
        bool                has_normals;
        bool                has_tangents;
        bool                has_uv0;
        int                 current_group_id;
        bool                in_leaf_phase;
    };

    BuildContext ctx{
        &result, mesh,
        has_normals, has_tangents, has_uv0,
        0, true
    };

    clodBuild(config, clod_mesh, [&](clodGroup group, const clodCluster* clusters, size_t count) -> int {
        const int group_id = ctx.current_group_id++;

        ctx.result->groups.push_back(ClusterGroupData{
            .simplified_center = float3(
                group.simplified.center[0],
                group.simplified.center[1],
                group.simplified.center[2]
            ),
            .simplified_radius = group.simplified.radius,
            .simplified_error  = group.simplified.error,
            .depth             = group.depth
        });

        if (ctx.in_leaf_phase && count > 0 && clusters[0].refined != -1) {
            ctx.in_leaf_phase = false;
            ctx.result->num_leaf_clusters = static_cast<uint32>(ctx.result->clusters.size());
        }

        for (size_t i = 0; i < count; ++i) {
            ClusterData cluster_data = ExtractClusterData(
                clusters[i], ctx.mesh, ctx.has_normals, ctx.has_tangents, ctx.has_uv0);

            ctx.result->clusters.push_back(std::move(cluster_data));
            ctx.result->cluster_group_ids.push_back(group_id);
            ctx.result->cluster_refined_ids.push_back(clusters[i].refined);
        }

        return group_id;
    });

    // 如果所有 cluster 都是叶子（mesh 太小，无法简化），num_leaf_clusters = 全部
    if (ctx.in_leaf_phase) {
        result.num_leaf_clusters = static_cast<uint32>(result.clusters.size());
    }

    // 计算每个 group 的 parent_group_id（DAG 中替代该 group 的更粗层级 group）
    // 非叶子 cluster C 的 refined_id 指向被它替代的子 group。
    // 所以：parent_of[C.refined_id] = C.group_id
    result.group_parent_ids.resize(result.groups.size(), -1);
    for (size_t i = result.num_leaf_clusters; i < result.clusters.size(); ++i) {
        const int child_group  = result.cluster_refined_ids[i];
        const int parent_group = result.cluster_group_ids[i];
        if (child_group >= 0 && child_group < static_cast<int>(result.groups.size())) {
            result.group_parent_ids[child_group] = parent_group;
        }
    }

    // LOG_DEBUG(
    //     "[ClusterBuilder] mesh_vertices={}, mesh_indices={}, total_clusters={}, leaf_clusters={}, "
    //     "non_leaf_clusters={}, groups={}",
    //     mesh->mNumVertices,
    //     input.flattened_indices.size(),
    //     result.clusters.size(),
    //     result.num_leaf_clusters,
    //     result.clusters.size() - result.num_leaf_clusters,
    //     result.groups.size()
    // );

    // 验证叶子 cluster 的 refined_id 都为 -1
    for (uint32 i = 0; i < result.num_leaf_clusters; ++i) {
        if (result.cluster_refined_ids[i] != -1) {
            LOG_ERROR("[ClusterBuilder] BUG: leaf cluster[{}] has refined_id={}, expected -1",
                      i, result.cluster_refined_ids[i]);
        }
    }
    // 验证非叶子 cluster 的 refined_id 都 >= 0
    for (size_t i = result.num_leaf_clusters; i < result.clusters.size(); ++i) {
        if (result.cluster_refined_ids[i] < 0) {
            LOG_ERROR("[ClusterBuilder] BUG: non-leaf cluster[{}] has refined_id={}, expected >= 0",
                      i, result.cluster_refined_ids[i]);
        }
    }

    return result;
}

} // namespace Moer::assimp
