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
#include <future>
#include <stb/stb_image.h>
#include <meshoptimizer.h>

namespace Moer::Resource::Gltf {

    struct Light {
        Vector4f color;
        Vector4f position;
        Vector4f direction;
        Vector4f info;
    };

    struct Parser::Impl {
        UniquePtr<SceneData> LoadSceneFromFile(const std::filesystem::path& file_path, bool _delete_after_load = false);
        void                 LoadSceneFromFileAsync(const std::filesystem::path& file_path);
        void                 LoadNode(const aiNode* node, const aiScene* scene);
        void                 LoadCameras(const aiScene* scene);
        void                 LoadMaterial(const aiScene* ai_scene, const aiMaterial* ai_material, const std::string& materialName);
        void                 LoadTexture(const aiScene* scene, const aiString& texture_path, MaterialInstanceRef& mat, const std::string& param_name);
        void                 LoadLights(const aiScene* scene);
        ~Impl() = default;

        // Moer::Array<RHIRenderPrimitiveRef>                   m_primitives{};
        Moer::UnorderedMap<std::string, TextureData>         m_textures{};
        Moer::UnorderedMap<std::string, MaterialInstanceRef> m_material_instances{};
        Moer::UnorderedMap<std::string, MaterialRef>         m_materials{};
        Moer::Array<MeshInfo>                                m_mesh_infos{};
        Moer::Array<InstanceData>                            m_instance_data{};
        Moer::Array<InstanceMeshInfo>                        m_instance_mesh_info{};

        Moer::Array<MeshletDesc>  m_meshlet_descs{};
        Moer::Array<MeshletBound> m_meshlet_bounds{};
        //temperaly store vertex and index data
        Moer::Array<float>    m_vertex_data{};
        Moer::Array<uint32_t> m_index_data{};

        std::filesystem::path m_file_parent_path{};
        UniquePtr<SceneData>  m_scene_data;
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
            attribute |= E_VERTEX_ATTRIBUTE::E_POSITION;
            stride += 3;
        }
        if (mesh->HasNormals()) {
            attribute |= E_VERTEX_ATTRIBUTE::E_NORMAL;
            stride += 3;
        }
        if (mesh->HasTangentsAndBitangents()) {
            attribute |= E_VERTEX_ATTRIBUTE::E_TANGENT;
            // attribute |= E_VERTEX_ATTRIBUTE::E_BITANGENT;
            stride += 3;
        }
        if (mesh->HasTextureCoords(0)) {
            attribute |= E_VERTEX_ATTRIBUTE::E_UV0;
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
        if (m_textures.contains(texture_path.C_Str())) {
            auto texture = m_textures[texture_path.C_Str()];
            m_scene_data->m_mat_instance_textures[mat->GetName()].textures.push_back({param_name, texture_path.C_Str()});
            SamplerParams params{};
            params.max_mip_level = texture.mips;
            //  mat->SetParameter("defulat_sampler", params);
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

        m_textures[texture_path.C_Str()] = texture_data;
        m_scene_data->m_mat_instance_textures[mat->GetName()].textures.push_back({param_name, texture_path.C_Str()});
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
            m_scene_data->m_cameras.push_back(Camera::CreateDefaultCamera());
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
                camera_ref->SetFov(full_yfov_deg);
                camera_ref->SetWorldTransform(transform);
                camera_ref->SetNearClip(camera->mClipPlaneNear);
                camera_ref->SetFarClip(camera->mClipPlaneFar);
                camera_ref->SetAspectRatio(camera->mAspect);
                m_scene_data->m_cameras.push_back(camera_ref);

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
            m_scene_data->m_lights = std::move(LightComponent::CreateDefaultLightComponents());
        } else {
            LOG_INFO("Found {} lights in the scene", light_num);

            LOG_WARNING("Due to the Sponza scene only has a dark light, loader will use default lights instead. Please remove this warning and the following code after adding a new scene with lights");
            m_scene_data->m_lights = std::move(LightComponent::CreateDefaultLightComponents());
            return;

            // The following code isn't tested fully. It may not work as expected.
            // TODO: Add a new scene with lights to test the following code
            for (uint32_t i = 0; i < light_num; i++) {
                const auto* light = scene->mLights[i];
                if (light->mType == aiLightSourceType::aiLightSource_DIRECTIONAL) {
                    LightComponentRef light_component = MoerNew(DirectionalLightComponent)(
                        ToVector3f(light->mColorDiffuse),// color
                        1.0f,                            // intensity
                        ToVector3f(light->mDirection)    // direction
                    );
                    m_scene_data->m_lights.push_back(light_component);

                } else if (light->mType == aiLightSourceType::aiLightSource_POINT) {
                    LightComponentRef light_component = MoerNew(PointLightComponent)(
                        ToVector3f(light->mColorDiffuse),// color
                        1.0f,                            // intensity
                        ToVector3f(light->mPosition)     // position
                    );
                    m_scene_data->m_lights.push_back(light_component);

                } else if (light->mType == aiLightSourceType::aiLightSource_SPOT) {
                    LightComponentRef light_component = MoerNew(SpotLightComponent)(
                        ToVector3f(light->mColorDiffuse),// color
                        1.0f,                            // intensity
                        ToVector3f(light->mPosition),    // position
                        ToVector3f(light->mDirection),   // direction
                        light->mAngleInnerCone,          // inner_cone_angle
                        light->mAngleOuterCone           // outer_cone_angle
                    );
                    m_scene_data->m_lights.push_back(light_component);

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

    void Parser::Impl::LoadMaterial(const aiScene* ai_scene, const aiMaterial* ai_material, const std::string& materialName) {

        if (!m_materials.contains("standered")) {
            m_materials["standered"] = GetDefaultMaterial();
        }
        const auto material = m_materials["standered"];

        m_material_instances[materialName] = material->CreateInstance();
        MaterialInstanceRef mi             = m_material_instances[materialName];
        mi->SetName(materialName);

        aiString base_color_path, normal_path, metallic_roughness_path, ao_path, emissive_path;
        if (ai_material->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &base_color_path) == AI_SUCCESS) {
            LoadTexture(ai_scene, base_color_path, mi, "albedo_map");
        }
        if (ai_material->GetTexture(aiTextureType_NORMALS, 0, &normal_path) == AI_SUCCESS) {
            LoadTexture(ai_scene, normal_path, mi, "normal_map");
        }
        if (ai_material->GetTexture(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, &metallic_roughness_path) == AI_SUCCESS) {
            LoadTexture(ai_scene, metallic_roughness_path, mi, "metallic_roughness_map");
        }
        if (ai_material->GetTexture(aiTextureType_LIGHTMAP, 0, &ao_path) == AI_SUCCESS) {
            LoadTexture(ai_scene, ao_path, mi, "ao_map");
        }
        if (ai_material->GetTexture(aiTextureType_EMISSIVE, 0, &emissive_path) == AI_SUCCESS) {
            LoadTexture(ai_scene, emissive_path, mi, "emissive_map");
        }

        aiColor4D baseColorFactor;
        aiColor3D emissiveFactor;
        float     metallicFactor  = 1.0;
        float     roughnessFactor = 1.0;

        if (ai_material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColorFactor) == AI_SUCCESS) {
            Vector4f base_color_factor_cast = *reinterpret_cast<Vector4f*>(&baseColorFactor);
            mi->SetParameter("base_color_factor", base_color_factor_cast);
        }

        if (ai_material->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveFactor) == AI_SUCCESS) {
            Vector3f emissive_factor_cast = *reinterpret_cast<Vector3f*>(&emissiveFactor);
            mi->SetParameter("emissive_factor", emissive_factor_cast);
        }

        if (ai_material->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLIC_FACTOR, metallicFactor) == AI_SUCCESS) {
            mi->SetParameter("metalic_factor", metallicFactor);
        }

        if (ai_material->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_ROUGHNESS_FACTOR, roughnessFactor) == AI_SUCCESS) {
            mi->SetParameter("roughness_factor", roughnessFactor);
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
    UniquePtr<SceneData> Parser::Impl::LoadSceneFromFile(const std::filesystem::path& _file_path, bool _delete_after_load) {

        GpuPrimitiveBuilder::InitBuild();
        m_scene_data = UniquePtr<SceneData>(MoerNew(SceneData));
        Assimp::Importer importer;
        auto             real_path = std::filesystem::weakly_canonical(_file_path);
        if (!std::filesystem::exists(real_path)) {
            LOG_WARNING("File not exist: {}", real_path.string());
            return nullptr;
        }
        const auto* gltf_scene = importer.ReadFile(_file_path.string(), aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenBoundingBoxes | aiProcess_GenNormals | aiProcess_CalcTangentSpace);
        if (!gltf_scene) {
            LOG_WARNING("Failed to load gltf file: {} ", _file_path.string());
            return nullptr;
        }
        // loadNode()

        // auto moer_scene = std::make_unique<Scene>();
        m_file_parent_path = _file_path.parent_path();

        uint32_t stride;
        //Assume all mesh has same attribute
        auto attribute = GetAttribute(gltf_scene->mMeshes[0], stride);

        stride *= sizeof(float);
        Moer::Array<RTVertex>    rt_vertices;
        Moer::Array<RTMeshInfo>  rt_mesh_infos;
        Moer::Array<RTPrimitvie> rt_prims;
        Moer::Array<uint3>       rt_indices;

        uint rt_vtx_cnt  = 0;
        uint rt_prim_cnt = 0;

        for (uint32_t i = 0; i < gltf_scene->mNumMeshes; i++) {
            const auto* mesh = gltf_scene->mMeshes[i];
            rt_vtx_cnt += mesh->mNumVertices;
            rt_prim_cnt += mesh->mNumFaces;
        }
        rt_vertices.reserve(rt_vtx_cnt);
        rt_mesh_infos.reserve(gltf_scene->mNumMeshes);
        rt_indices.reserve(rt_prim_cnt);
        rt_prims.reserve(rt_prim_cnt);

        rt_vtx_cnt  = 0;
        rt_prim_cnt = 0;

        m_mesh_infos.resize(gltf_scene->mNumMeshes);

        uint32_t vertex_count = 0, index_count = 0, meshlet_count = 0;
        for (uint32_t i = 0; i < gltf_scene->mNumMeshes; i++) {
            const auto* mesh = gltf_scene->mMeshes[i];

            auto aabb_min    = mesh->mAABB.mMin;
            auto aabb_max    = mesh->mAABB.mMax;
            auto aabb_center = (mesh->mAABB.mMin + mesh->mAABB.mMax) * 0.5f;
            auto aabb_extent = mesh->mAABB.mMax - aabb_center;

            uint32_t temp_stride;
            auto     flags = GetAttribute(gltf_scene->mMeshes[i], temp_stride);
            assert(attribute == flags && "Meshes have different attribute");
            assert(temp_stride == stride / 4 && "Meshes have different attribute");
            Moer::Array<float> temp_vertex_data(mesh->mNumVertices * stride / sizeof(float));
            GetVertexData(mesh, temp_vertex_data.data());

            Moer::MeshProcessInput input;
            input.vertex_data   = temp_vertex_data.data();
            input.vertex_count  = mesh->mNumVertices;
            input.vertex_stride = stride;

            Moer::Array<uint32_t> indices;
            indices.reserve(mesh->mNumFaces * 3);// NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
            for (uint32_t i = 0; i < mesh->mNumFaces; i++) {
                const auto& face = mesh->mFaces[i];
                indices.push_back(face.mIndices[0]);
                indices.push_back(face.mIndices[1]);
                indices.push_back(face.mIndices[2]);
            }

            input.index_data  = indices.data();
            input.index_count = indices.size();

            Moer::MeshProcessOutput&& output = Moer::MeshProcessor::GenerateMeshlets(input);

            // vertex_count += mesh->mNumVertices;
            // index_count += mesh->mNumFaces * 3;

            m_meshlet_bounds.insert(m_meshlet_bounds.end(), output.meshlet_bounds.begin(), output.meshlet_bounds.end());
            m_meshlet_descs.insert(m_meshlet_descs.end(), output.meshlets.begin(), output.meshlets.end());

            m_mesh_infos[i] = {.center         = {aabb_center.x, aabb_center.y, aabb_center.z},
                               .vertex_offset  = vertex_count,
                               .extent         = {aabb_extent.x, aabb_extent.y, aabb_extent.z},
                               .index_offset   = index_count,
                               .vertex_count   = (uint32_t)(output.meshlet_vertex_data.size() / (stride / sizeof(float))),
                               .index_count    = (uint32_t)output.primitive_indices.size(),
                               .meshlet_offset = meshlet_count,
                               .meshlet_count  = (uint32_t)output.meshlets.size()};

            m_vertex_data.insert(m_vertex_data.end(), output.meshlet_vertex_data.begin(), output.meshlet_vertex_data.end());
            m_index_data.insert(m_index_data.end(), output.primitive_indices.begin(), output.primitive_indices.end());

            for (size_t vtx_idx = 0; vtx_idx < mesh->mNumVertices; vtx_idx++) {
                RTVertex rt_vertex;
                rt_vertex.position = ToVector3f(mesh->mVertices[vtx_idx]);
                rt_vertex.normal   = ToVector3f(mesh->mNormals[vtx_idx]);
                rt_vertex.uv0      = mesh->mTextureCoords[0][vtx_idx].x;
                rt_vertex.uv1      = mesh->mTextureCoords[0][vtx_idx].y;
                rt_vertex.tangent  = ToVector3f(mesh->mTangents[vtx_idx]);
                rt_vertices.emplace_back(std::move(rt_vertex));
            }

            for (size_t idx_idx = 0; idx_idx < mesh->mNumFaces; idx_idx++) {
                const auto& face = mesh->mFaces[idx_idx];
                rt_indices.emplace_back(uint3(face.mIndices[0], face.mIndices[1], face.mIndices[2]));
                rt_prims.emplace_back(uint3(face.mIndices[0], face.mIndices[1], face.mIndices[2]), CalculateWorldToUvUnits(rt_vertices[face.mIndices[0]], rt_vertices[face.mIndices[1]], rt_vertices[face.mIndices[2]]));
            }

            RTMeshInfo rt_mesh_info{};
            rt_mesh_info.primitive_offset = rt_prim_cnt;
            rt_mesh_info.primitive_count  = mesh->mNumFaces;
            rt_mesh_info.vertex_offset    = rt_vtx_cnt;
            rt_mesh_info.vertex_count     = mesh->mNumVertices;

            rt_mesh_infos.emplace_back(std::move(rt_mesh_info));
            rt_vtx_cnt += mesh->mNumVertices;
            rt_prim_cnt += mesh->mNumFaces;

            vertex_count += output.meshlet_vertex_data.size() / (stride / sizeof(float));
            index_count += output.primitive_indices.size();
            meshlet_count += output.meshlets.size();
        }

        // Moer::Array<float>*    m_vertex_data = new Moer::Array<float>{};
        // Moer::Array<uint32_t>* m_index_data  = new Moer::Array<uint32_t>;

        // m_vertex_data->resize(vertex_count * stride);
        // m_index_data->resize(index_count);

        // uint32_t vertex_offset = 0, index_offset = 0;
        // //uint32_t cur_vertex_count=0,cur_index_count=0;
        // for (uint32_t i = 0; i < gltf_scene->mNumMeshes; i++) {
        //     const auto* mesh             = gltf_scene->mMeshes[i];
        //     auto        cur_vertex_count = GetVertexData(mesh, m_vertex_data->data() + vertex_offset * stride);
        //     auto        cur_index_count  = GetIndexData(mesh, m_index_data->data() + index_offset);
        //     m_mesh_infos[i]              = {.vertex_offset = vertex_offset, .index_offset = index_offset, .vertex_count = cur_vertex_count, .index_count = cur_index_count};
        //     vertex_offset += cur_vertex_count;
        //     index_offset += cur_index_count;
        // }

        //Todo Load Lights
        LoadCameras(gltf_scene);
        LoadNode(gltf_scene->mRootNode, gltf_scene);
        LoadLights(gltf_scene);

        auto                      instance_id = 0;
        Moer::Array<InstanceData> instance_data;
        Moer::Array<RTInstance>   rt_instances;
        rt_instances.reserve(m_scene_data->m_prim_infos.size());

        Moer::Array<InstanceMeshInfo> instance_mesh_info;
        Moer::Array<uint32_t>         instance_ids;
        // instance_data.reserve(entities.size());
        for (int i = 0; i < m_scene_data->m_prim_infos.size(); i++) {
            auto              transform    = m_scene_data->m_prim_infos[i].transform;
            auto              mesh_info    = m_mesh_infos[m_scene_data->m_prim_infos[i].mesh_id];
            const RTMeshInfo& rt_mesh_info = rt_mesh_infos[m_scene_data->m_prim_infos[i].mesh_id];

            auto model_2_world = transform.GetMatrix4x4();
            //todo material data not correct
            auto scale = transform.AffineDecomposition().scaling;
            instance_data.emplace_back(model_2_world,
                                       Inverse(model_2_world),
                                       std::max(scale.x, std::max(scale.y, scale.z)),
                                       0,
                                       m_scene_data->m_material_instance_indexes[m_scene_data->m_prim_infos[i].material_id],
                                       0);
            Vector4f corner[8];
            corner[0]    = model_2_world * Vector4f(mesh_info.center + mesh_info.extent, 1.0f);
            corner[1]    = model_2_world * Vector4f(mesh_info.center - Vector3f(mesh_info.extent.x, mesh_info.extent.y, -mesh_info.extent.z), 1.0f);
            corner[2]    = model_2_world * Vector4f(mesh_info.center - Vector3f(mesh_info.extent.x, -mesh_info.extent.y, mesh_info.extent.z), 1.0f);
            corner[3]    = model_2_world * Vector4f(mesh_info.center - Vector3f(mesh_info.extent.x, -mesh_info.extent.y, -mesh_info.extent.z), 1.0f);
            corner[4]    = model_2_world * Vector4f(mesh_info.center - Vector3f(-mesh_info.extent.x, mesh_info.extent.y, mesh_info.extent.z), 1.0f);
            corner[5]    = model_2_world * Vector4f(mesh_info.center - Vector3f(-mesh_info.extent.x, mesh_info.extent.y, -mesh_info.extent.z), 1.0f);
            corner[6]    = model_2_world * Vector4f(mesh_info.center - Vector3f(-mesh_info.extent.x, -mesh_info.extent.y, mesh_info.extent.z), 1.0f);
            corner[7]    = model_2_world * Vector4f(mesh_info.center - mesh_info.extent, 1.0f);
            auto new_min = corner[0];
            auto new_max = corner[0];
            for (int i = 1; i < 8; i++) {
                new_min = Min(new_min, corner[i]);
                new_max = Max(new_max, corner[i]);
            }

            RTInstance rt_instance;
            rt_instance.overload_m1 = model_2_world.r0;
            rt_instance.overload_m2 = model_2_world.r1;
            rt_instance.overload_m3 = model_2_world.r2;

            rt_instance.SetMaterial(0, m_scene_data->m_material_instance_indexes[m_scene_data->m_prim_infos[i].material_id]);
            rt_instance.flags       = Render::RTVM_DEFAULT;
            rt_instance.prim_offset = rt_mesh_info.primitive_offset;
            rt_instance.vtx_offset  = rt_mesh_info.vertex_offset;

            rt_instances.push_back(rt_instance);

            instance_mesh_info.emplace_back(
                Vector3f(new_min + new_max) * 0.5f,
                mesh_info.vertex_offset,
                Vector3f(new_max - new_min) * 0.5f,
                mesh_info.vertex_count,
                mesh_info.index_offset,
                mesh_info.index_count,
                mesh_info.meshlet_offset,
                mesh_info.meshlet_count);

            instance_ids.push_back(instance_id);
            instance_id++;
        }

        if (IsCurrentlyGameThread()) {
            RenderThreadFence fence;
            fence.Wait();
        }
        // if (!IsCurrentlyRenderThread()) {
        //     ScopeEventRef event;
        //     EnqueueRenderTask([&event]() {
        //         event.Trigger();
        //     });
        // }
        // TaskGraph::QueueTask()

        m_scene_data->m_vertex_data        = std::move(m_vertex_data);
        m_scene_data->m_index_data         = std::move(m_index_data);
        m_scene_data->m_meshlet_descs      = std::move(m_meshlet_descs);
        m_scene_data->m_meshlet_bounds     = std::move(m_meshlet_bounds);
        m_scene_data->m_mesh_infos         = std::move(m_mesh_infos);
        m_scene_data->m_instance_data      = std::move(instance_data);
        m_scene_data->m_instance_mesh_info = std::move(instance_mesh_info);
        m_scene_data->m_instance_id        = std::move(instance_ids);

        m_scene_data->rt_instances  = std::move(rt_instances);
        m_scene_data->rt_vertices   = std::move(rt_vertices);
        m_scene_data->rt_prims      = std::move(rt_prims);
        m_scene_data->rt_indices    = std::move(rt_indices);
        m_scene_data->rt_mesh_infos = std::move(rt_mesh_infos);

        m_scene_data->m_materials          = std::move(m_materials);
        m_scene_data->m_material_instances = std::move(m_material_instances);
        m_scene_data->m_textures           = std::move(m_textures);
        m_scene_data->m_cameras            = std::move(m_scene_data->m_cameras);
        m_scene_data->m_path               = _file_path.string();
        m_scene_data->m_vertex_stride      = stride;
        m_scene_data->m_index_stride       = sizeof(uint32_t);

        auto scene_data = std::move(m_scene_data);

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

    void Parser::Impl::LoadNode(const aiNode* node, const aiScene* scene) {
        if (node->mMeshes) {
            for (uint32_t i = 0; i < node->mNumMeshes; i++) {
                const auto  mesh_idx = node->mMeshes[i];
                auto* const ai_mesh  = scene->mMeshes[mesh_idx];
                const auto& aabb     = ai_mesh->mAABB;
                auto        entity   = EntityManager::Get().Create();

                auto transform = GetTransform(node);

                uint32_t stride = 0;

                uint32_t          material_id = ai_mesh->mMaterialIndex;
                aiMaterial const* material    = scene->mMaterials[material_id];

                aiString    name;
                std::string material_name;

                if (material->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
                    material_name = name.C_Str();
                } else {
                    material_name = std::string("_default_material_Name") + std::to_string(material_id);
                }

                m_scene_data->m_material_instance_indexes[material_name] = material_id;

                if (!m_material_instances.contains(material_name)) {
                    LoadMaterial(scene, material, material_name);
                }

                m_scene_data->m_prim_infos.push_back({.mesh_id = mesh_idx, .material_id = material_name, .transform = transform});

                //Todo Load Material
            }
        }
        for (uint32_t i = 0; i < node->mNumChildren; i++) {
            LoadNode(node->mChildren[i], scene);
        }
    }

    UniquePtr<SceneData> Parser::LoadSceneFromFile(const std::filesystem::path& file_path) noexcept {

        //  auto lights = scene->mLights;
        Impl impl;
        return std::move(UniquePtr<SceneData>(impl.LoadSceneFromFile(file_path.generic_string().data())));
    }

    void Parser::LoadSceneFromFileAsync(const std::filesystem::path& file_path) noexcept {
        Impl* impl = MoerNew(Impl)();
        //  impl->LoadSceneFromFileAsync(file_path);
    }

}// namespace Moer::Resource::Gltf