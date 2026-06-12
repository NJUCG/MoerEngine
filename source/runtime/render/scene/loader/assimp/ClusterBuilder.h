#pragma once

#include <assimp/mesh.h>

#include "misc/BoundingBox.h"

namespace Moer::assimp {

// 这里先直接与 Assimp 深度耦合，降低接入复杂度
// TODO: 如果未来确实出现 Assimp 之外的稳定输入来源，再考虑将 Builder 从 Assimp 语义中解耦

struct ImportedMeshData {
    // 该指针即为 ai_scene->mMeshes[source_mesh_index]
    const aiMesh* source_mesh = nullptr;

    // 展开后的index数据
    Array<uint32> flattened_indices;

    // 在assimp原数组(ai_scene->mMeshes)中的index
    uint32 source_mesh_index = 0;
};

// 当前先固定一组默认参数，后续如果需要调优再向外暴露
struct ClusterBuildConfig {
    size_t max_vertices  = 64;
    size_t max_triangles = 124;
    float  cone_weight   = 0.0f;
};

// ClusterBuilder 只负责 cluster 数据构建，不负责 scene wiring
struct ClusterData {
    Array<float3> positions;

    Array<uint32> packed_normals;
    Array<uint32> packed_tangents;
    Array<float2> texcoord0;

    Array<uint32> indices;

    Box3D aabb = Box3D();
};

struct ClusterGroupData {
    float3 simplified_center = float3(0.f, 0.f, 0.f);
    float  simplified_radius = 0.f;
    float  simplified_error  = 0.f;
    int    depth             = 0; // DAG 层级（0=leaf, 1+=简化层级）
};

struct ClusterBuildResult {
    // 所有 LOD 层级的 cluster（叶子在前，按 depth 顺序排列）
    Array<ClusterData> clusters;

    // 每个 cluster 对应的 LOD group 信息
    Array<int> cluster_group_ids;      // cluster → group index
    Array<int> cluster_refined_ids;    // cluster → refined group index (-1 for leaf)

    // Group 数据（用于运行时 LOD 选择）
    Array<ClusterGroupData> groups;

    // 每个 group 的父 group ID（即在 DAG 中替代该 group 的更粗层级 group），
    // -1 表示该 group 是 DAG 根节点（最粗层级，无法被替代）
    Array<int> group_parent_ids;

    // 叶子 cluster 数量（clusters[0..num_leaf_clusters) 为叶子）
    uint32 num_leaf_clusters = 0;
};

class ClusterBuilder {
public:
    ClusterBuilder() = default;

    explicit ClusterBuilder(const ClusterBuildConfig& config);

    // 基于一个 Assimp mesh 的输入数据构建 cluster 结果
    ClusterBuildResult Build(const ImportedMeshData& input) const;

private:
    ClusterBuildConfig m_config{};
};

} // namespace Moer::assimp