#include "loader/gltf/Parser.h"

#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "rhi/RHI.h"
#include "resources/GpuScene.h"
#include "scene/EntityManager.h"
#include "scene/CameraManager.h"
#include "scene/Material.h"
#include "scene/MaterialInstance.h"
#include "scene/RenderableManager.h"
#include "scene/TransformManager.h"

#include <stb/stb_image.h>

namespace Moer::Resource::Gltf {

    struct Parser::Impl {
        std::unique_ptr<Scene>                               LoadSceneFromFile(const std::filesystem::path& file_path);
        void                                                 LoadNode(const aiNode* node, const aiScene* scene);
        void                                                 loadCameras(const aiScene* scene);
        void                                                 loadMaterial(const aiScene* ai_scene, const aiMaterial* ai_material, const std::string& materialName);
        void                                                 LoadTexture(const aiScene* scene, const aiString& texture_path, MaterialInstanceRef& mat, const std::string& param_name);
        Moer::Array<RHIRenderPrimitiveRef>                   m_primitives{};
        Moer::UnorderedMap<std::string, RHITextureRef>       m_textures{};
        Moer::UnorderedMap<std::string, MaterialInstanceRef> m_materials{};
        Moer::UnorderedMap<size_t, MaterialRef>              m_material_cache{};
        std::filesystem::path                                m_file_parent_path{};
        std::unique_ptr<Scene>                               m_scene{nullptr};
    };

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
            data                                    = stbi_load(texture_file_path.string().c_str(), &width, &height, &channel, 4);
            builder->CallBack(stbi_image_free);
            //todo
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
            default_camera->SetFov(60.0f);
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

    std::unique_ptr<Scene> Parser::Impl::LoadSceneFromFile(const std::filesystem::path& file_path) {
        GpuPrimitiveBuilder::InitBuild();
        m_scene = std::make_unique<Scene>();
        Assimp::Importer importer;
        const auto*      gltf_scene = importer.ReadFile(file_path.string(), aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_CalcTangentSpace);
        if (!gltf_scene) {
            LOG_WARNING("Failed to load gltf file: {} ", file_path.string());
            return nullptr;
        }
        // loadNode()

        auto moer_scene = std::make_unique<Scene>();

        m_file_parent_path = file_path.parent_path();
        //Todo Load Lights
        loadCameras(gltf_scene);
        LoadNode(gltf_scene->mRootNode, gltf_scene);
        //todo after build all    end build
        // GpuPrimitiveBuilder::EndBuild();
        return std::move(m_scene);
    }

    Moer::Array<float> GetVertexData(const aiMesh* mesh) {
        Moer::Array<float> data;
        bool               has_position = mesh->HasPositions();
        bool               has_normal   = mesh->HasNormals();
        bool               has_tangent  = mesh->HasTangentsAndBitangents();
        bool               has_uv       = mesh->HasTextureCoords(0);
        size_t             stride       = 0;
        size_t             attr_offset[4];
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
        data.resize(vertex_num * stride);
        for (uint32_t i = 0; i < vertex_num; i++) {
            if (has_position) {
                auto* const copy_src = reinterpret_cast<float*>(mesh->mVertices + i);
                std::copy(copy_src, copy_src + 3, data.data() + attr_offset[0] + i * stride);
            }
            if (has_normal) {
                auto* const copy_src = reinterpret_cast<float*>(mesh->mNormals + i);
                std::copy(copy_src, copy_src + 3, data.data() + attr_offset[1] + i * stride);
            }
            if (has_tangent) {
                auto* copy_src = reinterpret_cast<float*>(mesh->mTangents + i);
                std::copy(copy_src, copy_src + 3, data.data() + attr_offset[2] + i * stride);
                copy_src = reinterpret_cast<float*>(mesh->mBitangents + i);
                std::copy(copy_src, copy_src + 3, data.data() + attr_offset[2] + i * stride + 3);
            }
            if (has_uv) {
                auto* const copy_src = reinterpret_cast<float*>(mesh->mTextureCoords[0] + i);
                std::copy(copy_src, copy_src + 2, data.data() + attr_offset[3] + i * stride);
            }
        }
        return data;
    }

    Moer::Array<uint32_t> GetIndexData(const aiMesh* mesh) {
        Moer::Array<uint32_t> data;
        if (mesh->HasFaces()) {
            for (uint32_t i = 0; i < mesh->mNumFaces; i++) {
                const auto& face = mesh->mFaces[i];
                data.insert(data.end(), face.mIndices, face.mIndices + face.mNumIndices);
            }
        }
        return data;
    }

    VertexAttributeFlags GetAttribute(const aiMesh* mesh) {
        VertexAttributeFlags attribute = 0;
        if (mesh->HasPositions()) {
            attribute |= E_VERTEX_ATTRIBUTE::E_POSITION;
        }
        if (mesh->HasNormals()) {
            attribute |= E_VERTEX_ATTRIBUTE::E_NORMAL;
        }
        if (mesh->HasTangentsAndBitangents()) {
            attribute |= E_VERTEX_ATTRIBUTE::E_TANGENT;
            attribute |= E_VERTEX_ATTRIBUTE::E_BITANGENT;
        }
        if (mesh->HasTextureCoords(0)) {
            attribute |= E_VERTEX_ATTRIBUTE::E_UV0;
        }
        return attribute;
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

                auto entity = EntityManager::Get().Create();
                RenderableManager::Builder().Geometry(EPrimitiveType::TRIANGLES, GetVertexData(ai_mesh), GetIndexData(ai_mesh), 0, ai_mesh->mNumFaces * 3).Build(entity);
                TransformManager::Get().Set(entity, GetTransform(node));

                VertexAttributeFlags attribute = GetAttribute(ai_mesh);
                EnqueueRenderTask([entity, attribute]() {
                    GpuPrimitiveBuilder   gpu_primitive_builder;
                    RHIRenderPrimitiveRef ref = gpu_primitive_builder.Vertex(&RenderableManager::Get().GetVertexData(entity))
                                                    .Index(&RenderableManager::Get().GetIndexData(entity))
                                                    .Attribute(attribute)
                                                    .Build();
                    RenderableManager::Get().SetRHIRenderPrimitiveRef(entity, ref);
                });

                m_scene->AddEntity(entity);

                uint32_t          materialId = ai_mesh->mMaterialIndex;
                aiMaterial const* material   = scene->mMaterials[materialId];

                aiString    name;
                std::string materialName;

                if (material->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
                    materialName = name.C_Str();
                }

                if (!m_materials.contains(materialName)) {
                    loadMaterial(scene, material, materialName);
                }
                auto material_instance = m_materials[materialName];
                RenderableManager::Get().SetMaterialInstance(entity, material_instance);
                //Todo Load Material
            }
        }
        for (uint32_t i = 0; i < node->mNumChildren; i++) {
            LoadNode(node->mChildren[i], scene);
        }
    }

    std::unique_ptr<Scene> Parser::LoadSceneFromFile(const std::filesystem::path& file_path) noexcept {

        //  auto lights = scene->mLights;
        Impl impl;
        return impl.LoadSceneFromFile(file_path);
    }

}// namespace Moer::Resource::Gltf