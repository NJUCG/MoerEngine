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

struct ClusterBuildResult {
    Array<ClusterData> clusters;
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