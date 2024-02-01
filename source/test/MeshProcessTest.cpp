#include <metis.h>

#include "Core.h"
#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"


void MetisTest();
void InitTestEnv(const std::filesystem::path& workspace_path);
void ExitTestEnv();

int main(const int argc, const char** argv) {
    std::filesystem::path workspace_path = std::filesystem::current_path();
    InitTestEnv(workspace_path);
    MetisTest();
    ExitTestEnv();
}

int input_first_line[]{7,11,001};
std::vector<std::vector<int>>input_subsequents{
{5,1, 3, 2, 2, 1},
{1, 1, 3, 2, 4, 1},
{5, 3, 4, 2, 2, 2, 1, 2},
{2, 1, 3, 2, 6, 2, 7, 5},
{1, 1, 3, 3, 6, 2},
{5, 2, 4, 2, 7, 6},
{6, 6, 4, 5}
};



void MetisTest(){
    decltype(METIS_PartGraphRecursive)* metis_patition_func = METIS_PartGraphRecursive;

    decltype(METIS_MeshToDual)* metis_mesh_to_dual_func = METIS_MeshToDual;


    Assimp::Importer importer;

    auto& config_manager = Moer::ConfigManager::GetInstance();
    
    auto default_test_obj_path = config_manager.GetEditorResourcePath() / "default/scenes/sponza/models/walls-lib_2.obj";
    const aiScene* scene = importer.ReadFile(default_test_obj_path.string(), aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);
    auto* meshes = scene->mMeshes;
    auto num_meshes = scene->mNumMeshes;

    for(int i = 0; i < num_meshes ; i++){
        auto* mesh = meshes[i];
        auto* faces = mesh->mFaces;
        auto* vertices = mesh->mVertices;
        auto* normals = mesh->mNormals;
        auto* tangents = mesh->mTangents;
        auto* bitangents = mesh->mBitangents;
        auto* texture_coords = mesh->mTextureCoords[i];
        auto num_faces = mesh->mNumFaces;

        int num_element = mesh->mNumFaces;
        int num_nodes = mesh->mNumFaces * 3;

        Moer::Array<int32_t> eptr(num_element+1);
        Moer::Array<int32_t> eind(num_nodes);//num indices

        constexpr int32_t num_node_per_element = 3; //triangle mesh
        int32_t current_offset = 0;
        for(int i = 0; i < num_element + 1; i++){
            eptr[i] = current_offset;
            current_offset += num_node_per_element;
        }

        for(int i = 0; i < num_faces; i++){
            auto& face = faces[i];
            for(int j = 0; j < face.mNumIndices; j++){
                eind[i * 3 + j] = face.mIndices[j];
            }
        }

        //two triangle is connected when they have two common nodes
        int num_common_node = 2;
        int num_flag = 0;

        idx_t* xadj, *adjncy;
        auto r = metis_mesh_to_dual_func(
            &num_element,
            &num_nodes,
            eptr.data(),
            eind.data(),
            &num_common_node,
            &num_flag,
            &xadj,
            &adjncy
        );
        rstatus_et res = (rstatus_et)r;
        int n_conditions = 1;

        int v_size = 64;
        int* vwgt = nullptr;
        int* adjwgt = nullptr;
        int num_parts = num_faces / v_size;
        double* tpwgts = nullptr ;

        int obj_val;
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
            &part[0]
        );
        LOG_INFO("Metis Test");

    }
    


}

void InitTestEnv(const std::filesystem::path& workspace_path){
    //core
    Moer::ConfigManager::GetInstance().Init(workspace_path);
    Moer::TaskSystem::Init();
    Moer::LogSystem::Init();


}

void ExitTestEnv(){
    Moer::TaskSystem::ShutDown();
}