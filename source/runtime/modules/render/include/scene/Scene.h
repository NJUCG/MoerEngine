#pragma once
#include <future>
#include <unordered_set>
#include <vector>

#include "API_Macro.h"
#include "Entity.h"
#include "math/Base.h"
#include "math/Matrix.h"
#include "misc/CountableRef.h"
#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include "scene/TransformManager.h"
#include "serialize/Serializer.h"
#include "shaderheaders/shared/Geometry.h"
#include "resources/vertexfactory/VertexAttributes.h"
#include "resources/vertexfactory/VertexFactoryBuffers.h"

namespace Moer {

    // enum EVertexAttributes {
    //     Position = 0,
    //     Normal,
    //     Tangent,
    //     Texcoord0,
    //     Texcoord1,
    //     Color,
    //     JointIndices,
    //     JointWeights,
    //     VETA_Num
    // };

    enum class EGpuSceneResource {
        MeshInfo,
        InstanceInfo,
        MaterialInfo,
        LightInfo,
        CameraInfo,
        GaussianSplattingVertex,
        RTInstance,
        GeometryInfo,
        GeometryInstance,
        Num
    };
    // [deprecated] struct InstanceData {
    //     Matrix4x4f model2world;
    //     Matrix4x4f inv_model2world;
    //     float      scale;
    //     uint       padding;
    //     uint       material_id;
    //     uint       material_type;
    // };

    struct RTInstance {
        static constexpr uint material_type_mask = 0xff;
        static constexpr uint material_id_offset = 8;
        float4                overload_m1;
        float4                overload_m2;
        float4                overload_m3;
        uint                  material_type_and_id;
        uint                  flags;
        uint                  prim_offset;
        uint                  vtx_offset;

        uint GetMaterialType() const { return material_type_and_id & material_type_mask; }
        uint GetMaterialID() const { return material_type_and_id >> material_id_offset; }
        void SetMaterialType(uint _type) { material_type_and_id = (_type & material_type_mask) | (material_type_and_id & ~material_type_mask); }
        void SetMaterialID(uint _id) { material_type_and_id = (_id << material_id_offset) | (material_type_and_id & material_type_mask); }
        void SetMaterial(uint _type, uint _id) {
            material_type_and_id = (_type & material_type_mask) | ((_id << material_id_offset) & ~material_type_mask);
        }
    };
    struct Range {
        uint64 offset;
        uint64 size;
    };

    struct Box3D {
        float3 min;
        float3 max;

        float3 GetCenter() const noexcept { return (min + max) * 0.5f; }
        float3 GetExtent() const noexcept { return max - min; }

        void Expand(const float3& _point) noexcept {
            min = Min(min, _point);
            max = Max(max, _point);
        }
        void Expand(const Box3D& _box) noexcept {
            min = Min(min, _box.min);
            max = Max(max, _box.max);
        }
    };

    struct SceneTexture {
        Render::TextureRef texture;
        uint               bindless_handle;
    };

    //////////////////////////////////////////////////////////////////////////
    //cpu data
    //////////////////////////////////////////////////////////////////////////
    struct MeshBuffers {
        Render::BufferRef vertex_buffer;
        Render::BufferRef index_buffer;
        Render::BufferRef instance_buffer;

        int vtx_bdls_handle;
        int idx_bdls_handle;
        int inst_bdls_handle;

        StaticArray<Range, VA_NUM> vertex_ranges;

        //cpu datas
        Array<uint> indices;

        VertexFactoryBuffers vertex_factory_buffers;

        bool   HasAttribute(EVertexAttributes _attr) const noexcept { return vertex_ranges[static_cast<size_t>(_attr)].size > 0; }
        Range  GetAttributeRange(EVertexAttributes _attr) const noexcept { return vertex_ranges[static_cast<size_t>(_attr)]; }
        Range& GetAttributeRange(EVertexAttributes _attr) noexcept { return vertex_ranges[static_cast<size_t>(_attr)]; }

        void FillRanges() {
            size_t offset = 0;
            for (size_t i = 0; i < vertex_factory_buffers.GetAttributesCount(); i++) {
                EVertexAttributes attr = vertex_factory_buffers.GetAttribute(i);

                vertex_ranges[static_cast<size_t>(attr)] = {offset, vertex_factory_buffers.GetBufferLength(attr) * vertex_factory_buffers.GetSizeOfAttribute(attr)};

                offset += vertex_ranges[static_cast<size_t>(attr)].size;
            }
            // vertex_ranges[EVertexAttributes::Position]     = {0, positions.size() * sizeof(float3)};
            // vertex_ranges[EVertexAttributes::Normal]       = {vertex_ranges[EVertexAttributes::Position].size, normals.size() * sizeof(uint)};
            // vertex_ranges[EVertexAttributes::Tangent]      = {vertex_ranges[EVertexAttributes::Normal].offset + vertex_ranges[EVertexAttributes::Normal].size, tangents.size() * sizeof(uint)};
            // vertex_ranges[EVertexAttributes::Texcoord0]    = {vertex_ranges[EVertexAttributes::Tangent].offset + vertex_ranges[EVertexAttributes::Tangent].size, texcoords0.size() * sizeof(float2)};
            // vertex_ranges[EVertexAttributes::Texcoord1]    = {vertex_ranges[EVertexAttributes::Texcoord0].offset + vertex_ranges[EVertexAttributes::Texcoord0].size, texcoords1.size() * sizeof(float2)};
            // vertex_ranges[EVertexAttributes::JointIndices] = {vertex_ranges[EVertexAttributes::Texcoord1].offset + vertex_ranges[EVertexAttributes::Texcoord1].size, joint_data.size() * sizeof(uint16)};
            // vertex_ranges[EVertexAttributes::JointWeights] = {vertex_ranges[EVertexAttributes::JointIndices].offset + vertex_ranges[EVertexAttributes::JointIndices].size, joint_weights.size() * sizeof(float4)};
        }

        InputStream& operator>>(InputStream& _stream) {
            // _stream >> vertex_ranges >> indices >> positions >> normals >> tangents >> texcoords0 >> texcoords1 >> joint_data >> joint_weights;
            _stream >> vertex_ranges >> indices >> vertex_factory_buffers;
            return _stream;
        }

        OutputStream& operator<<(OutputStream& _stream) const {
            // _stream << vertex_ranges << indices << positions << normals << tangents << texcoords0 << texcoords1 << joint_data << joint_weights;
            _stream << vertex_ranges << indices << vertex_factory_buffers;
            return _stream;
        }
    };

    struct MeshGeometry {
        Box3D bounding_box;
        uint  material_id;
        uint  local_idx_offset;
        uint  local_idx_count;
        uint  local_vtx_offset;
        uint  local_vtx_count;
        uint  global_geom_idx;

        uint                   mesh_buffers_idx;// For serialization
        SharedPtr<MeshBuffers> mesh_buffers;

        InputStream& operator>>(InputStream& _stream) {
            _stream >> bounding_box >> material_id;
            _stream >> local_idx_offset >> local_idx_count >> local_vtx_offset >> local_vtx_count;
            _stream >> global_geom_idx >> mesh_buffers_idx;
            // 'buffers' will be filled in SceneCache::ReadSceneGeomInfo(..)
            return _stream;
        }

        OutputStream& operator<<(OutputStream& _stream) const {
            _stream << bounding_box << material_id;
            _stream << local_idx_offset << local_idx_count << local_vtx_offset << local_vtx_count;
            _stream << global_geom_idx << mesh_buffers_idx;
            // 'buffers' shouldn't be serialized
            return _stream;
        }
    };

    struct MeshInfo {
        std::string                    name;
        Array<SharedPtr<MeshGeometry>> geometries;
        uint                           geom_start_idx;//for serialization
        Box3D                          bounding_box;

        uint global_mesh_idx;

        InputStream& operator>>(InputStream& _stream) {
            _stream >> name;
            uint size;
            _stream >> size;
            geometries.resize(size);
            _stream >> geom_start_idx >> bounding_box >> global_mesh_idx;
            return _stream;
        }

        OutputStream& operator<<(OutputStream& _stream) const {
            _stream << name << uint(geometries.size()) << geom_start_idx << bounding_box << global_mesh_idx;
            return _stream;
        }
    };

    struct MeshInstance {
        int                 instance_id;
        int                 geom_instance_id;
        SharedPtr<MeshInfo> mesh_info;
        uint                mesh_info_idx;//for serialization

        InputStream& operator>>(InputStream& _stream) {
            _stream >> instance_id >> geom_instance_id >> mesh_info_idx;
            return _stream;
        }

        OutputStream& operator<<(OutputStream& _stream) const {
            _stream << instance_id << geom_instance_id << mesh_info_idx;
            return _stream;
        }
    };

    struct InstanceInfo {
        Transform transform;
        uint      mesh_instance_id;
    };

    struct GeometryInstance {
        uint first_geo_idx;
        uint geo_count;
        uint instance_id;
    };

    //////////////////////////////////////////////////////////////////////////
    //gpu data
    //////////////////////////////////////////////////////////////////////////

    struct RTMeshInfo {
        uint32_t vertex_offset;
        uint32_t vertex_count;
        uint32_t primitive_offset;
        uint32_t primitive_count;
    };

    struct RTVertex {
        float3 position;
        float  uv0;
        float3 normal;
        float  uv1;
        float3 tangent;
        float  padding;
    };

    struct RTPrimitvie {
        uint3 indices;
        float world_uv_units;
    };

    struct InstanceMeshInfo {
        Vector3f center;
        uint32_t vertex_offset;
        Vector3f extent;
        uint32_t vertex_count;
        uint32_t index_offset;
        uint32_t index_count;
        uint32_t meshlet_offset;
        uint32_t meshlet_count;
    };

    // using EntitySet = Moer::UnorderedSet<Entity, Entity::Hasher>;

    struct UniqueEntityArray {
        Array<Entity>            entities;
        UnorderedMap<uint, uint> entity_map;
        Array<uint>              free_indices;

        void AddEntity(Entity _entity) {
            if (free_indices.empty()) {
                entity_map[_entity.mIdentity] = entities.size();
                entities.push_back(_entity);
            } else {
                uint idx = free_indices.back();
                free_indices.pop_back();
                entity_map[_entity.mIdentity] = idx;
                entities[idx]                 = _entity;
            }
        }

        void RemoveEntity(Entity _entity) {
            auto it = entity_map.find(_entity.mIdentity);
            if (it != entity_map.end()) {
                free_indices.push_back(it->second);
                entity_map.erase(it);
            }
        }

        std::span<const Entity> GetEntities() const noexcept { return entities; }
    };

    struct AsyncSceneLoadInfo {

        bool         IsValid() const noexcept { return b_valid; };
        bool         IsReady() const noexcept { return b_valid && progress == 1.f; };
        float        GetProgress() const noexcept { return progress.load(); };
        class Scene* TryGetScene();
        COUNTABLE_IMPLEMENTATION_AUTO_DESTROY
        // private:
        Scene*           scene;
        std::atomic_uint progress = 0u;
        bool             b_valid  = false;
    };
    using AsyncSceneLoadInfoRef = CountableRef<AsyncSceneLoadInfo>;
#define DEFAULT_SCENE_NAME "Sponza"
    struct GpuScene;
    class RENDER_API Scene {
    public:
        Scene() noexcept;
        ~Scene() noexcept;
        void                    AddEntity(Entity _entity) noexcept;
        void                    AddCamera(Entity _entity) noexcept;
        void                    AddLight(Entity _entity) noexcept;
        void                    SetTlas(RHIRayTracingTLASRef _tlas) noexcept;
        void                    SetBlasList(Moer::Array<RHIRayTracingBLASRef> _blas_list) noexcept;
        void                    SetRaytracingScene(Render::RaytracingSceneRef _scene) noexcept;
        void                    RemoveEntity(Entity _entity) noexcept;
        void                    SetBuffer(const std::string& _name, RHIBufferRef _buffer) noexcept;
        void                    SetBuffer(EGpuSceneResource _type, Render::BufferRef _buffer) noexcept;
        Render::BufferRef       GetBuffer(EGpuSceneResource _type) const noexcept;
        RHIBufferRef            GetBuffer(const std::string& _name) const noexcept;
        std::span<const Entity> GetEntities() const noexcept;
        std::span<const Entity> GetLights() const noexcept;
        std::span<const Entity> GetCameras() const noexcept;
        Entity                  GetMainCamera() const noexcept;
        bool                    IsEntitiesEmpty() const noexcept;
        bool                    IsLightsEmpty() const noexcept;
        bool                    IsCamerasEmpty() const noexcept;
        void                    ForEach(std::function<void(Entity)> _func) const noexcept;
        bool                    IsReady() const noexcept;
        uint                    GetEntityCount() const noexcept;

        // [temperory]
        void RegisterMaterialTextures(UnorderedMap<std::string, SceneTexture> _textures) noexcept;

        static Scene* GetCurrentScene() noexcept;
        static void   SetCurrentScene(Scene* _scene) noexcept;

        static AsyncSceneLoadInfoRef GetCurrentSceneLoadInfo() noexcept;

        static bool RegisterAsyncLoadInfo(AsyncSceneLoadInfoRef _load_info);
        static void ResetAsyncLoadInfo() noexcept;

        GpuScene& GetGpuScene() noexcept;

        void SetVertexBuffer(Render::BufferRef _buffer) noexcept;
        void SetIndexBuffer(Render::BufferRef _buffer) noexcept;
        void SetInstanceBuffer(Render::BufferRef _buffer) noexcept;

        Render::BufferRef        GetVertexBuffer() const noexcept;
        Render::BufferRef        GetIndexBuffer() const noexcept;
        Render::BufferRef        GetInstanceBuffer() const noexcept;
        Render::BindlessArrayRef GetBindlessArray() const noexcept;

        void                                                                                UpdateGpuData();
        std::span<const Render::GeometryData>                                               GetGeometryDatas() const noexcept;
        std::span<const Render::InstanceData>                                               GetInstanceDatas() const noexcept;
        std::span<const UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>> GetVertexBufferViews();
        std::span<const UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>>         GetIndexBufferViews();
        std::span<const Render::GeometryInstance>                                           GetGeometryInstances() const noexcept;

    protected:
        class Impl;
        Impl* m_impl = nullptr;
    };

    struct GpuScene {
        Render::BufferRef GetGpuBuffer(EGpuSceneResource _resource) const { return global_resources.buffers[(uint32_t)_resource]; }
        // RHIRayTracingTLASRef GetTLAS() const { return tlas; }
        struct GResource {
            StaticArray<Render::BufferRef, (uint32_t)EGpuSceneResource::Num> buffers;
        } global_resources;
        Render::RaytracingSceneRef rt_scene{nullptr};
        Render::BufferRef          vertex_buffer{nullptr}, index_buffer{nullptr};
        Render::BindlessArrayRef   bindless_array{nullptr};

        UnorderedMap<std::string, SceneTexture> material_textures;
    };

    extern RENDER_API Scene* g_scene;

}// namespace Moer