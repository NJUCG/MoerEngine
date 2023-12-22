#include "loader/gltf/Parser.h"

#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "rhi/RHI.h"
#include "scene/EntityManager.h"
#include "resources/GpuScene.h"
#include "scene/RenderableManager.h"
#include "scene/TransformManager.h"

RHI* g_rhi = g_rhi;


namespace  Moer::Resource::Gltf  {
    
    struct Parser::Impl {
        std::unique_ptr<Scene> LoadSceneFromFile(const std::filesystem::path &  file_path);
        void loadNode(const aiNode * node,const aiScene * scene);
        void loadTextures(const aiScene *scene);
        std::vector<CountableRef<RHITexture>> m_textures{};
        std::vector<CountableRef<RHIRenderPrimitive>> m_primitives{};
        std::unique_ptr<Scene> m_scene{nullptr};
    };

    void Parser::Impl::loadTextures(const aiScene* scene) {
        const uint32_t texture_num = scene->mNumTextures;
        if(texture_num == 0) {
            LOG_INFO("Current Scene has no texture");
            return; 
        }
        for(uint32_t i = 0; i<texture_num ;i++) {
            const auto * texture = scene->mTextures[i];
            
        }
        
    }

    std::unique_ptr<Scene> Parser::Impl::LoadSceneFromFile(const std::filesystem::path& file_path) {
        GpuPrimitiveBuilder::InitBuild();
        m_scene = std::make_unique<Scene>();
        Assimp::Importer importer;
        auto gltf_scene = importer.ReadFile(file_path.string(), aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_CalcTangentSpace);
        if (!gltf_scene) {
            LOG_WARNING("Failed to load gltf file: {} ", file_path.string());
            return nullptr;
        }
        // loadNode()
        
        auto moer_scene = std::make_unique<Scene>();
        //Todo Load Textures
        //Todo Load Materials
        //Todo Load Cameras
        //Todo Load Lights
        loadNode(gltf_scene->mRootNode,gltf_scene);
        //todo after build all    end build 
       // GpuPrimitiveBuilder::EndBuild();
        return std::move(m_scene);
    }

    std::vector<float> GetVertexData(const aiMesh * mesh) {
        std::vector<float> data;
        bool has_position = mesh->HasPositions();
        bool has_normal = mesh->HasNormals();
        bool has_tangent = mesh->HasTangentsAndBitangents();
        bool has_uv = mesh->HasTextureCoords(0);
        size_t stride = 0;
        size_t  attr_offset[4];
        if(has_position) {
            attr_offset[0] = stride;
            stride += 3;
        }
        if(has_normal) {
            attr_offset[1] = stride;
            stride += 3;
        }
        if(has_tangent) {
            attr_offset[2] = stride;
            stride += 6;
        }
        if(has_uv) {
            attr_offset[3] = stride;
            stride += 2;
        }

        uint32_t vertex_num = mesh->mNumVertices;
        data.resize(vertex_num * stride);
        for(uint32_t i = 0 ;i< vertex_num ;i++) {
            if(has_position) {
                const auto copySrc = reinterpret_cast<float *>(mesh->mVertices+i);
                std::copy( copySrc,copySrc+3,data.data() + attr_offset[0] + i * stride);
            }
            if(has_normal) {
                const auto copySrc = reinterpret_cast<float *>(mesh->mNormals+i);
                std::copy( copySrc,copySrc+3,data.data() + attr_offset[1] + i * stride);
            }
            if(has_tangent) {
                auto copySrc = reinterpret_cast<float *>(mesh->mTangents+i);
                std::copy( copySrc,copySrc+3,data.data() + attr_offset[2] + i * stride);
                copySrc = reinterpret_cast<float *>(mesh->mBitangents+i);
                std::copy( copySrc,copySrc+3,data.data() + attr_offset[2] + i * stride + 3);
            }
            if(has_uv) {
                const auto copySrc = reinterpret_cast<float *>(mesh->mTextureCoords[0]+i);
                std::copy( copySrc,copySrc+2,data.data() + attr_offset[3] + i * stride);
            }
        }
        return data;
        
    }

    std::vector<uint32_t> GetIndexData(const aiMesh * mesh) {
        std::vector<uint32_t> data;
        if(mesh->HasFaces()) {
            for(uint32_t i = 0 ;i<mesh->mNumFaces;i++) {
                const auto & face = mesh->mFaces[i];
                data.insert(data.end(),face.mIndices,face.mIndices+face.mNumIndices);
            }
        }
        return data;
    }

    VertexAttributeFlags GetAttribute(const aiMesh * mesh) {
        VertexAttributeFlags attribute = 0;
        if(mesh->HasPositions()) {
            attribute |=  E_VERTEX_ATTRIBUTE::E_POSITION;
        }
        if(mesh->HasNormals()) {
            attribute |=  E_VERTEX_ATTRIBUTE::E_NORMAL;
        }
        if(mesh->HasTangentsAndBitangents()) {
            attribute |=  E_VERTEX_ATTRIBUTE::E_TANGENT;
            attribute |=  E_VERTEX_ATTRIBUTE::E_BITANGENT;
        }
        if(mesh->HasTextureCoords(0)) {
            attribute |=  E_VERTEX_ATTRIBUTE::E_UV0;
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

    Transform GetTransform(const aiNode * node) {
        Matrix4x4f matrix;
        for(uint32_t i = 0 ;i<4;i++) {
            for(uint32_t j = 0 ;j<4;j++) {
                matrix[i][j] = node->mTransformation[i][j];
            }
        }
        return {matrix};
    }

    void Parser::Impl::loadNode(const aiNode* node,const aiScene * scene) {
        if(node->mMeshes) {
            for(uint32_t  i = 0; i<node->mNumMeshes;i++) {
                const auto meshIdx = node->mMeshes[i];
                const auto mesh = scene->mMeshes[meshIdx];
                
                auto entity = EntityManager::Get().Create();
                RenderableManager::Builder().Geometry(EPrimitiveType::TRIANGLES,GetVertexData(mesh),GetIndexData(mesh),0,mesh->mNumFaces*3).Build(entity);
                TransformManager::Get().Set(entity,GetTransform(node));

                VertexAttributeFlags attribute = GetAttribute(mesh);
                EnqueueRenderTask([entity,attribute]() {
                    GpuPrimitiveBuilder gpuPrimitiveBuilder;
                    RHIRenderPrimitiveRef ref = gpuPrimitiveBuilder.Vertex(&RenderableManager::Get().GetVertexData(entity))
                                                                   .Index(&RenderableManager::Get().GetIndexData(entity)).Attribute(attribute).Build();
                    RenderableManager::Get().SetRHIRenderPrimitiveRef(entity,ref);
                });
                
                m_scene->AddEntity(entity);
                //Todo Load Material
            }
            
        }
        for(uint32_t i = 0 ;i< node->mNumChildren;i ++ ) {
            loadNode(node->mChildren[i],scene);
        }
    }


    
std::unique_ptr<Scene>  Parser::LoadSceneFromFile(const std::filesystem::path &  file_path) noexcept {
   
  //  auto lights = scene->mLights;
    Impl impl;
    return impl.LoadSceneFromFile(file_path);
}

}

    
