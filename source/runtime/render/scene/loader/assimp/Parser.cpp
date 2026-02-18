#include "Parser.h"

#include <assimp/Importer.hpp>
#include <assimp/pbrmaterial.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <entt/entt.hpp>
#include <gtl/phmap.hpp>

#include "log/LogSystem.h"
#include "math/Function.h"
#include "math/Transform.h"
#include "misc/Hash.h"
#include "misc/Timer.h"
#include "misc/Traits.h"
#include "scene/LogicalScene.h"
#include "scene/loader/io/ImageIO.h"
#include "shaderheaders/shared/utils/Packing.h"
#include "taskgraph/TaskGraph.h"

namespace Moer::assimp {

int32_t GetEmbeddedTextureId(const std::string& path) {
    if (path.length() >= 2 && path[0] == '*') {
        for (int i = 1; i < path.length(); i++) {
            if (!isdigit(path[i])) {
                return -1;
            }
        }
        return std::atoi(path.c_str() + 1); // NOLINT
    }
    return -1;
}

// PImpl模式，转发
bool Parser::LoadSceneFromFile(ecs::LogicalScene& out_logical_scene, const std::filesystem::path& file_path) {
    // MARK: Assimp

    // ai_scene资源的持有者是importer，所以需要注意importer生命周期
    Assimp::Importer importer;

    const aiScene* ai_scene = ([&]() -> const aiScene* {
        auto path = std::filesystem::weakly_canonical(file_path);

        if (!std::filesystem::exists(path)) {
            LOG_WARNING("File not exist: {}", path.string());
            LOG_WARNING("Please check the `scene_path` in source/configs/MoerEngine.toml.");
            return nullptr;
        }

        const aiScene* gltf_scene = importer.ReadFile(
            file_path.string(),
            aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenBoundingBoxes | aiProcess_GenNormals |
                aiProcess_CalcTangentSpace
                /* 后面两个是新加的 */
                | aiProcess_RemoveRedundantMaterials | aiProcess_OptimizeMeshes
        );

        if (gltf_scene == nullptr) {
            LOG_WARNING(
                "Failed to load gltf file: {}. Assimp error: {}",
                file_path.string(),
                importer.GetErrorString()
            );
            return nullptr;
        }

        return gltf_scene;
    }());

    if (ai_scene == nullptr) {
        return false;
    }

    /**
     * MARK: Tools Functions
     */

    auto color4_to_float4 = [](const aiColor4D& ai_vec) -> float4 {
        return float4{ai_vec.r, ai_vec.g, ai_vec.b, ai_vec.a};
    };

    auto quat_to_float4 = [](const aiQuaternion& ai_quat) -> float4 {
        return float4{ai_quat.x, ai_quat.y, ai_quat.z, ai_quat.w};
    };

    auto color3_to_float3 = [](const aiColor3D& ai_vec) -> float3 {
        return float3{ai_vec.r, ai_vec.g, ai_vec.b};
    };

    auto vec3_to_float3 = [](const aiVector3D& ai_vec) -> float3 {
        return float3{ai_vec.x, ai_vec.y, ai_vec.z};
    };

    auto to_float4x4 = [](const aiMatrix4x4& ai_mat) -> float4x4 {
        // aiMatrix4x4 is row-major
        // float4x4 is row-major
        return float4x4{
            {ai_mat[0][0], ai_mat[0][1], ai_mat[0][2], ai_mat[0][3]},
            {ai_mat[1][0], ai_mat[1][1], ai_mat[1][2], ai_mat[1][3]},
            {ai_mat[2][0], ai_mat[2][1], ai_mat[2][2], ai_mat[2][3]},
            {ai_mat[3][0], ai_mat[3][1], ai_mat[3][2], ai_mat[3][3]}
        };
    };

    auto color3_to_string = [](const aiColor3D& ai_color) -> std::string {
        return "(" + std::to_string(ai_color.r) + ", " + std::to_string(ai_color.g) + ", " +
               std::to_string(ai_color.b) + ")";
    };
    auto color4_to_string = [](const aiColor4D& ai_color) -> std::string {
        return "(" + std::to_string(ai_color.r) + ", " + std::to_string(ai_color.g) + ", " +
               std::to_string(ai_color.b) + ", " + std::to_string(ai_color.a) + ")";
    };
    auto vec3_to_string = [](const aiVector3D& ai_vec) -> std::string {
        return "(" + std::to_string(ai_vec.x) + ", " + std::to_string(ai_vec.y) + ", " +
               std::to_string(ai_vec.z) + ")";
    };

    // MARK: Initialize Logical Scene

    // TODO: 把这些code独立出去

    auto& r = out_logical_scene.r();

    r.clear();

    r.ctx().emplace<ecs::CtxMegaBuffers>();

    auto& mega_bufs = r.ctx().get<ecs::CtxMegaBuffers>();
    auto& pos_buf   = mega_bufs.position;
    auto& nor_buf   = mega_bufs.packed_normal;
    auto& tan_buf   = mega_bufs.packed_tangent;
    auto& uv0_buf   = mega_bufs.texcoord0;
    auto& idx_buf   = mega_bufs.index;

    // MARK: Prepare Buffers

    {
        // 先遍历一遍所有mesh，初始化每个buf大小

        uint32 vtx_cnt = 0;
        uint32 idx_cnt = 0;

        for (uint i = 0; i < ai_scene->mNumMeshes; i++) {
            auto* mesh = ai_scene->mMeshes[i];

            vtx_cnt += mesh->mNumVertices;
            idx_cnt += mesh->mNumFaces * 3;
        }

        pos_buf.reserve(vtx_cnt);
        nor_buf.reserve(vtx_cnt);
        tan_buf.reserve(vtx_cnt);
        uv0_buf.reserve(vtx_cnt);
        idx_buf.reserve(idx_cnt);
    }

    // MARK: Load Meshes
    gtl::flat_hash_map<aiMesh*, entt::entity> mesh_entity_map;
    {
        uint32 vtx_cnt = 0;
        uint32 idx_cnt = 0;

        for (uint i = 0; i < ai_scene->mNumMeshes; i++) {
            auto* mesh = ai_scene->mMeshes[i];

            auto make_buffer_view = [&](uint32 existing_size, uint32 current_size, uint32 stride) {
                return ecs::CPrimitive::BufferView{
                    .start_idx = existing_size,
                    .stride    = stride,
                    .is_valid  = true // 该mesh是否存在这个顶点属性
                };
            };

            const auto entity = r.create();

            auto& c_primitive = r.emplace<ecs::CPrimitive>(entity);

            // bounding box

            c_primitive.aabb = Box3D(
                float3(mesh->mAABB.mMin.x, mesh->mAABB.mMin.y, mesh->mAABB.mMin.z),
                float3(mesh->mAABB.mMax.x, mesh->mAABB.mMax.y, mesh->mAABB.mMax.z)
            );

            // mesh

            c_primitive.vertex_count = mesh->mNumVertices;
            c_primitive.index_count  = mesh->mNumFaces * 3;

            mesh_entity_map[mesh] = entity;

            if (mesh->HasPositions()) {
                c_primitive.position = make_buffer_view(pos_buf.size(), mesh->mNumVertices, sizeof(float3));

                for (uint j = 0; j < mesh->mNumVertices; j++) {
                    const auto& pos = mesh->mVertices[j];
                    pos_buf.emplace_back(pos.x, pos.y, pos.z);
                }
            }

            if (mesh->HasNormals()) {
                c_primitive.packed_normal =
                    make_buffer_view(nor_buf.size(), mesh->mNumVertices, sizeof(uint32));

                for (uint j = 0; j < mesh->mNumVertices; j++) {
                    const auto& nor    = mesh->mNormals[j];
                    float3      normal = {nor.x, nor.y, nor.z};
                    nor_buf.emplace_back(Pack_Normal(normal));
                }
            }

            if (mesh->HasTangentsAndBitangents()) {
                c_primitive.packed_tangent =
                    make_buffer_view(tan_buf.size(), mesh->mNumVertices, sizeof(uint32));

                for (uint j = 0; j < mesh->mNumVertices; j++) {
                    const auto& tan     = mesh->mTangents[j];
                    float3      tangent = {tan.x, tan.y, tan.z};
                    tan_buf.emplace_back(Pack_Normal(tangent));
                }
            }

            if (mesh->HasTextureCoords(0)) {
                c_primitive.texcoord0 = make_buffer_view(uv0_buf.size(), mesh->mNumVertices, sizeof(float2));

                for (uint j = 0; j < mesh->mNumVertices; j++) {
                    const auto& uv0 = mesh->mTextureCoords[0][j];
                    uv0_buf.emplace_back(uv0.x, uv0.y);
                }
            }

            // TODO: Support TexCoord1
            if (mesh->HasTextureCoords(1)) {
                static bool first_time = true;
                if (first_time) {
                    first_time = false;
                    LOG_WARNING("Mesh has TexCoord1, which is not supported yet.");
                }
            }

            {
                c_primitive.index = make_buffer_view(idx_buf.size(), mesh->mNumFaces * 3, sizeof(uint32));

                for (uint j = 0; j < mesh->mNumFaces; j++) {
                    const auto& face = mesh->mFaces[j];
                    idx_buf.emplace_back(face.mIndices[0]);
                    idx_buf.emplace_back(face.mIndices[1]);
                    idx_buf.emplace_back(face.mIndices[2]);
                }
            }

            vtx_cnt += mesh->mNumVertices;
            idx_cnt += mesh->mNumFaces * 3;
        }

        LOG_INFO("Meshed Loaded: {} meshes, {} vertices, {} indices", ai_scene->mNumMeshes, vtx_cnt, idx_cnt);
    }

    // MARK: Build Primitive Hash

    out_logical_scene.SBuildPrimitiveHash();

    // MARK: Load Textures

    // 支持多线程加载纹理
    gtl::parallel_flat_hash_map<std::string, entt::entity> tex_map;
    {
        const auto available_texture_types = Moer::Array<aiTextureType>{
            // legacy
            aiTextureType_DIFFUSE,
            aiTextureType_SPECULAR,
            aiTextureType_AMBIENT,
            aiTextureType_EMISSIVE,
            aiTextureType_HEIGHT,
            aiTextureType_NORMALS,
            aiTextureType_SHININESS,
            aiTextureType_OPACITY,
            aiTextureType_DISPLACEMENT,
            aiTextureType_LIGHTMAP,
            aiTextureType_REFLECTION,
            // pbr
            aiTextureType_BASE_COLOR,
            aiTextureType_NORMAL_CAMERA,
            aiTextureType_EMISSION_COLOR,
            aiTextureType_METALNESS,
            aiTextureType_DIFFUSE_ROUGHNESS,
            aiTextureType_AMBIENT_OCCLUSION,
            // pbr extensions
            aiTextureType_SHEEN,
            aiTextureType_CLEARCOAT,
            aiTextureType_TRANSMISSION
        };

        // 收集所有需要加载的纹理路径
        auto all_needed_textures_set = UnorderedSet<std::string>{};

        for (int i = 0; i < ai_scene->mNumMaterials; i++) {
            const auto* material = ai_scene->mMaterials[i];

            for (const auto texture_type : available_texture_types) {
                const uint32_t texture_count = material->GetTextureCount(texture_type);

                for (uint32_t j = 0; j < texture_count; j++) {
                    aiString texture_path;

                    if (material->GetTexture(texture_type, j, &texture_path) == AI_SUCCESS) {
                        all_needed_textures_set.insert(texture_path.C_Str());
                    }
                }
            }
        }

        // load texture single

        // 加载ImageReadDesc
        auto load_image_desc = [&](const std::string& texture_path) {
            // TODO: 这里没有重构过，需要检查是否有bug
            // 可能存在的问题：① channel和format ② mipmap

            const int32 embedded_id = GetEmbeddedTextureId(texture_path);

            const uint32 preferred_channel   = 4;
            const bool   is_generate_mipmaps = true; // 暂定mipmap直接读取

            if (embedded_id >= 0) {
                const aiTexture* texture = ai_scene->mTextures[embedded_id];

                return ImageIO::ReadFromMemory(
                    reinterpret_cast<unsigned char*>(texture->pcData),
                    texture->mWidth * texture->mHeight * 4,
                    preferred_channel,
                    is_generate_mipmaps
                );
            }

            const auto            preferred_format  = EPixelFormat::PF_R8G8B8A8_UNORM;
            std::filesystem::path texture_file_path = file_path.parent_path() / texture_path;

            return ImageIO::ReadFromFile(
                texture_file_path, preferred_channel, preferred_format, is_generate_mipmaps
            );
        };

        // 初始化CTexture
        auto update_c_texture = [&](ecs::CTexture& c_texture, const ImageReadDesc& image_desc) {
            // copy
            c_texture.format            = image_desc.format;
            c_texture.width             = image_desc.width;
            c_texture.height            = image_desc.height;
            c_texture.mip_level_count   = image_desc.mips;
            c_texture.array_layer_count = image_desc.layers;
            c_texture.data.resize(image_desc.data_size);
            std::copy_n(
                reinterpret_cast<uint8*>(image_desc.data), image_desc.data_size, c_texture.data.data()
            );

            // call back
            if (image_desc.data_callback != nullptr) {
                image_desc.data_callback(image_desc.data);
            }

            // assert
            assert(
                image_desc.channel == Render::GetChannelFromPixelFormat(image_desc.format) &&
                "Image channel does not match its format."
            );
        };

        /**
         * 多线程注意点
         * 
         * entt::registry不保证线程安全，所以我们必须提前创建好entity和CTexture
         * 同时，映射表也需要使用线程安全的数据结构
         */
        for (const auto& texture_path : all_needed_textures_set) {
            const auto entity    = r.create();
            auto&      c_texture = r.emplace<ecs::CTexture>(entity);
            auto&      c_name    = r.emplace<ecs::CName>(entity);

            tex_map[texture_path] = entity;
        }

        // 加载单个纹理并创建对应的entity和CTexture
        auto load_texture = [&](const std::string& texture_path) {
            ImageReadDesc image_desc = load_image_desc(texture_path);

            if (!image_desc.IsValid()) {
                LOG_WARNING("Load Texture Failed: {}", texture_path);
                return;
            }

            const auto entity    = tex_map[texture_path];
            auto&      c_texture = r.get<ecs::CTexture>(entity);
            auto&      c_name    = r.get<ecs::CName>(entity);

            c_name.name = texture_path;

            update_c_texture(c_texture, image_desc);
        };

        // multi-thread load textures

        const bool is_multi_thread = true;

        Timer elapsed_timer;
        elapsed_timer.Start();

        if (is_multi_thread) {
            // 多线程加载纹理
            std::atomic<uint32> done_cnt = 0;

            Array<std::string> all_needed_textures(
                all_needed_textures_set.begin(), all_needed_textures_set.end()
            );

            auto all_done = ParallelForAsync(all_needed_textures.size(), [&](uint32 i) {
                load_texture(all_needed_textures[i]);
                done_cnt.fetch_add(1);
            });

            // 计时，每2s打印一次进度，结束后输出总耗时
            LoopedTimer log_timer(2.0f, false);
            while (all_done->IsComplete() == false) {
                if (log_timer.Tick()) {
                    LOG_INFO(
                        "Loading Textures Parallelly: {}/{}", done_cnt.load(), all_needed_textures.size()
                    );
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

        } else {
            // 单线程加载纹理
            uint32 done_cnt = 0;
            for (const auto& texture_path : all_needed_textures_set) {
                load_texture(texture_path);
                done_cnt++;
                LOG_INFO("Loading Textures Sequentially: {}/{}", done_cnt, all_needed_textures_set.size());
            }
        }

        elapsed_timer.Stop();

        LOG_INFO(
            "Textures Loaded: {}. Multi-threads is {}. Time used: {} sec",
            all_needed_textures_set.size(),
            is_multi_thread ? "ON" : "OFF",
            elapsed_timer.ElapsedSeconds()
        );
    }

    // MARK: Load Materials

    std::stringstream mat_ss;
    mat_ss << "\nMaterial Info: \n";

    gtl::flat_hash_map<uint, entt::entity> material_index_to_entity_map;
    {
        gtl::flat_hash_map<std::string, entt::entity> mat_map;

        auto load_texture_by_types = [&](const aiMaterial*                 mat,
                                         const std::string&                type_name,
                                         const std::vector<aiTextureType>& types) -> entt::entity {
            for (const auto texture_type : types) {
                aiString texture_path;
                if (mat->GetTexture(texture_type, 0, &texture_path) == AI_SUCCESS) {
                    assert(
                        tex_map.contains(texture_path.C_Str()) &&
                        "Texture is needed, but hasn't loaded. Perhaps internal code error"
                    );

                    mat_ss << "\t\tLinked Texture: " << texture_path.C_Str() << " for type " << type_name
                           << "\n";

                    return tex_map[texture_path.C_Str()];
                }
            }
            return entt::null;
        };

        auto load_normal_map = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            out_mat.normal_map_entt = load_texture_by_types(
                mat, "normal map", {aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA}
            );
        };

        auto load_ao_map = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            out_mat.ao_map_entt = load_texture_by_types(
                mat,
                "ao map",
                {aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP, aiTextureType_AMBIENT}
            );
        };

        auto load_emissive_map = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            out_mat.emissive_map_entt = load_texture_by_types(
                mat, "emissive map", {aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE}
            );
        };

        auto load_albedo_map = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            out_mat.albedo_map_entt =
                load_texture_by_types(mat, "albedo map", {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE});
        };

        auto load_metallic_roughness_map = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            // 这个纹理比较特殊，是glTF独有的
            aiString texture_path;
            if (mat->GetTexture(
                    AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, &texture_path
                ) == AI_SUCCESS) {

                assert(
                    tex_map.contains(texture_path.C_Str()) &&
                    "Texture is needed, but hasn't loaded. Perhaps internal code error"
                );

                out_mat.metallic_roughness_map_entt = tex_map[texture_path.C_Str()];
                mat_ss << "\t\tLinked Texture: " << texture_path.C_Str() << " for type metallic roughness map"
                       << "\n";
            }
        };

        auto load_albedo_factor = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            aiColor4D albedo_factor;
            if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, albedo_factor) != AI_SUCCESS)
                return;

            out_mat.albedo_factor = color4_to_float4(albedo_factor);
            mat_ss << "\t\tLoad Albedo Factor: " << out_mat.albedo_factor.ToString() << "\n";
        };

        auto load_emissive_factor = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            aiColor3D emissive_factor;
            if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive_factor) != AI_SUCCESS)
                return;

            out_mat.emissive_factor = color3_to_float3(emissive_factor);
            mat_ss << "\t\tLoad Emissive Factor: " << out_mat.emissive_factor.ToString() << "\n";
        };

        auto load_metallic_factor = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            float metallic_factor = 0.0f;
            if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic_factor) != AI_SUCCESS)
                return;

            out_mat.metallic_factor = static_cast<float>(metallic_factor);
            mat_ss << "\t\tLoad Metallic Factor: " << out_mat.metallic_factor << "\n";
        };

        auto load_roughness_factor = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            float roughness_factor = 0.5f;
            if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness_factor) != AI_SUCCESS)
                return;

            out_mat.roughness_factor = static_cast<float>(roughness_factor);
            mat_ss << "\t\tLoad Roughness Factor: " << out_mat.roughness_factor << "\n";
        };

        auto load_alpha_param = [&](ecs::CMaterial& out_mat, const aiMaterial* mat) {
            aiString alpha_mode;

            if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode) != AI_SUCCESS) {
                mat_ss << "\t\tLoad Alpha Mode: OPAQUE (Default)" << "\n";
                return;
            }

            if (alpha_mode == aiString("BLEND")) {
                out_mat.alpha_mode = EAlphaMode::Blend;
                mat_ss << "\t\tLoad Alpha Mode: BLEND" << "\n";

            } else if (alpha_mode == aiString("MASK")) {
                out_mat.alpha_mode = EAlphaMode::Mask;

                float alpha_cutoff = 0.5f;
                if (mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alpha_cutoff) == AI_SUCCESS) {
                    out_mat.alpha_cutoff = static_cast<float>(alpha_cutoff);
                    mat_ss << "\t\tLoad Alpha Mode: MASK. And Alpha Cutoff: " << out_mat.alpha_cutoff << "\n";
                }

            } else if (alpha_mode == aiString("OPAQUE")) {
                mat_ss << "\t\tLoad Alpha Mode: OPAQUE" << "\n";

            } else {
                assert(false && "Unknown Alpha Mode in glTF material.");
            }
        };

        // 前面添加了aiProcess_RemoveRedundantMaterials，此处基本保证没有重复Material
        for (uint i = 0; i < ai_scene->mNumMaterials; i++) {
            const auto* material = ai_scene->mMaterials[i];

            std::string material_name = ([&]() {
                aiString name;
                if (material->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
                    return std::string(name.C_Str());
                } else {
                    return std::format("Material #{}", i);
                }
            }());

            const auto entity     = r.create();
            auto&      c_material = r.emplace<ecs::CMaterial>(entity);

            mat_map[material_name]          = entity;
            material_index_to_entity_map[i] = entity;

            // Load Data

            mat_ss << "\tLoading Material: " << material_name << "\n";

            load_normal_map(c_material, material);
            load_ao_map(c_material, material);
            load_albedo_map(c_material, material);
            load_emissive_map(c_material, material);
            load_metallic_roughness_map(c_material, material);

            load_albedo_factor(c_material, material);
            load_emissive_factor(c_material, material);
            load_metallic_factor(c_material, material);
            load_roughness_factor(c_material, material);

            load_alpha_param(c_material, material);
        }
    }

    // MARK: 在CPrimitive上挂载Material Entity
    {
        // 遍历所有 mesh，将对应的 material entity 挂载到 primitive 上
        for (const auto& [mesh, primitive_entity] : mesh_entity_map) {
            if (!r.valid(primitive_entity) || !r.all_of<ecs::CPrimitive>(primitive_entity)) {
                continue;
            }

            auto& c_primitive = r.get<ecs::CPrimitive>(primitive_entity);

            // 通过 mesh->mMaterialIndex 找到对应的 material entity
            uint material_index = mesh->mMaterialIndex;
            if (material_index_to_entity_map.contains(material_index)) {
                c_primitive.material_entt = material_index_to_entity_map[material_index];
            } else {
                LOG_WARNING(
                    "Mesh material index {} out of range. Total materials: {}. "
                    "Primitive will have null material.",
                    material_index,
                    ai_scene->mNumMaterials
                );
                c_primitive.material_entt = entt::null;
            }
        }
    }

    /**
     * MARK: Preload Cameras and Lights
     * 
     * 因为assimp中，无法通过node定位camera和light，而是需要在camera和light中拿name去找
     * 所以这里需要建立 name -> camera/light ptr 的 map
     */

    gtl::flat_hash_map<std::string, const aiCamera*> camera_map;
    gtl::flat_hash_map<std::string, const aiLight*>  light_map;

    std::string main_camera_name;
    std::string main_light_name;

    {

        bool has_main_camera = false;
        bool has_main_light  = false;

        for (uint i = 0; i < ai_scene->mNumCameras; i++) {
            const auto* camera = ai_scene->mCameras[i];

            const std::string camera_name = camera->mName.C_Str();

            camera_map[camera_name] = camera;

            if (has_main_camera == false) {
                has_main_camera  = true;
                main_camera_name = camera_name;
            }
        }
        for (uint i = 0; i < ai_scene->mNumLights; i++) {
            const auto* light = ai_scene->mLights[i];

            const std::string light_name = light->mName.C_Str();

            light_map[light_name] = light;

            if (has_main_light == false && light->mType == aiLightSource_DIRECTIONAL) {
                has_main_light  = true;
                main_light_name = light_name;
            }
        }
    }

    /**
     * MARK: Camera & Light Tools
     */

    auto is_mesh_node = [&](const aiNode* node) -> bool {
        return node->mNumMeshes > 0;
    };
    auto is_camera_node = [&](const aiNode* node) -> bool {
        std::string node_name = node->mName.C_Str();
        return camera_map.contains(node_name);
    };
    auto is_light_node = [&](const aiNode* node) -> bool {
        std::string node_name = node->mName.C_Str();
        return light_map.contains(node_name);
    };

    // MARK: Mesh

    gtl::flat_hash_map<uint64, entt::entity> mesh_node_map;

    auto create_mesh_to_node = [&](entt::entity node_entt, const aiNode* node) {
        // 每个Mesh Node的node entity都会绑定一个CRenderable，表示这个Node应被渲染
        auto& c_renderable = r.emplace<ecs::CRenderable>(node_entt);

        // 计算Mesh Node的hash值
        uint64 hash = 0;
        for (uint i = 0; i < node->mNumMeshes; i++) {
            auto* mesh = ai_scene->mMeshes[node->mMeshes[i]];
            HashCombine(hash, reinterpret_cast<uint64>(mesh));
        };

        if (mesh_node_map.contains(hash)) {
            // 已经创建过CMesh了
            // 把Renderable指向已有的CMesh Entity
            c_renderable.mesh_entt = mesh_node_map[hash];
            return;
        }

        // 没有创建过CMesh
        // 创建CMesh Entity，并把Renderable指向它
        const auto mesh_entity = r.create();
        mesh_node_map[hash]    = mesh_entity;
        c_renderable.mesh_entt = mesh_entity;

        // 在CMesh Entity上添加CMesh组件
        auto& c_mesh = r.emplace<ecs::CMesh>(mesh_entity);
        for (uint i = 0; i < node->mNumMeshes; i++) {
            auto* mesh = ai_scene->mMeshes[node->mMeshes[i]];

            assert(
                mesh_entity_map.contains(mesh) &&
                "Mesh entity not found in mesh_entity_map. Perhaps internal code error."
            );

            c_mesh.primitive_entts.emplace_back(mesh_entity_map[mesh]);
        }
    };

    // MARK: Camera

    auto create_camera_to_node = [&](entt::entity node_entt, const aiNode* node) {
        const aiCamera* camera = camera_map[node->mName.C_Str()];

        auto& c_camera = r.emplace<ecs::CCamera>(node_entt);
        if (main_camera_name == camera->mName.C_Str()) {
            r.emplace<ecs::CTagMainCamera>(node_entt);
        }

        float4 position = float4(camera->mPosition.x, camera->mPosition.y, camera->mPosition.z, 1.0f);
        float4 look_at  = float4(camera->mLookAt.x, camera->mLookAt.y, camera->mLookAt.z, 0.0f);
        float4 up       = float4(camera->mUp.x, camera->mUp.y, camera->mUp.z, 0.0f);

        const auto& c_transform    = r.get<ecs::CTransform>(node_entt);
        Transform   node_transform = c_transform.d_world_transform;

        float4 world_pos           = node_transform * position;
        float4 world_look_at       = node_transform * look_at;
        float4 world_up            = node_transform * up;
        float4 world_loot_at_point = world_pos + world_look_at;

        Transform world2camera = Transform(float3(world_pos), float3(world_loot_at_point), float3(world_up));
        Transform camera2world = Inverse(world2camera.GetMatrix4x4());

        // The interpretation of 'mHorizontalFOV' is inconsistent among gltf2, fbx, and the documentation in the official Assimp version (5.4.2).
        // In this project, we ensure 'mHorizontalFOV' is the 'half' of the horizontal field of view angle (at least in GLTF2 and FBX).
        float fov_y_deg = AI_RAD_TO_DEG(2.0f * atan(tan(camera->mHorizontalFOV) / camera->mAspect));

        c_camera.camera = Camera();
        c_camera.camera.Initialize(
            camera2world,
            fov_y_deg,
            camera->mAspect,
            std::min(0.001f, camera->mClipPlaneNear),
            camera->mClipPlaneFar
        );
    };

    // MARK: Light

    auto create_light_to_node = [&](entt::entity node_entt, const aiNode* node) {
        const aiLight* light = light_map[node->mName.C_Str()];

        auto& c_light = r.emplace<ecs::CLight>(node_entt);
        if (main_light_name == light->mName.C_Str()) {
            r.emplace<ecs::CTagMainLight>(node_entt);
        }

        if (light->mType == aiLightSourceType::aiLightSource_DIRECTIONAL) {
            c_light.type = ELightType::Directional;

            // 处理方向：light->mDirection 是相对于 node transform 的局部空间方向（根据 assimp 文档）
            float3 local_dir = vec3_to_float3(light->mDirection);
            // 归一化局部方向
            float len = Lengthf(local_dir);
            if (len > 1e-6f) {
                local_dir = local_dir / len;
            } else {
                // 如果方向为零向量，使用默认方向
                local_dir = float3(0.0f, 0.0f, -1.0f);
            }

            // 应用 node 的 transform 到 light direction
            // direction 是向量，只需要应用 rotation（scale 不影响方向向量的方向，只影响长度）
            const auto& c_transform    = r.get<ecs::CTransform>(node_entt);
            float3x3    world_rotation = float3x3{
                float3(c_transform.d_world_transform.r0.xyz),
                float3(c_transform.d_world_transform.r1.xyz),
                float3(c_transform.d_world_transform.r2.xyz)
            };
            float3 world_dir = Normalizef(world_rotation * local_dir);

            // 存储应用了 node transform 后的方向
            r.emplace<ecs::CLightDirectional>(
                node_entt,
                ecs::CLightDirectional{.color = color3_to_float3(light->mColorDiffuse), .intensity = 1.0f}
            );

            // 更新 CTransform 的 rotation，使得默认方向 (0, 0, -1) 旋转到 world_dir
            float3 origin_dir = float3(0.0f, 0.0f, -1.0f);
            if (Dotf(world_dir, origin_dir) < 0.9999f) {
                Quaternion light_rot_quat = Quaternion(origin_dir, world_dir);
                // 直接设置 rotation，因为 world_dir 已经应用了 node 的 transform
                auto& c_transform_mut    = r.get<ecs::CTransform>(node_entt);
                c_transform_mut.rotation = light_rot_quat;
                c_transform_mut.is_dirty = true;
            }

        } else if (light->mType == aiLightSourceType::aiLightSource_POINT) {
            c_light.type = ELightType::Point;

            float3 local_pos = vec3_to_float3(light->mPosition);

            // 应用 node 的 transform 到 light position
            // position 是点，需要应用完整的 transform（translation + rotation + scale）
            const auto& c_transform = r.get<ecs::CTransform>(node_entt);
            Transform   node_transform(c_transform.translation, c_transform.scale, c_transform.rotation);
            float3      world_pos = node_transform * local_pos;

            // 存储应用了 node transform 后的位置
            r.emplace<ecs::CLightPoint>(
                node_entt,
                ecs::CLightPoint{.color = color3_to_float3(light->mColorDiffuse), .intensity = 1.0f}
            );

            // 更新 CTransform 的 translation 为 world_pos
            auto& c_transform_mut       = r.get<ecs::CTransform>(node_entt);
            c_transform_mut.translation = world_pos;
            c_transform_mut.is_dirty    = true;

        } else if (light->mType == aiLightSourceType::aiLightSource_SPOT) {
            c_light.type = ELightType::Spot;

            LOG_WARNING("Spot light is not supported yet. TODO");

        } else if (light->mType == aiLightSourceType::aiLightSource_AMBIENT) {
            c_light.type = ELightType::Ambient;

            LOG_WARNING("Ambient light is not supported yet. TODO");

        } else {
            LOG_WARNING(
                "Unsupported light type: {}. Only Directional, Point, Spot, and Ambient lights are "
                "supported.",
                static_cast<uint32>(light->mType)
            );
            return;
        }
    };

    // MARK: Load Node

    uint32 total_node_cnt = 0;
    {
        auto decompose_transform = [&](const aiMatrix4x4& ai_mat,
                                       float3&            out_translation,
                                       Quaternion&        out_rotation_quaternion,
                                       float3&            out_scale) {
            aiVector3D   ai_translation;
            aiQuaternion ai_rotation;
            aiVector3D   ai_scale;

            ai_mat.Decompose(ai_scale, ai_rotation, ai_translation);

            out_translation         = float3(ai_translation.x, ai_translation.y, ai_translation.z);
            out_rotation_quaternion = Quaternion(ai_rotation.x, ai_rotation.y, ai_rotation.z, ai_rotation.w);
            out_scale               = float3(ai_scale.x, ai_scale.y, ai_scale.z);
        };

        // Root Node
        const auto& root_node_entt = r.create();
        {
            r.emplace<ecs::CTagRootNode>(root_node_entt);

            r.emplace<ecs::CNode>(root_node_entt);
            r.emplace<ecs::CTransform>(root_node_entt);

            // root_node_entt的深度默认为0，之后的depth会在UEmplaceNodeYToX中计算
        }

        // Scene Meta Data
        {
            const auto& entity            = r.create();
            auto&       c_scene_meta_data = r.emplace<ecs::CSceneMetaData>(entity);

            c_scene_meta_data.root_node_entt = root_node_entt;
            c_scene_meta_data.scene_path     = file_path.string();
        }

        // Traverse Nodes
        std::function<void(entt::entity, const aiNode*)> dfs_node = [&](const entt::entity x,
                                                                        const aiNode*      y) {
            // x: current entity
            // y: current aiNode
            total_node_cnt++;

            // here, assert (1) x has been created (2) has CNode (3) has CTransform
            assert(
                (r.all_of<ecs::CNode, ecs::CTransform>(x)) && "Entity has no CNode & CTransform components."
            );

            // get CNode
            auto& c_node  = r.get<ecs::CNode>(x);
            auto& c_trans = r.get<ecs::CTransform>(x);

            // 基础数据
            decompose_transform(y->mTransformation, c_trans.translation, c_trans.rotation, c_trans.scale);
            // transform
            c_trans.d_world_transform = to_float4x4(y->mTransformation);
            // 如果有parent，则更新world transform
            if (c_node.parent_entt != entt::null) {
                const auto& parent_transform = r.get<ecs::CTransform>(c_node.parent_entt);
                c_trans.d_world_transform    = parent_transform.d_world_transform * c_trans.d_world_transform;
            }
            c_trans.is_dirty = false;

            /**
             * traverse children
             * 
             * 需要设置c_node: first_child_entt, last_child_entt, child_count
             * 需要设置c_child_node: parent_entt, prev_sibling_entt, next_sibling_entt
             */

            Array<entt::entity> child_entities;
            child_entities.reserve(y->mNumChildren);

            for (uint i = 0; i < y->mNumChildren; i++) {
                const auto& child_entity = r.create();
                child_entities.emplace_back(child_entity);

                // add CNode to child
                auto& c_child_node      = r.emplace<ecs::CNode>(child_entity);
                auto& c_child_transform = r.emplace<ecs::CTransform>(child_entity);

                out_logical_scene.UEmplaceNodeToParent(x, c_node, child_entity, c_child_node);
            }

            /**
             * mesh or light or camera
             * 
             * 注意：虽然 glTF 规范保证一个 node 最多只有一类对象（mesh/light/camera 三选一），
             * 但 Assimp 在某些情况下（如格式转换）可能会产生同时有 mesh 和 camera/light 引用的节点。
             * 如果出现这种情况，我们按优先级处理：mesh > camera > light
             */

            bool is_mesh   = is_mesh_node(y);
            bool is_light  = is_light_node(y);
            bool is_camera = is_camera_node(y);

            uint type_count = uint(is_mesh) + uint(is_light) + uint(is_camera);
            if (type_count > 1) {
                LOG_WARNING(
                    "Node '{}' has multiple types (mesh: {}, camera: {}, light: {}). "
                    "Processing with priority: mesh > camera > light.",
                    y->mName.C_Str(),
                    is_mesh,
                    is_camera,
                    is_light
                );
            }

            if (is_mesh) {
                create_mesh_to_node(x, y);

            } else if (is_camera) {
                create_camera_to_node(x, y);

            } else if (is_light) {
                create_light_to_node(x, y);

            } else {
                // empty node, transform only
            }

            /**
             * recursive traverse
             */
            for (uint i = 0; i < y->mNumChildren; i++) {
                dfs_node(child_entities[i], y->mChildren[i]);
            }
        };

        dfs_node(root_node_entt, ai_scene->mRootNode);
    }

    if (camera_map.size() == 0) {
        out_logical_scene.UCreateDefaultCamera();
    }

    if (light_map.size() == 0) {
        out_logical_scene.UCreateDefaultLights();
    }

    out_logical_scene.SBuildPrimitiveHash();

    out_logical_scene.SBuildMeshHash();

    // AABB计算分为两步
    // 1. 如果Mesh有更新，则把AABB向Mesh同步：SBuildMeshAABB
    // 2. Node更新时，取Renderable对应的Mesh的AABB，在树上进行AABB合并：SUpdateAllNodeTransformAndAABB
    out_logical_scene.SBuildMeshAABB();

    // 有camera和light，会修改CTransform的数据
    // - 例如，平行光会把自己的direction叠加到对应transform的rotation上
    out_logical_scene.SUpdateAllNodeTransformAndAABB();

    {
        // log

        // log all nodes info
        std::stringstream ss;
        ss << "\nScene Info: \n";

        // node & mesh
        ss << "\tTotal Node Count: " << total_node_cnt << "\n";
        ss << "\tAssimp Mesh Count: " << ai_scene->mNumMeshes << "\n";
        r.view<const ecs::CNode>().each([&](auto entity_id, const ecs::CNode& c_node) {
            // get the num of mesh from CRenderable
            uint mesh_cnt = 0;
            if (r.all_of<ecs::CRenderable>(entity_id)) {
                const auto& c_renderable = r.get<ecs::CRenderable>(entity_id);
                if (c_renderable.mesh_entt != entt::null && r.valid(c_renderable.mesh_entt) &&
                    r.all_of<ecs::CMesh>(c_renderable.mesh_entt)) {
                    const auto& c_mesh = r.get<ecs::CMesh>(c_renderable.mesh_entt);
                    mesh_cnt           = static_cast<uint>(c_mesh.primitive_entts.size());
                }
            }

            ss << "\t\tNode " << (uint32)entity_id << ": mesh count: " << mesh_cnt;

            if (r.all_of<ecs::CTransform>(entity_id)) {
                const ecs::CTransform& c_transform = r.get<ecs::CTransform>(entity_id);
                ss << ": transform: " << c_transform.d_world_transform.ToString(false, 3);
            }

            ss << "\n";
        });

        // camera
        ss << "\tTotal Camera Count: " << camera_map.size() << ". Assimp: " << ai_scene->mNumCameras << "\n";
        for (const auto& [name, camera] : camera_map) {
            ss << "\t\tCamera " << name << ": " << camera->mPosition.x << ", " << camera->mPosition.y << ", "
               << camera->mPosition.z << "\n";
        }

        // light
        ss << "\tTotal Light Count: " << light_map.size() << ". Assimp: " << ai_scene->mNumLights << "\n";
        for (const auto& [name, light] : light_map) {
            if (light->mType == aiLightSourceType::aiLightSource_DIRECTIONAL) {
                ss << "\t\tLight " << name
                   << ": Directional. Color: " << color3_to_string(light->mColorDiffuse) << "\n";
            } else if (light->mType == aiLightSourceType::aiLightSource_POINT) {
                ss << "\t\tLight " << name << ": Point. Color: " << color3_to_string(light->mColorDiffuse)
                   << "\n";
            } else if (light->mType == aiLightSourceType::aiLightSource_SPOT) {
                ss << "\t\tLight " << name << ": Spot. Color: " << color3_to_string(light->mColorDiffuse)
                   << "\n";
            } else if (light->mType == aiLightSourceType::aiLightSource_AMBIENT) {
                ss << "\t\tLight " << name << ": Ambient. Color: " << color3_to_string(light->mColorDiffuse)
                   << "\n";
            }
        }

        LOG_INFO("{}", ss.str());
        LOG_INFO("{}", mat_ss.str());
    }

    return true;
}

} // namespace Moer::assimp