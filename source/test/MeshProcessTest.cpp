#include <algorithm>
#include <metis.h>

#include "Core.h"
#include "assimp/Importer.hpp"
#include "assimp/mesh.h"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "assimp/vector3.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "math/Base.h"
#include "math/Constant.h"
#include "math/Function.h"

#include <meshoptimizer.h>

#include <ranges>

void MetisTest();
void InitTestEnv(const std::filesystem::path& workspace_path);
void ExitTestEnv();

int main(const int argc, const char** argv) {
    std::filesystem::path workspace_path = std::filesystem::current_path();
    InitTestEnv(workspace_path);
    MetisTest();
    ExitTestEnv();
}

struct MeshletDesc {
    uint32_t vertex_count;// number of vertices used
    uint32_t prim_count;  // number of primitives (triangles) used
    uint32_t vertex_begin;// offset into vertexIndices
    uint32_t prim_begin;  // offset into primitiveIndices
};

//information for culling
struct MeshletInfo{
    Moer::Vector3f center;
    Moer::Vector3f extent;
};

struct MoerMesh {
    Moer::Array<MeshletDesc> meshlets;

    Moer::Array<uint8_t> primitive_indices;//local triangle indices

    Moer::Array<uint32_t> vertex_indices;//unique original vertex indices
    Moer::Array<MeshletInfo> meshlet_info;
};

struct Vertex {
    Moer::Vector3f position;
    Moer::Vector3f normal;
    Moer::Vector3f tangent;
    Moer::Vector3f bitangent;
    Moer::Vector2f texture_coord;
};

struct OriginalMesh {
    Moer::Array<Vertex>   vertexs;
    Moer::Array<uint32_t> indices;
};

struct MoerMeshletOutputs {
    MoerMesh mesh;
};

struct MoerMeshletInputs {
    OriginalMesh original_mesh;
};

void GenerateMoerMeshletMesh(const MoerMeshletInputs& input, MoerMeshletOutputs& output);

void MetisTest() {

    Assimp::Importer importer;

    auto& config_manager = Moer::ConfigManager::GetInstance();

    auto           default_test_obj_path = config_manager.GetEditorResourcePath() / "default/scenes/sponza/models/walls-lib_2.obj";
    const aiScene* scene                 = importer.ReadFile(default_test_obj_path.string(), aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);
    auto*          meshes                = scene->mMeshes;
    auto           flag                  = scene->mFlags;
    if (flag & AI_SCENE_FLAGS_INCOMPLETE || !scene || !scene->mRootNode) {
        LOG_ERROR("Assimp load scene failed");
        return;
    }
    auto                      num_meshes = scene->mNumMeshes;
    Moer::Array<OriginalMesh> original_meshes(num_meshes);

    for (int i = 0; i < num_meshes; i++) {

        OriginalMesh& original_mesh = original_meshes[i];

        auto* mesh           = meshes[i];
        auto* faces          = mesh->mFaces;
        auto* vertices       = mesh->mVertices;
        auto* normals        = mesh->mNormals;
        auto* tangents       = mesh->mTangents;
        auto* bitangents     = mesh->mBitangents;
        auto* texture_coords = mesh->mTextureCoords[i];
        auto  num_faces      = mesh->mNumFaces;

        for (int j = 0; j < mesh->mNumVertices; j++) {
            auto& vertex        = vertices[j];
            auto& normal        = normals[j];
            auto& tangent       = tangents[j];
            auto& bitangent     = bitangents[j];
            auto& texture_coord = texture_coords[j];

            original_mesh.vertexs.push_back({Moer::Vector3f(vertex.x, vertex.y, vertex.z),
                                             Moer::Vector3f(normal.x, normal.y, normal.z),
                                             Moer::Vector3f(tangent.x, tangent.y, tangent.z),
                                             Moer::Vector3f(bitangent.x, bitangent.y, bitangent.z),
                                             Moer::Vector2f(texture_coord.x, texture_coord.y)});
        }

        for (int j = 0; j < num_faces; j++) {
            auto& face = faces[j];
            for (int k = 0; k < face.mNumIndices; k++) {
                original_mesh.indices.push_back(face.mIndices[k]);
            }
        }
    }

    Moer::Array<MoerMeshletOutputs> moer_meshes(original_meshes.size());
    for (auto& original_mesh : original_meshes) {
        MoerMeshletInputs inputs{original_mesh};
        auto&             output = moer_meshes[&original_mesh - &original_meshes[0]];
        GenerateMoerMeshletMesh(inputs, output);

        int meshlet_index = 0;
        for (const auto& meshlet : output.mesh.meshlets) {
            LOG_INFO("Meshlet id: {}, Meshlet vertex count: {}, Meshlet prim count: {}", meshlet_index, meshlet.vertex_count, meshlet.prim_count);
            meshlet_index++;
            assert(meshlet.prim_count == 64);
        }
    }
    LOG_INFO("Metis test done");
}

void InitTestEnv(const std::filesystem::path& workspace_path) {
    //core
    Moer::ConfigManager::GetInstance().Init(workspace_path);
    Moer::TaskSystem::Init();
    Moer::LogSystem::Init();
}

void ExitTestEnv() {
    Moer::TaskSystem::ShutDown();
}

void GenerateMoerMeshletMesh(const MoerMeshletInputs& input, MoerMeshletOutputs& output) {
    static decltype(METIS_PartGraphRecursive)* metis_patition_func = METIS_PartGraphRecursive;

    static decltype(METIS_MeshToDual)* metis_mesh_to_dual_func = METIS_MeshToDual;

    MoerMesh&           moer_mesh     = output.mesh;
    const OriginalMesh& original_mesh = input.original_mesh;

    auto GetOriginalIndice = [&](int offset) {
        if (offset < original_mesh.indices.size()) {
            return original_mesh.indices[offset];
        }
        return original_mesh.indices.back();
    };

    int num_faces = original_mesh.indices.size() / 3;

    if (num_faces % 64 != 0) {
        num_faces = (num_faces / 64 + 1) * 64;
    }

    int num_element = num_faces;
    int num_nodes   = num_faces * 3;

    Moer::Array<int32_t> eptr(num_element + 1);
    Moer::Array<int32_t> eind(num_nodes);//num indices

    constexpr int32_t num_node_per_element = 3;//triangle mesh
    int32_t           current_offset       = 0;
    for (int i = 0; i < num_element + 1; i++) {
        eptr[i] = current_offset;
        current_offset += num_node_per_element;
    }

    for (int i = 0; i < original_mesh.indices.size(); i++) {
        eind[i] = original_mesh.indices[i];
    }

    //two triangle is connected when they have two common nodes
    int num_common_node = 2;
    int num_flag        = 0;

    idx_t *xadj, *adjncy;//range and edges
    auto   r = metis_mesh_to_dual_func(
        &num_element,
        &num_nodes,
        eptr.data(),
        eind.data(),
        &num_common_node,
        &num_flag,
        &xadj,
        &adjncy);

    rstatus_et res          = (rstatus_et)r;
    int        n_conditions = 1;

    int     v_size    = 64;
    int*    vwgt      = nullptr;
    int*    adjwgt    = nullptr;
    int     num_parts = (num_faces + v_size - 1) / v_size;
    double* tpwgts    = nullptr;

    int              obj_val;
    std::vector<int> part(num_faces);
    res = (rstatus_et)metis_patition_func(
        &num_element,
        &n_conditions,
        xadj,
        adjncy,
        vwgt,
        &v_size,
        adjwgt,
        &num_parts,
        tpwgts,
        nullptr,
        nullptr,
        &obj_val,
        &part[0]);

    moer_mesh.meshlets.resize(num_parts);

    std::for_each(moer_mesh.meshlets.begin(), moer_mesh.meshlets.end(), [&](MeshletDesc& meshlet) {
        meshlet.prim_count   = 0;
        meshlet.vertex_count = 0;

        meshlet.vertex_begin = moer_mesh.vertex_indices.size();
        meshlet.prim_begin   = moer_mesh.primitive_indices.size();

        Moer::Array<uint32_t> vertex_indices;
        Moer::Array<uint8_t>  primitive_indices;

        uint32_t meshlet_index = &meshlet - &moer_mesh.meshlets[0];
        for (const int& partition_id : part | std::views::filter([&](const int& p) { return p == meshlet_index; }) | std::views::transform([&](const int& p) { return &p - &part[0]; })) {

            int face_index    = partition_id;
            int indice_offset = face_index * 3;

            meshlet.prim_count++;

            int local_indice_index = 0;
            for (int i = 0; i < 3; i++) {
                int indice = GetOriginalIndice(indice_offset + i);
                if (auto target = std::find(vertex_indices.begin(), vertex_indices.end(), indice); target == vertex_indices.end()) {
                    vertex_indices.push_back(indice);
                    local_indice_index = vertex_indices.size() - 1;
                    meshlet.vertex_count += 1;
                } else {
                    local_indice_index = target - vertex_indices.begin();
                }

                primitive_indices.push_back(local_indice_index);
            }
        }

        moer_mesh.vertex_indices.insert(moer_mesh.vertex_indices.end(), vertex_indices.begin(), vertex_indices.end());
        moer_mesh.primitive_indices.insert(moer_mesh.primitive_indices.end(), primitive_indices.begin(), primitive_indices.end());
    });

    std::for_each(moer_mesh.meshlets.begin(), moer_mesh.meshlets.end(), [&](MeshletDesc& meshlet) {
        MeshletInfo info;
        info.center = Moer::Vector3f(0);
        info.extent = Moer::Vector3f(0);
        Moer::Vector3f min_pos(Moer::MAX_FLOAT);
        Moer::Vector3f max_pos(Moer::MIN_FLOAT);

        for (int i = 0; i < meshlet.vertex_count; i++) {
            int vertex_index = moer_mesh.vertex_indices[meshlet.vertex_begin + i];
            Moer::Vector3f position = original_mesh.vertexs[vertex_index].position;
            min_pos = Moer::Min(min_pos, position);
            max_pos = Moer::Max(max_pos, position);
        }
        
        info.center = (min_pos + max_pos) * 0.5f;
        info.extent = (max_pos - min_pos) * 0.5f;

        moer_mesh.meshlet_info.push_back(info);
    });
}