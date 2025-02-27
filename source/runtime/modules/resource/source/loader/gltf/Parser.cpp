#include "loader/gltf/Parser.h"

#include "Core.h"
#include "RenderThread.h"
#include "../io/ImageIO.h"
#include "../sceneCache/SceneCache.h"
#include "assimp/Importer.hpp"
#include "assimp/pbrmaterial.h"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "math/Base.h"
#include "math/Function.h"
#include "meshprocess/MeshProcessor.h"
#include "misc/CountableRef.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "rhi/RHI.h"
#include "resources/GpuScene.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/BufferInterfaceBlock.h"
#include "scene/Entity.h"
#include "scene/EntityManager.h"
#include "scene/CameraManager.h"
#include "scene/Material.h"
#include "scene/MaterialInstance.h"
#include "scene/RenderableManager.h"
#include "scene/Scene.h"
#include "scene/TransformManager.h"
#include "scene/light/LightComponent.h"
#include "scene/light/DirectionalLightComponent.h"
#include "scene/light/PointLightComponent.h"
#include "scene/light/SpotLightComponent.h"
#include "taskgraph/Event.h"
#include "taskgraph/GraphTask.h"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <future>
#include <stb/stb_image.h>
#include <meshoptimizer.h>

#include "shaderheaders/shared/Geometry.h"
#include "shaderheaders/shared/utils/Packing.h"

namespace Moer::Resource::Gltf {

    struct Light {
        Vector4f color;
        Vector4f position;
        Vector4f direction;
        Vector4f info;
    };

    struct Parser::Impl {
        UniquePtr<SceneData> LoadSceneFromFile(const std::filesystem::path& file_path, bool _delete_after_load = false);

        void LoadNodes(const aiScene* scene, const aiNode* node, std::function<void(const aiNode*)>& _on_load_node);
        void LoadCameras(const aiScene* scene);
        void LoadMaterial(const aiScene* _ai_scene, const aiMaterial* _ai_material, const std::string& _material_name);
        void LoadTexture(const aiScene* scene, const aiString& texture_path, MaterialInstanceRef& mat, const std::string& param_name);
        void LoadLights(const aiScene* scene);
        ~Impl() = default;

        std::filesystem::path m_file_parent_path{};

        UniquePtr<SceneData> data = UniquePtr<SceneData>(MoerNew(SceneData));
    };

    Transform GetTransform(const aiNode* node);

    uint32_t GetVertexData(const aiMesh* mesh, float* data) {
        //  Moer::Array<float> data;
        bool   has_position = mesh->HasPositions();
        bool   has_normal   = mesh->HasNormals();
        bool   has_tangent  = mesh->HasTangentsAndBitangents();
        bool   has_uv       = mesh->HasTextureCoords(0);
        size_t stride       = 0;
        size_t attr_offset[4];
        if (has_position) {
            attr_offset[0] = stride;
            stride += 3;
        }
        if (has_normal) {
            attr_offset[1] = stride;
            stride += 3;
        }
        if (has_tangent) {
            attr_offset[2] = stride;
            stride += 3;
        }
        if (has_uv) {
            attr_offset[3] = stride;
            stride += 2;
            // FIXME: Assimp库的uv貌似是float3来着，步长应该从2改为3。但我看了下，不太敢修，后面Meshlet还有用到，怕修完爆炸了
        }

        uint32_t vertex_num = mesh->mNumVertices;
        // data.resize(vertex_num * stride);
        for (uint32_t i = 0; i < vertex_num; i++) {
            if (has_position) {
                auto* const copy_src = reinterpret_cast<float*>(mesh->mVertices + i);
                std::copy(copy_src, copy_src + 3, data + attr_offset[0] + i * stride);
            }
            if (has_normal) {
                auto* const copy_src = reinterpret_cast<float*>(mesh->mNormals + i);
                std::copy(copy_src, copy_src + 3, data + attr_offset[1] + i * stride);
            }
            if (has_tangent) {
                auto* copy_src = reinterpret_cast<float*>(mesh->mTangents + i);
                std::copy(copy_src, copy_src + 3, data + attr_offset[2] + i * stride);
                // copy_src = reinterpret_cast<float*>(mesh->mBitangents + i);
                // std::copy(copy_src, copy_src + 3, data + attr_offset[2] + i * stride + 3);
            }
            if (has_uv) {
                auto* const copy_src = reinterpret_cast<float*>(mesh->mTextureCoords[0] + i);
                std::copy(copy_src, copy_src + 2, data + attr_offset[3] + i * stride);
            }
        }

        return vertex_num;
    }

    uint32_t GetIndexData(const aiMesh* mesh, uint32_t* data) {
        uint32_t offset = 0;
        if (mesh->HasFaces()) {
            for (uint32_t i = 0; i < mesh->mNumFaces; i++) {
                const auto& face = mesh->mFaces[i];
                std::copy_n(face.mIndices, face.mNumIndices, data + offset);
                offset += face.mNumIndices;
            }
        }
        return offset;
    }

    VertexAttributeFlags GetAttribute(const aiMesh* mesh, uint32_t& stride) {
        stride                         = 0;
        VertexAttributeFlags attribute = 0;
        if (mesh->HasPositions()) {
            attribute |= EVertexAttributeFlags::E_POSITION;
            stride += 3;
        }
        if (mesh->HasNormals()) {
            attribute |= EVertexAttributeFlags::E_NORMAL;
            stride += 3;
        }
        if (mesh->HasTangentsAndBitangents()) {
            attribute |= EVertexAttributeFlags::E_TANGENT;
            // attribute |= E_VERTEX_ATTRIBUTE::E_BITANGENT;
            stride += 3;
        }
        if (mesh->HasTextureCoords(0)) {
            attribute |= EVertexAttributeFlags::E_UV0;
            stride += 2;
        }
        return attribute;
    }

    int32_t GetEmbeddedTextureId(const aiString& path) {
        const char* pathStr = path.C_Str();
        if (path.length >= 2 && pathStr[0] == '*') {
            for (int i = 1; i < path.length; i++) {
                if (!isdigit(pathStr[i])) {
                    return -1;
                }
            }
            return std::atoi(pathStr + 1);// NOLINT
        }
        return -1;
    }

    void Parser::Impl::LoadTexture(const aiScene* scene, const aiString& texture_path, MaterialInstanceRef& mat, const std::string& param_name) {
        if (data->m_textures.contains(texture_path.C_Str())) {
            auto texture = data->m_textures[texture_path.C_Str()];
            data->m_mat_instance_textures[mat->GetName()].textures.push_back({param_name, texture_path.C_Str()});
            SamplerParams params{};
            params.max_mip_level = texture.mips;
            return;
        }

        int32_t         embedded_id = GetEmbeddedTextureId(texture_path);
        TextureBuilder* builder     = MoerNew(TextureBuilder);
        ImageReadDesc   image_desc;

        if (embedded_id >= 0) {
            const aiTexture* texture = scene->mTextures[embedded_id];
            image_desc               = ImageIO::ReadFromMemory(reinterpret_cast<unsigned char*>(texture->pcData), texture->mWidth * texture->mHeight * 4);
            //todo
        } else {
            std::filesystem::path texture_file_path = m_file_parent_path / texture_path.C_Str();
            image_desc                              = ImageIO::ReadFromFile(texture_file_path);
        }

        if (!image_desc.IsValid()) {
            LOG_WARNING("Load Texture Failed:{}", texture_path.C_Str());
            return;
        }

        TextureData texture_data;
        texture_data.mips      = image_desc.mips;
        texture_data.layers    = image_desc.layers;
        texture_data.width     = image_desc.width;
        texture_data.height    = image_desc.height;
        texture_data.channal   = image_desc.channal;
        texture_data.format    = image_desc.format;
        texture_data.data_size = image_desc.data_size;
        texture_data.data.resize(image_desc.data_size);
        std::copy_n(reinterpret_cast<uint8_t*>(image_desc.data), image_desc.data_size, texture_data.data.data());
        texture_data.mip_offsets = image_desc.mip_offsets;
        texture_data.mip_extents = image_desc.mip_extents;

        if (image_desc.data_callback != nullptr) {
            image_desc.data_callback(image_desc.data);
        }

        data->m_textures[texture_path.C_Str()] = texture_data;
        data->m_mat_instance_textures[mat->GetName()].textures.push_back({param_name, texture_path.C_Str()});
        LOG_INFO("Load Texture Success:{}", texture_path.C_Str());
    }

    static Vector3f ToVector3f(const aiVector3D& vec) {
        return {vec.x, vec.y, vec.z};
    }

    static Vector3f ToVector3f(const aiColor3D& vec) {
        return {vec.r, vec.g, vec.b};
    }

    void Parser::Impl::LoadCameras(const aiScene* scene) {
        const uint32_t camera_num = scene->mNumCameras;
        if (camera_num == 0) {
            LOG_INFO("No camera found, create default camera");
            data->m_cameras.push_back(Camera::CreateDefaultCamera());
        } else {
            for (uint32_t i = 0; i < camera_num; i++) {
                const auto* camera = scene->mCameras[i];
                Vector4f    position(camera->mPosition.x, camera->mPosition.y, camera->mPosition.z, 1.f);
                Vector4f    lookAt_vector(camera->mLookAt.x, camera->mLookAt.y, camera->mLookAt.z, 0.f);
                Vector4f    up(camera->mUp.x, camera->mUp.y, camera->mUp.z, 0.f);
                aiNode*     camera_node           = scene->mRootNode->FindNode(camera->mName);
                Transform   camera_node_transform = GetTransform(camera_node);

                Vector4f world_position      = camera_node_transform * position;
                Vector4f world_lookAt_vector = camera_node_transform * lookAt_vector;
                Vector4f world_up            = camera_node_transform * up;
                Vector4f world_lookAt_point  = world_position + world_lookAt_vector;

                CameraRef camera_ref  = MoerNew(Camera)();
                Transform transform   = Transform();
                auto      world_2_cam = Transform(Vector3f(world_position), Vector3f(world_lookAt_point), Vector3f(world_up));
                transform.matrix      = Inverse(world_2_cam.GetMatrix4x4());

                // The interpretation of 'mHorizontalFOV' is inconsistent among gltf2, fbx, and the documentation in the official Assimp version (5.4.2).
                // In this project, we ensure 'mHorizontalFOV' is the 'half' of the horizontal field of view angle (at least in GLTF2 and FBX).
                float full_yfov_deg = AI_RAD_TO_DEG(2 * atan(tan(camera->mHorizontalFOV) / camera->mAspect));
                camera_ref->Initialize(
                    transform,
                    full_yfov_deg,
                    camera->mAspect,
                    camera->mClipPlaneNear,
                    camera->mClipPlaneFar);
                data->m_cameras.push_back(camera_ref);

                // LOG_INFO("Camera: {}", camera_ref->ToString());
            }
        }
    }

    /**
     * Load lights from gltf scene
     * Refer to: https://assimp-docs.readthedocs.io/en/latest/API/API-Documentation.html#_CPPv47aiLight
     */
    void Parser::Impl::LoadLights(const aiScene* scene) {
        const uint32_t light_num = scene->mNumLights;
        if (light_num == 0) {
            LOG_INFO("No lights found, loader will use default lights");
            data->m_lights = std::move(LightComponent::CreateDefaultLightComponents());
        } else {
            LOG_INFO("Found {} lights in the scene", light_num);

            LOG_WARNING("gltf LoadLights function isn't tested fully. It may not work as expected.");
            // The following code isn't tested fully. It may not work as expected.
            // TODO: Add a new scene with lights to test the following code
            for (uint32_t i = 0; i < light_num; i++) {
                const auto* light = scene->mLights[i];
                if (light->mType == aiLightSourceType::aiLightSource_DIRECTIONAL) {
                    LightComponentRef light_component = MoerNew(DirectionalLightComponent)(
                        ToVector3f(light->mColorDiffuse),// color
                        1.0f,                            // intensity
                        ToVector3f(light->mDirection),   // direction
                        0.f                              // angular_size
                    );
                    data->m_lights.push_back(light_component);

                } else if (light->mType == aiLightSourceType::aiLightSource_POINT) {
                    LightComponentRef light_component = MoerNew(PointLightComponent)(
                        ToVector3f(light->mColorDiffuse),// color
                        1.0f,                            // intensity
                        ToVector3f(light->mPosition)     // position
                    );
                    data->m_lights.push_back(light_component);

                } else if (light->mType == aiLightSourceType::aiLightSource_SPOT) {
                    LightComponentRef light_component = MoerNew(SpotLightComponent)(
                        ToVector3f(light->mColorDiffuse),// color
                        1.0f,                            // intensity
                        ToVector3f(light->mPosition),    // position
                        ToVector3f(light->mDirection),   // direction
                        light->mAngleInnerCone,          // inner_cone_angle
                        light->mAngleOuterCone           // outer_cone_angle
                    );
                    data->m_lights.push_back(light_component);

                } else if (light->mType == aiLightSourceType::aiLightSource_AMBIENT) {
                    LOG_WARNING("Unsupported light type `Ambient Light` in loading scene");

                } else if (light->mType == aiLightSourceType::aiLightSource_AREA) {
                    LOG_WARNING("Unsupported light type `Area Light` in loading scene");
                }
            }
        }
    }

    MaterialRef GetDefaultMaterial() {
        MaterialBuilder materialBuilder{};
        MaterialRef     default_material = MoerNew(Material)();
        materialBuilder.SetParameter("base_color_factor", UniformType::FLOAT4);
        materialBuilder.SetParameter("emissive_factor", UniformType::FLOAT3);
        materialBuilder.SetParameter("metalic_factor", UniformType::FLOAT);
        materialBuilder.SetParameter("roughness_factor", UniformType::FLOAT);
        materialBuilder.SetParameter("ao", UniformType::FLOAT);

        materialBuilder.SetTexture("albedo_map", ETextureDimension::TEX_2D);
        materialBuilder.SetTexture("normal_map", ETextureDimension::TEX_2D);
        materialBuilder.SetTexture("metallic_roughness_map", ETextureDimension::TEX_2D);
        materialBuilder.SetTexture("ao_map", ETextureDimension::TEX_2D);
        materialBuilder.SetTexture("emissive_map", ETextureDimension::TEX_2D);
        materialBuilder.SetType(EMaterialType::E_PBR_STANDARD);
        materialBuilder.SetName("standered");

        return materialBuilder.Build();
    }

    void Parser::Impl::LoadMaterial(const aiScene* _ai_scene, const aiMaterial* _ai_material, const std::string& _material_name) {

        if (!data->m_materials.contains("standered")) {
            data->m_materials["standered"] = GetDefaultMaterial();
        }
        const auto material = data->m_materials["standered"];

        MaterialInstanceRef mi                            = data->m_material_instances.emplace_back(material->CreateInstance());
        uint                load_idx                      = data->m_material_instances.size() - 1;
        data->m_material_instance_indexes[_material_name] = load_idx;

        mi->SetName(_material_name);

        aiString base_color_path, normal_path, metallic_roughness_path, ao_path, emissive_path;
        if (_ai_material->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &base_color_path) == AI_SUCCESS) {
            LoadTexture(_ai_scene, base_color_path, mi, "albedo_map");
        }
        if (_ai_material->GetTexture(aiTextureType_NORMALS, 0, &normal_path) == AI_SUCCESS) {
            LoadTexture(_ai_scene, normal_path, mi, "normal_map");
        }
        if (_ai_material->GetTexture(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, &metallic_roughness_path) == AI_SUCCESS) {
            LoadTexture(_ai_scene, metallic_roughness_path, mi, "metallic_roughness_map");
        }
        if (_ai_material->GetTexture(aiTextureType_LIGHTMAP, 0, &ao_path) == AI_SUCCESS) {
            LoadTexture(_ai_scene, ao_path, mi, "ao_map");
        }
        if (_ai_material->GetTexture(aiTextureType_EMISSIVE, 0, &emissive_path) == AI_SUCCESS) {
            LoadTexture(_ai_scene, emissive_path, mi, "emissive_map");
        }

        aiColor4D base_color_factor;
        aiColor3D emissive_factor;
        float     metallic_factor  = 1.0;
        float     roughness_factor = 1.0;

        if (_ai_material->Get(AI_MATKEY_COLOR_DIFFUSE, base_color_factor) == AI_SUCCESS) {
            Vector4f base_color_factor_cast = *reinterpret_cast<Vector4f*>(&base_color_factor);
            mi->SetParameter("base_color_factor", base_color_factor_cast);
        }

        if (_ai_material->Get(AI_MATKEY_COLOR_EMISSIVE, emissive_factor) == AI_SUCCESS) {
            Vector3f emissive_factor_cast = *reinterpret_cast<Vector3f*>(&emissive_factor);
            mi->SetParameter("emissive_factor", emissive_factor_cast);
        }

        if (_ai_material->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLIC_FACTOR, metallic_factor) == AI_SUCCESS) {
            mi->SetParameter("metalic_factor", metallic_factor);
        }

        if (_ai_material->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_ROUGHNESS_FACTOR, roughness_factor) == AI_SUCCESS) {
            mi->SetParameter("roughness_factor", roughness_factor);
        }

        mi->SetParameter("ao", 1.0f);
    }
    static float CalculateWorldToUvUnits(const RTVertex& _v0, const RTVertex& _v1, const RTVertex& _v2) {

        float3 p0(_v0.position);
        float3 p1(_v1.position);
        float3 p2(_v2.position);

        float3 edge20          = p2 - p0;
        float3 edge10          = p1 - p0;
        float3 triangle_normal = Cross(edge20, edge10);
        float  world_area      = std::max((float)Length(triangle_normal), 1e-9f);

        float3 uv_edge20 = float3(_v2.uv0, _v2.uv1, 0.0f) - float3(_v0.uv0, _v0.uv1, 0.0f);
        float3 uv_edge10 = float3(_v1.uv0, _v1.uv1, 0.0f) - float3(_v0.uv0, _v0.uv1, 0.0f);

        float3 uv_normal = Cross(uv_edge20, uv_edge10);
        float  uv_area   = Length(Cross(uv_edge20, uv_edge10));

        return uv_area == 0 ? 1.0f : sqrt(uv_area / world_area);
    }

    using GeomSet = Moer::UnorderedSet<aiMesh*>;

    struct GeomSetHash {
        size_t operator()(const GeomSet& _set) const {
            size_t hash = 0;
            for (auto& mesh : _set) {
                hash ^= std::hash<aiMesh*>{}(mesh);
            }
            return hash;
        }
    };

    struct GeomSetEqual {
        bool operator()(const GeomSet& _lhs, const GeomSet& _rhs) const {
            if (_lhs.size() != _rhs.size()) {
                return false;
            }
            for (auto& mesh : _lhs) {
                if (_rhs.find(mesh) == _rhs.end()) {
                    return false;
                }
            }
            return true;
        }
    };
    using GeomRecord = UnorderedMap<GeomSet, SharedPtr<MeshInfo>, GeomSetHash, GeomSetEqual>;

    UniquePtr<SceneData> Parser::Impl::LoadSceneFromFile(const std::filesystem::path& _file_path, bool _delete_after_load) {

        GpuPrimitiveBuilder::InitBuild();
        Assimp::Importer importer;
        auto             real_path = std::filesystem::weakly_canonical(_file_path);
        if (!std::filesystem::exists(real_path)) {
            LOG_WARNING("File not exist: {}", real_path.string());
            LOG_WARNING("Please check the `scenepath` in source/configs/MoerEngine.ini.");
            return nullptr;
        }
        const auto* gltf_scene = importer.ReadFile(_file_path.string(), aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenBoundingBoxes | aiProcess_GenNormals | aiProcess_CalcTangentSpace);
        if (!gltf_scene) {
            LOG_WARNING("Failed to load gltf file: {} ", _file_path.string());
            return nullptr;
        }

        m_file_parent_path = _file_path.parent_path();

        // MARK: Prepare

        // Assume all mesh has same attribute
        uint32_t stride;
        auto     attribute = GetAttribute(gltf_scene->mMeshes[0], stride);
        stride *= sizeof(float);

        // MARK: Part 0 Generate Meshlet (Deprecated)
        //
        // Meshlet is deprecated in current version, needs to be re-implemented.
        // You can refer to the old implementation in this commit (browse the file in this commit): 937c1dac60c94d602f000e52f927e33b40a19dae

        // MARK: Part 1 LoadNode Prev
        // The following code obtains these data:
        //   - m_instance_infos, m_mesh_instances, m_mesh_infos
        //   - all about materials & textures (m_materials, m_material_instances, m_material_instance_indexes, m_textures)

        data->m_mesh_infos.reserve(gltf_scene->mNumMeshes);

        GeomRecord                        geom_record;
        UnorderedMap<const aiNode*, uint> node2instance;

        uint total_vtx_cnt        = 0;
        uint total_idx_cnt        = 0;
        uint geom_instance_offset = 0;
        {
            std::function<void(const aiNode*)> on_load_node_prev = [&](const aiNode* _node) {
                if (_node->mMeshes) {
                    GeomSet geo_set;

                    MeshInstance& mesh_instance    = data->m_mesh_instances.emplace_back();
                    mesh_instance.instance_id      = data->m_mesh_instances.size() - 1;
                    mesh_instance.geom_instance_id = geom_instance_offset;
                    node2instance[_node]           = mesh_instance.instance_id;

                    for (uint32_t i = 0; i < _node->mNumMeshes; i++) {
                        auto* mesh = gltf_scene->mMeshes[_node->mMeshes[i]];
                        geo_set.insert(mesh);
                    }
                    auto geo_iter = geom_record.find(geo_set);

                    if (geo_iter == geom_record.end()) {
                        uint  local_vtx_cnt = 0;
                        uint  local_idx_cnt = 0;
                        Box3D bounding_box;

                        SharedPtr<MeshInfo>& mesh_info = data->m_mesh_infos.emplace_back(MakeShared<MeshInfo>());
                        mesh_info->geometries.resize(geo_set.size());
                        mesh_info->global_mesh_idx = data->m_mesh_infos.size() - 1;
                        mesh_info->name            = _node->mName.C_Str();

                        for (uint32_t i = 0; i < _node->mNumMeshes; i++) {
                            auto* mesh = gltf_scene->mMeshes[_node->mMeshes[i]];

                            Box3D current_box{
                                {mesh->mAABB.mMin.x, mesh->mAABB.mMin.y, mesh->mAABB.mMin.z},
                                {mesh->mAABB.mMax.x, mesh->mAABB.mMax.y, mesh->mAABB.mMax.z}};
                            bounding_box.Expand(current_box);
                            uint material_id = mesh->mMaterialIndex;

                            aiMaterial const* material = gltf_scene->mMaterials[material_id];

                            aiString    name;
                            std::string material_name;

                            if (material->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
                                material_name = name.C_Str();
                            } else {
                                material_name = std::string("_default_material_Name") + std::to_string(material_id);
                            }

                            // m_material_instance_indexes[material_name] = material_id;

                            if (!data->m_material_instance_indexes.contains(material_name)) {
                                LoadMaterial(gltf_scene, material, material_name);
                            }

                            SharedPtr<MeshGeometry> mesh_geometry = data->m_mesh_geometries.emplace_back(MakeShared<MeshGeometry>());
                            mesh_geometry->local_idx_count        = mesh->mNumFaces * 3;
                            mesh_geometry->local_vtx_count        = mesh->mNumVertices;
                            mesh_geometry->local_idx_offset       = local_idx_cnt;
                            mesh_geometry->local_vtx_offset       = local_vtx_cnt;
                            mesh_geometry->material_id            = data->m_material_instance_indexes[material_name];
                            mesh_geometry->global_geom_idx        = data->m_mesh_geometries.size() - 1;

                            local_vtx_cnt += mesh->mNumVertices;
                            local_idx_cnt += mesh->mNumFaces * 3;

                            mesh_info->geometries[i] = mesh_geometry;
                        }

                        mesh_info->vtx_offset   = total_vtx_cnt;
                        mesh_info->idx_offset   = total_idx_cnt;
                        mesh_info->vtx_count    = local_vtx_cnt;
                        mesh_info->idx_count    = local_idx_cnt;
                        mesh_info->bounding_box = bounding_box;

                        //for serialization
                        mesh_info->geom_start_idx = data->m_mesh_geometries.size() - _node->mNumMeshes;

                        total_vtx_cnt += local_vtx_cnt;
                        total_idx_cnt += local_idx_cnt;

                        geom_record[geo_set] = mesh_info;
                    }
                    geo_iter = geom_record.find(geo_set);

                    Render::InstanceData& inst   = data->m_instance_infos.emplace_back();
                    inst.geom_count              = _node->mNumMeshes;
                    inst.first_geom_idx          = geo_iter->second->geometries[0]->global_geom_idx;
                    inst.first_geom_instance_idx = mesh_instance.geom_instance_id;
                    inst.model2world             = GetTransform(_node).GetMatrix3x4();
                    inst.prev_model2world        = inst.model2world;

                    //for serialization
                    mesh_instance.mesh_info_idx = geo_iter->second->global_mesh_idx;
                    mesh_instance.mesh_info     = geo_iter->second;
                    geom_instance_offset += _node->mNumMeshes;
                }
            };
            LoadNodes(gltf_scene, gltf_scene->mRootNode, on_load_node_prev);
        }

        // MARK: Part 2 LoadNode Post
        // The following code obtains these data:
        //   - m_mesh_buffers

        {
            SharedPtr<MeshBuffers> mesh_buffer = MakeShared<MeshBuffers>();
            data->m_mesh_buffers.emplace_back(mesh_buffer);

            mesh_buffer->vertex_factory_buffers = VertexFactoryBuffers(
                {EVertexAttributes::VA_POSITION, EVertexAttributes::VA_NORMAL, EVertexAttributes::VA_TANGENT, EVertexAttributes::VA_TEXCOORD0});

            // create temp buffers
            Array<float3> position_buffer(total_vtx_cnt);
            Array<uint>   normal_buffer(total_vtx_cnt);
            Array<uint>   tangent_buffer(total_vtx_cnt);
            Array<float2> texcoord0_buffer(total_vtx_cnt);

            mesh_buffer->indices.resize(total_idx_cnt);

            total_vtx_cnt = 0;
            total_idx_cnt = 0;

            geom_record.clear();

            std::function<void(const aiNode*)> on_load_node_post = [&](const aiNode* _node) {
                if (!_node->mMeshes) return;
                uint idx = node2instance[_node];

                const auto& instance  = data->m_mesh_instances[idx];
                auto&       mesh_info = data->m_mesh_infos[instance.mesh_info_idx];

                //check if need loading
                GeomSet geo_set;
                for (uint32_t i = 0; i < _node->mNumMeshes; i++) {
                    auto* mesh = gltf_scene->mMeshes[_node->mMeshes[i]];
                    geo_set.insert(mesh);
                }

                auto geo_iter = geom_record.find(geo_set);
                //already loaded
                if (geo_iter != geom_record.end()) {
                    return;
                }
                geom_record[geo_set] = mesh_info;

                mesh_info->buffers = mesh_buffer;
                mesh_info->buf_idx = data->m_mesh_buffers.size() - 1;// For serialization

                for (uint32_t i = 0; i < _node->mNumMeshes; i++) {

                    const auto* mesh = gltf_scene->mMeshes[_node->mMeshes[i]];

                    for (uint32_t j = 0; j < mesh->mNumVertices; j++) {
                        if (mesh->HasPositions()) {
                            const auto& pos                    = mesh->mVertices[j];
                            position_buffer[total_vtx_cnt + j] = {pos.x, pos.y, pos.z};
                        } else {
                            assert(false && "Mesh has no position");
                        }

                        if (mesh->HasNormals()) {
                            const auto& nor                  = mesh->mNormals[j];
                            float3      normal               = {nor.x, nor.y, nor.z};
                            normal_buffer[total_vtx_cnt + j] = Pack_RGB8_SNORM(normal);
                        }
                        if (mesh->HasTangentsAndBitangents()) {
                            const auto& tan                   = mesh->mTangents[j];
                            float3      tangent               = {tan.x, tan.y, tan.z};
                            tangent_buffer[total_vtx_cnt + j] = Pack_RGB8_SNORM(tangent);
                        }
                        if (mesh->HasTextureCoords(0)) {
                            const auto& uv0                     = mesh->mTextureCoords[0][j];
                            texcoord0_buffer[total_vtx_cnt + j] = {uv0.x, uv0.y};
                        }
                    }
                    for (uint32_t j = 0; j < mesh->mNumFaces; j++) {
                        const auto& face                                = mesh->mFaces[j];
                        mesh_buffer->indices[total_idx_cnt + j * 3 + 0] = face.mIndices[0];
                        mesh_buffer->indices[total_idx_cnt + j * 3 + 1] = face.mIndices[1];
                        mesh_buffer->indices[total_idx_cnt + j * 3 + 2] = face.mIndices[2];
                    }

                    total_vtx_cnt += mesh->mNumVertices;
                    total_idx_cnt += mesh->mNumFaces * 3;
                }
            };

            LoadNodes(gltf_scene, gltf_scene->mRootNode, on_load_node_post);

            // use temp buffers to initialize vertex_factory_buffers
            assert(total_vtx_cnt == position_buffer.size());
            mesh_buffer->vertex_factory_buffers.AssignAllBuffers({
                {EVertexAttributes::VA_POSITION, position_buffer.data(), position_buffer.size()},
                {EVertexAttributes::VA_NORMAL, normal_buffer.data(), normal_buffer.size()},
                {EVertexAttributes::VA_TANGENT, tangent_buffer.data(), tangent_buffer.size()},
                {EVertexAttributes::VA_TEXCOORD0, texcoord0_buffer.data(), texcoord0_buffer.size()},
            });

            mesh_buffer->FillRanges();
        }

        // MARK: Part 3 Camera&Light

        LoadCameras(gltf_scene);
        LoadLights(gltf_scene);

        // MARK: Path
        data->m_path = _file_path.string();

        /**
         * Old code:
         * 
         * // Basic information:
         * m_scene_data->m_path = _file_path.string();
         * 
         * // Data obtained from Part 1:
         * m_scene_data->m_meshlet_descs  = std::move(m_meshlet_descs);
         * m_scene_data->m_meshlet_bounds = std::move(m_meshlet_bounds);
         * 
         * // Data obtained from Part 2:
         * m_scene_data->m_instance_infos = std::move(m_instance_infos);
         * m_scene_data->m_mesh_instances = std::move(m_mesh_instances);
         * 
         * m_scene_data->m_mesh_infos      = std::move(m_mesh_infos);
         * m_scene_data->m_mesh_geometries = std::move(m_mesh_geometries);
         * 
         * m_scene_data->m_materials                 = std::move(m_materials);
         * m_scene_data->m_material_instances        = std::move(m_material_instances);
         * m_scene_data->m_material_instance_indexes = std::move(m_material_instance_indexes);
         * m_scene_data->m_mat_instance_textures     = std::move(m_mat_instance_textures);
         * m_scene_data->m_textures                  = std::move(m_textures);
         * 
         * // Data obtained from Part 3:
         * m_scene_data->m_mesh_buffers = std::move(m_mesh_buffers);
         * 
         * // Data obtained from Part 4:
         * m_scene_data->m_cameras = std::move(m_cameras);
         * m_scene_data->m_lights  = std::move(m_lights);
         */

        auto scene_data = std::move(data);

        //todo after build all    end build
        // GpuPrimitiveBuilder::EndBuild();
        if (_delete_after_load) {
            MoerDelete(this);
        }
        return scene_data;
    }
    using TPromise = std::promise<std::unique_ptr<Scene, MoerDeleter>>;
    using TScene   = std::unique_ptr<Scene, MoerDeleter>;

    struct PromiseWrapper : public Countable {
        TPromise m_promise;
        explicit PromiseWrapper() {
        }
        ~PromiseWrapper() {
        }
        void Destroy() override {
            delete this;
        }
        void SetValue(TScene&& _scene) {
            m_promise.set_value(std::move(_scene));
        }
        auto GetFuture() {
            return m_promise.get_future();
        }
    };

    Transform GetTransform(const aiNode* node) {
        Matrix4x4f matrix;
        for (uint32_t i = 0; i < 4; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                matrix[i][j] = node->mTransformation[i][j];
            }
        }
        if (node->mParent) {
            auto parent_matrix = GetTransform(node->mParent);
            matrix             = parent_matrix.GetMatrix4x4() * matrix;
        }
        return {matrix};
    }

    void Parser::Impl::LoadNodes(const aiScene* _scene, const aiNode* _node, std::function<void(const aiNode*)>& _on_load_node) {
        for (uint32_t i = 0; i < _node->mNumChildren; i++) {
            LoadNodes(_scene, _node->mChildren[i], _on_load_node);
        }
        if (_node->mMeshes) {
            _on_load_node(_node);
        }
    }

    UniquePtr<SceneData> Parser::LoadSceneFromFile(const std::filesystem::path& file_path) noexcept {

        //  auto lights = scene->mLights;
        Impl impl;
        return std::move(UniquePtr<SceneData>(impl.LoadSceneFromFile(file_path.generic_string().data())));
    }

}// namespace Moer::Resource::Gltf