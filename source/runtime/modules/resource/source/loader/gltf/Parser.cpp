#include "loader/gltf/Parser.h"

#include "RenderThread.h"
#include "../io/ImageIO.h"
#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "math/Base.h"
#include "meshprocess/MeshProcessor.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "resources/GpuScene.h"
#include "rhi/RHICommon.h"
#include "scene/EntityManager.h"
#include "scene/CameraManager.h"
#include "scene/Material.h"
#include "scene/MaterialInstance.h"
#include "scene/RenderableManager.h"
#include "scene/Scene.h"
#include "scene/TransformManager.h"

#include <filesystem>
#include <stb/stb_image.h>
#include <meshoptimizer.h>

namespace Moer::Resource::Gltf {

    struct Parser::Impl {
        std::unique_ptr<Scene, MoerDeleter> LoadSceneFromFile(const std::filesystem::path& file_path);
        void                                LoadNode(const aiNode* node, const aiScene* scene);
        void                                loadCameras(const aiScene* scene);
        void                                loadMaterial(const aiScene* ai_scene, const aiMaterial* ai_material, const std::string& materialName);
        void                                LoadTexture(const aiScene* scene, const aiString& texture_path, MaterialInstanceRef& mat, const std::string& param_name);
        ~Impl() = default;

        Moer::Array<RHIRenderPrimitiveRef>                   m_primitives{};
        Moer::UnorderedMap<std::string, RHITextureRef>       m_textures{};
        Moer::UnorderedMap<std::string, MaterialInstanceRef> m_materials{};
        Moer::UnorderedMap<size_t, MaterialRef>              m_material_cache{};
        Moer::Array<MeshInfo>                                m_mesh_infos{};
        Moer::Array<InstanceData>                            m_instance_data{};
        Moer::Array<InstanceMeshInfo>                        m_instance_mesh_info{};

        Moer::Array<MeshletDesc>  m_meshlet_descs{};
        Moer::Array<MeshletBound> m_meshlet_bounds{};
        //temperaly store vertex and index data
        Moer::Array<float>    m_vertex_data{};
        Moer::Array<uint32_t> m_index_data{};

        std::filesystem::path m_file_parent_path{};
        UniquePtr<Scene>      m_scene{nullptr};
    };

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
            stride += 6;
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
                copy_src = reinterpret_cast<float*>(mesh->mBitangents + i);
                std::copy(copy_src, copy_src + 3, data + attr_offset[2] + i * stride + 3);
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
            attribute |= E_VERTEX_ATTRIBUTE::E_BITANGENT;
            stride += 6;
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
            mat->SetParameter(param_name, m_textures[texture_path.C_Str()]);
            return;
        }

        int32_t         embedded_id = GetEmbeddedTextureId(texture_path);
        TextureBuilder* builder     = MoerNew(TextureBuilder);
        int             width, height, channel;
        void*           data = nullptr;
        if (embedded_id >= 0) {
            const aiTexture* texture = scene->mTextures[embedded_id];
            stbi_load_from_memory(reinterpret_cast<const unsigned char*>(texture->pcData), texture->mWidth, &width, &height, &channel, 4);

            builder->CallBack(&free);
            //todo
        } else {
            std::filesystem::path texture_file_path = m_file_parent_path / texture_path.C_Str();
            auto                  image_desc        = ImageIO::ReadFromFile(texture_file_path);
            width                                   = image_desc.width;
            height                                  = image_desc.height;
            channel                                 = image_desc.channal;
            data                                    = image_desc.data;
            builder->CallBack(image_desc.data_callback);
        }
        builder->Width(width).Height(height).Format(EPixelFormat::PF_R8G8B8A8_UNORM).Data(data);
        EnqueueRenderTask([this, builder, mat, param_name, texture_path]() {
            RHITextureRef texture = builder->Build();
            MoerDelete(builder);
            mat->SetParameter(param_name, texture);
            m_textures[texture_path.C_Str()] = texture;
        });
    }
    static Vector3f ToVector3f(const aiVector3D& vec) {
        return {vec.x, vec.y, vec.z};
    }

    void Parser::Impl::loadCameras(const aiScene* scene) {
        const uint32_t camera_num = scene->mNumCameras;
        if (camera_num == 0) {
            LOG_WARNING("Current Scene has no camera");
            Entity    entity         = EntityManager::Get().Create();
            CameraRef default_camera = CameraManager::Get().Create(entity);
            default_camera->SetFov(36.f);
            Transform transform = Transform(Vector3f(0.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, 1.0f), Vector3f(0.0f, 1.0f, 0.0f));
            default_camera->SetWorldTransform(transform);
            default_camera->SetNearClip(0.1f);
            default_camera->SetFarClip(1000.0f);
            default_camera->SetAspectRatio(16.0f / 9.0f);
            m_scene->AddCamera(entity);
        } else
            for (uint32_t i = 0; i < camera_num; i++) {
                const auto* camera     = scene->mCameras[i];
                Entity      entity     = EntityManager::Get().Create();
                CameraRef   camera_ref = CameraManager::Get().Create(entity);
                Transform&  transform  = TransformManager::Get().Create(entity);
                transform              = Transform(ToVector3f(camera->mPosition), ToVector3f(camera->mLookAt), ToVector3f(camera->mUp));
                camera_ref->SetFov(Angle::RadianToDegree(camera->mHorizontalFOV));
                camera_ref->SetWorldTransform(transform);
                camera_ref->SetNearClip(camera->mClipPlaneNear);
                camera_ref->SetFarClip(camera->mClipPlaneFar);
                camera_ref->SetAspectRatio(camera->mAspect);
                m_scene->AddCamera(entity);
            }
    }

    MaterialRef GetDefaultMaterial() {
        MaterialBuilder materialBuilder{};
        MaterialRef     default_material = new Material();
        materialBuilder.parameter("defaultSampler", ESamplerType::SAMPLER_2D);
        materialBuilder.parameter("baseColorMap", ETextureDimension::TEX_2D);
        return materialBuilder.Build();
    }

    void Parser::Impl::loadMaterial(const aiScene* ai_scene, const aiMaterial* ai_material, const std::string& materialName) {

        size_t material_hash = 0;
        if (!m_material_cache.contains(material_hash)) {
            m_material_cache[material_hash] = GetDefaultMaterial();
        }
        const auto material = m_material_cache[material_hash];

        auto material_instance    = material->CreateInstance();
        m_materials[materialName] = material->CreateInstance();

        aiString base_color_path;
        if (ai_material->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &base_color_path) == AI_SUCCESS) {
            LoadTexture(ai_scene, base_color_path, material_instance, "baseColorMap");
        }
        material_instance->SetParameter("defaultSampler", SamplerParams{});

        m_materials[materialName] = material_instance;
    }

    std::unique_ptr<Scene, MoerDeleter> Parser::Impl::LoadSceneFromFile(const std::filesystem::path& file_path) {
        GpuPrimitiveBuilder::InitBuild();
        m_scene = std::move(UniquePtr<Scene>(MoerNew(Scene)()));
        Assimp::Importer importer;
        auto             real_path  = std::filesystem::canonical(file_path);
        const auto*      gltf_scene = importer.ReadFile(file_path.string(), aiProcess_Triangulate | aiProcess_GenBoundingBoxes | aiProcess_GenNormals | aiProcess_CalcTangentSpace);
        if (!gltf_scene) {
            LOG_WARNING("Failed to load gltf file: {} ", file_path.string());
            return nullptr;
        }
        // loadNode()

        // auto moer_scene = std::make_unique<Scene>();

        m_file_parent_path = file_path.parent_path();

        uint32_t stride;
        //Assume all mesh has same attribute
        auto attribute = GetAttribute(gltf_scene->mMeshes[0], stride);

        stride *= sizeof(float);
        m_mesh_infos.resize(gltf_scene->mNumMeshes);

        uint32_t vertex_count = 0, index_count = 0, meshlet_count = 0;
        for (uint32_t i = 0; i < gltf_scene->mNumMeshes; i++) {
            const auto* mesh = gltf_scene->mMeshes[i];

            auto aabb_min    = mesh->mAABB.mMin;
            auto aabb_max    = mesh->mAABB.mMax;
            auto aabb_center = (mesh->mAABB.mMin + mesh->mAABB.mMax) * 0.5f;
            auto aabb_extent = mesh->mAABB.mMax - aabb_center;

            uint32_t temp_stride;
            GetAttribute(gltf_scene->mMeshes[0], temp_stride);
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
        auto* gpu_scene = m_scene.get();
        EnqueueRenderTask([vertex_data    = std::move(m_vertex_data),
                           index_data     = std::move(m_index_data),
                           meshlet_bounds = std::move(m_meshlet_bounds),
                           meshlet_descs  = std::move(m_meshlet_descs),
                           gpu_scene] {
            GpuSceneBufferBuilder gpu_scene_buffer_builder;
            gpu_scene_buffer_builder.Vertex(&vertex_data);
            gpu_scene_buffer_builder.Index(&index_data);
            auto buffer_pair = gpu_scene_buffer_builder.Build();

            auto meshlet_bounds_buffer = gpu_scene_buffer_builder.CopyFrom(meshlet_bounds.data(), meshlet_bounds.size() * sizeof(MeshletBound));

            auto meshlet_descs_buffer = gpu_scene_buffer_builder.CopyFrom(meshlet_descs.data(), meshlet_descs.size() * sizeof(MeshletDesc));
            gpu_scene->SetBuffer("vertex_buffer", buffer_pair.first);
            gpu_scene->SetBuffer("index_buffer", buffer_pair.second);
            gpu_scene->SetBuffer("meshlet_bounds", meshlet_bounds_buffer);
            gpu_scene->SetBuffer("meshlet_descs", meshlet_descs_buffer);
        });

        //Todo Load Lights
        loadCameras(gltf_scene);
        LoadNode(gltf_scene->mRootNode, gltf_scene);

        auto                          instance_id = 0;
        Moer::Array<InstanceData>     instance_data;
        Moer::Array<InstanceMeshInfo> instance_mesh_info;
        Moer::Array<uint32_t>         instance_ids;
        // instance_data.reserve(entities.size());
        m_scene->ForEach([this, &instance_data, &instance_mesh_info, &instance_ids, &instance_id](Entity entity) {
            auto material_id   = RenderableManager::Get().GetMaterialInstance(entity);
            auto transform     = TransformManager::Get().Get(entity);
            auto mesh_info     = RenderableManager::Get().GetMeshInfo(entity);
            auto model_2_world = transform.GetMatrix4x4();
            //todo material data not correct
            auto scale = transform.AffineDecomposition().scaling;
            instance_data.emplace_back(Transpose(model_2_world),
                                       Transpose(Inverse(model_2_world)),
                                       std::min(scale.x, std::min(scale.y, scale.z)),
                                       0,
                                       instance_id,
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
        });

        EnqueueRenderTask([instance_data = std::move(instance_data), instance_mesh_info = std::move(instance_mesh_info), instance_id(std::move(instance_ids)), gpu_scene]() {
            GpuSceneBufferBuilder gpu_scene_buffer_builder;

            auto instance_buffer    = gpu_scene_buffer_builder.CopyFrom(instance_data.data(), instance_data.size() * sizeof(InstanceData));
            auto instance_id_buffer = gpu_scene_buffer_builder.CreateBufferWithData(
                EBufferUsageFlags::VERTEX_BUFFER, instance_id.data(), instance_id.size() * sizeof(int));
            auto instance_mesh_buffer = gpu_scene_buffer_builder.CopyFrom(instance_mesh_info.data(), instance_mesh_info.size() * sizeof(InstanceMeshInfo));
            gpu_scene->SetBuffer("instance_data", instance_buffer);
            gpu_scene->SetBuffer("instance_id_buffer", instance_id_buffer);
            gpu_scene->SetBuffer("instance_meshlet_info_buffer", instance_mesh_buffer);
        });
        RenderThreadFence fence;
        fence.Wait();
        //todo after build all    end build
        // GpuPrimitiveBuilder::EndBuild();
        return std::move(m_scene);
    }

    //Position
    // CountableRef<RHIBuffer> GetVertexBuffer(const aiMesh * mesh) {
    //     uint32_t size;
    //
    //     const uint32_t one_attr_size = mesh->mNumVertices * sizeof(float) * 3;
    //     if(mesh->HasPositions())
    //         size += one_attr_size;
    //     if(mesh->HasNormals())
    //         size += one_attr_size;
    //     if(mesh->HasTangentsAndBitangents())
    //         size += 2 * one_attr_size;
    //
    //
    //   //  builder.Attribute().
    //
    //     RHIBufferCreateInfo  info(size,sizeof(float),EBufferUsageFlags::VERTEX_BUFFER);
    //     RHIBufferRef buffer = g_rhi->RHICreateBuffer(info);
    //
    //     auto  mapped_ptr = static_cast<float * >(g_rhi->RHIMapBuffer(buffer,0,size));
    //     uint32_t offset = 0;
    //
    //
    //     if(mesh->HasPositions()) {
    //         memcpy(mapped_ptr+offset,mesh->mVertices,one_attr_size);
    //         offset += one_attr_size;
    //     }
    //     if(mesh->HasNormals()) {
    //         memcpy(mapped_ptr+offset,mesh->mNormals,one_attr_size);
    //         offset+= one_attr_size;
    //     }
    //     if(mesh->HasTangentsAndBitangents()) {
    //         memcpy(mapped_ptr+offset,mesh->mTangents,one_attr_size);
    //         offset+= one_attr_size;
    //         memcpy(mapped_ptr+offset,mesh->mTangents,one_attr_size);
    //     }
    //     g_rhi->RHIUnmapBuffer(buffer);
    //
    //     return buffer;
    // }
    // CountableRef<RHIBuffer> GetIndexBuffer(const aiMesh * mesh) {
    //     if(!mesh->HasFaces())
    //         return nullptr;
    //     //currently only support triangles
    //     uint32_t size = mesh->mNumFaces * sizeof(uint32_t) * 3;
    //     RHIBufferCreateInfo  info(size,sizeof(uint32_t),EBufferUsageFlags::INDEX_BUFFER);
    //     RHIBufferRef buffer = g_rhi->RHICreateBuffer(info);
    //     auto mapped_ptr = static_cast<uint32_t *>(g_rhi->RHIMapBuffer(buffer,0,size));
    //     for(uint32_t i = 0 ;i<mesh->mNumFaces;i++) {
    //         const auto face = mesh->mFaces[i];
    //         memcpy(mapped_ptr+i*3,face.mIndices,sizeof(uint32_t)*3);
    //     }
    //     g_rhi->RHIUnmapBuffer(buffer);
    //     return buffer;
    // }

    Transform GetTransform(const aiNode* node) {
        Matrix4x4f matrix;
        for (uint32_t i = 0; i < 4; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                matrix[i][j] = node->mTransformation[i][j];
            }
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

                RenderableManager::Builder().Geometry(EPrimitiveType::TRIANGLES,
                                                      m_mesh_infos[mesh_idx].vertex_count,
                                                      m_mesh_infos[mesh_idx].index_count,
                                                      m_mesh_infos[mesh_idx].vertex_offset,
                                                      m_mesh_infos[mesh_idx].index_offset,
                                                      m_mesh_infos[mesh_idx].meshlet_offset,
                                                      m_mesh_infos[mesh_idx].meshlet_count)
                    .Build(entity);
                RenderableManager::Get().SetMeshInfo(entity, m_mesh_infos[mesh_idx]);
                TransformManager::Get().Set(entity, GetTransform(node));

                uint32_t stride = 0;

                m_scene->AddEntity(entity);

                uint32_t          material_id = ai_mesh->mMaterialIndex;
                aiMaterial const* material    = scene->mMaterials[material_id];

                aiString    name;
                std::string material_name;

                if (material->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
                    material_name = name.C_Str();
                }

                if (!m_materials.contains(material_name)) {
                    loadMaterial(scene, material, material_name);
                }
                auto material_instance = m_materials[material_name];
                RenderableManager::Get().SetMaterialInstance(entity, material_instance);
                //Todo Load Material
            }
        }
        for (uint32_t i = 0; i < node->mNumChildren; i++) {
            LoadNode(node->mChildren[i], scene);
        }
    }

    UniquePtr<Scene> Parser::LoadSceneFromFile(const std::filesystem::path& file_path) noexcept {

        //  auto lights = scene->mLights;
        Impl impl;
        return impl.LoadSceneFromFile(file_path);
    }

}// namespace Moer::Resource::Gltf