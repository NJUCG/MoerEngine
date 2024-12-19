#include "scene/Scene.h"

#include "config/ConfigManager.h"
// #include "loader/gltf/Parser.h"
#include "log/LogSystem.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "scene/EntityManager.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "rhi/RHI.h"
#include "scene/TransformManager.h"
#include <atomic>

namespace Moer {
    // Scene * Scene::default_scene = nullptr;
    Scene* g_scene = nullptr;

    //////////////////////////////////////////////////////////////////////////
    // Scene::Impl
    //////////////////////////////////////////////////////////////////////////
    class RENDER_API Scene::Impl {
        friend class Scene;

    public:
        Impl() noexcept;
        ~Impl() noexcept;
        void         AddEntity(Entity _entity) noexcept { m_entities.emplace(_entity); }
        void         AddCamera(Entity _entity) noexcept { m_cameras.emplace(_entity); }
        void         AddLight(Entity _entity) noexcept { m_lights.emplace(_entity); }
        void         RemoveEntity(Entity _entity) noexcept { m_entities.erase(_entity); };
        void         SetBuffer(const std::string& _name, RHIBufferRef _buffer) { m_buffers[_name] = _buffer; }
        RHIBufferRef GetBuffer(const std::string& _name) const { return m_buffers.at(_name); }
        RHIUAVRef    GetUAV(const std::string& _name) const { return m_uavs.at(_name); }
        RHISRVRef    GetSRV(const std::string& _name) const { return m_srvs.at(_name); }
        void         ForEach(std::function<void(Entity)> _func) const noexcept {
            for (auto& entity : m_entities) {
                _func(entity);
            }
        }
        Array<Entity>                GetEntities() const noexcept;
        Array<Entity>                GetCameras() const noexcept;
        Array<Entity>                GetLights() const noexcept;
        bool                         IsEntitiesEmpty() const noexcept { return m_entities.empty(); }
        bool                         IsCamerasEmpty() const noexcept { return m_cameras.empty(); }
        bool                         IsLightsEmpty() const noexcept { return m_lights.empty(); }
        static AsyncSceneLoadInfoRef GetCurrentSceneLoadInfo() noexcept { return m_load_info; }

        GpuScene& GetGpuScene() noexcept { return gpu_scene; }

        void                                      UpdateGpuData();
        std::span<const Render::GeometryData>     GetGeometryDatas() const noexcept { return geometry_datas; }
        std::span<const Render::InstanceData>     GetInstanceDatas() const noexcept { return instance_datas; }
        std::span<const Render::GeometryInstance> GetGeometryInstances() { return geom_instances; }

    protected:
        Map<std::string, RHIBufferRef> m_buffers;
        Map<std::string, RHIUAVRef>    m_uavs;
        Map<std::string, RHISRVRef>    m_srvs;

        EntitySet m_entities;
        EntitySet m_cameras;
        EntitySet m_lights;

        static AsyncSceneLoadInfoRef m_load_info;
        GpuScene                     gpu_scene;

        Array<Render::GeometryData>                        geometry_datas;
        Array<Render::InstanceData>                        instance_datas;
        Array<StaticArray<Render::VertexBuffer, VETA_Num>> vtx_views;
        Array<Render::IndexBuffer>                         idx_views;
        Array<Render::GeometryInstance>                    geom_instances;
    };
    AsyncSceneLoadInfoRef Scene::Impl::m_load_info{nullptr};

    Scene::Impl::Impl() noexcept {
        gpu_scene.bindless_array = Render::RenderDevice::Get().CreateBindlessArray();
    }
    Array<Entity> Scene::Impl::GetEntities() const noexcept {
        Array<Entity> result;
        // result.reserve(m_entities.size());
        for (const Entity& entity : m_entities) {

            result.push_back(entity);
        }
        return result;
    }

    Array<Entity> Scene::Impl::GetCameras() const noexcept {
        Array<Entity> result;
        result.reserve(m_cameras.size());
        for (auto& entity : m_cameras) {
            result.push_back(entity);
        }
        return result;
    }

    Array<Entity> Scene::Impl::GetLights() const noexcept {
        Array<Entity> result;
        result.reserve(m_lights.size());
        for (auto& entity : m_lights) {
            result.push_back(entity);
        }
        return result;
    }

    void Scene::Impl::UpdateGpuData() {
        uint geometry_count = 0;
        uint instance_count = 0;
        for (auto& entity : m_entities) {
            const MeshInfo& info = *RenderableManager::Get().GetMeshInfo(entity);
            geometry_count += info.geometries.size();
            instance_count += 1;
        }

        geometry_datas.resize(geometry_count);
        geom_instances.resize(geometry_count);
        vtx_views.resize(instance_count);
        idx_views.resize(instance_count);
        instance_datas.resize(instance_count);

        for (auto& entity : m_entities) {
            const MeshInfo&                info          = *RenderableManager::Get().GetMeshInfo(entity);
            std::span<MaterialInstanceRef> mat_instances = RenderableManager::Get().GetMaterialInstances(entity);
            const MeshBuffers&             buffers       = *info.buffers;
            for (auto& geo : info.geometries) {
                uint  vtx_offset              = geo->local_vtx_offset + info.vtx_offset;
                uint  geo_idx                 = &geo - info.geometries.data();
                auto& geo_data                = geometry_datas[geo->global_geom_idx];
                geo_data.num_indices          = geo->local_idx_count;
                geo_data.num_vertices         = geo->local_vtx_count;
                geo_data.vertex_offset        = vtx_offset;
                geo_data.normal_offset        = buffers.GetAttributeRange(EVertexAttributes::Normal).offset + vtx_offset * sizeof(uint);
                geo_data.tangent_offset       = buffers.GetAttributeRange(EVertexAttributes::Tangent).offset + vtx_offset * sizeof(uint);
                geo_data.texcoord0_offset     = buffers.GetAttributeRange(EVertexAttributes::Texcoord0).offset + vtx_offset * sizeof(float2);
                geo_data.texcoord1_offset     = buffers.GetAttributeRange(EVertexAttributes::Texcoord1).offset + vtx_offset * sizeof(float2);
                geo_data.mat_idx_and_type     = geo->material_id << 8 | (uint)mat_instances[geo_idx]->GetMaterial()->GetType();
                geo_data.index_offset         = (geo->local_idx_offset + info.idx_offset) * sizeof(uint);
                geo_data.index_buffer_handle  = buffers.idx_bdls_handle;
                geo_data.vertex_buffer_handle = buffers.vtx_bdls_handle;

                auto& geom_instance        = geom_instances[geo->global_geom_idx];
                geom_instance.instance_idx = RenderableManager::Get().GetInstanceID(entity);
                geom_instance.geom_idx     = geo->global_geom_idx;
            }

            StaticArray<Render::VertexBuffer, VETA_Num>& vtx_view = vtx_views[info.global_mesh_idx];
            vtx_view[EVertexAttributes::Position]                 = {buffers.vertex_buffer.Get(), buffers.GetAttributeRange(EVertexAttributes::Position).offset};
            vtx_view[EVertexAttributes::Normal]                   = {buffers.vertex_buffer.Get(), buffers.GetAttributeRange(EVertexAttributes::Normal).offset};
            vtx_view[EVertexAttributes::Tangent]                  = {buffers.vertex_buffer.Get(), buffers.GetAttributeRange(EVertexAttributes::Tangent).offset};
            vtx_view[EVertexAttributes::Texcoord0]                = {buffers.vertex_buffer.Get(), buffers.GetAttributeRange(EVertexAttributes::Texcoord0).offset};
            vtx_view[EVertexAttributes::Texcoord1]                = {buffers.vertex_buffer.Get(), buffers.GetAttributeRange(EVertexAttributes::Texcoord1).offset};

            idx_views[info.global_mesh_idx] = {buffers.index_buffer->GetView(),
                                               EIndexElementType::IET_UINT32};

            uint  id                          = RenderableManager::Get().GetInstanceID(entity);
            auto& inst_data                   = instance_datas[id];
            inst_data.first_geom_idx          = info.geometries.front()->global_geom_idx;
            inst_data.geom_count              = info.geometries.size();
            inst_data.first_geom_instance_idx = info.global_mesh_idx;
            inst_data.model2world             = TransformManager::Get().Get(entity).GetMatrix3x4();
            // auto row_maj                      = TransformManager ::Get().Get(entity).GetMatrix3x4();
            // auto col_maj                      = TransformManager ::Get().Get(entity).GetMatrix3x4ColumnMajor();

            inst_data.prev_model2world = inst_data.model2world;
            inst_data.padding          = 0;
        }
    }

    Scene::Impl::~Impl() noexcept {

        for (auto& entity : m_entities) {
            MeshInfo&    info     = *RenderableManager::Get().GetMeshInfo(entity);
            MeshBuffers& buffers  = *info.buffers;
            buffers.index_buffer  = nullptr;
            buffers.vertex_buffer = nullptr;
        }
    }

    //////////////////////////////////////////////////////////////////////////
    // Scene
    //////////////////////////////////////////////////////////////////////////

    Scene::Scene() noexcept {
        m_impl = MoerNew(Impl)();
    }

    Scene::~Scene() noexcept {
        MoerDelete(m_impl);
    }

    void Scene::AddEntity(Entity entity) noexcept {
        m_impl->AddEntity(entity);
    }
    void Scene::SetBlasList(Moer::Array<RHIRayTracingBLASRef> _blas_list) noexcept {
        // m_impl->gpu_scene.blases =  std::move(_blas_list);
    }
    void Scene::SetRaytracingScene(Render::RaytracingSceneRef _scene) noexcept {
        m_impl->gpu_scene.rt_scene = _scene;
    }

    void Scene::SetTlas(RHIRayTracingTLASRef _tlas) noexcept {
        // m_impl->gpu_scene.tlas = _tlas;
    }
    void Scene::RemoveEntity(Entity entity) noexcept {
        m_impl->RemoveEntity(entity);
    }
    void Scene::SetBuffer(const std::string& name, RHIBufferRef buffer) noexcept {
        return m_impl->SetBuffer(name, buffer);
    }
    void Scene::SetBuffer(EGpuSceneResource _type, Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.global_resources.buffers[(uint32_t)_type] = _buffer;
    }
    Render::BufferRef Scene::GetBuffer(EGpuSceneResource _type) const noexcept {
        return m_impl->gpu_scene.global_resources.buffers[(uint32_t)_type];
    }
    RHIBufferRef Scene::GetBuffer(const std::string& _name) const noexcept {
        return nullptr;
    }

    void Scene::AddCamera(Entity entity) noexcept {
        m_impl->AddCamera(entity);
    }

    void Scene::AddLight(Entity entity) noexcept {
        m_impl->AddLight(entity);
    }

    Array<Entity> Scene::GetEntities() const noexcept {
        return m_impl->GetEntities();
    }

    Array<Entity> Scene::GetLights() const noexcept {
        return m_impl->GetLights();
    }

    Array<Entity> Scene::GetCameras() const noexcept {
        return m_impl->GetCameras();
    }

    Entity Scene::GetMainCamera() const noexcept {
        return GetCameras()[0];
    }

    bool Scene::IsEntitiesEmpty() const noexcept {
        return m_impl->IsEntitiesEmpty();
    }

    bool Scene::IsLightsEmpty() const noexcept {
        return m_impl->IsLightsEmpty();
    }

    bool Scene::IsCamerasEmpty() const noexcept {
        return m_impl->IsCamerasEmpty();
    }

    void Scene::ForEach(std::function<void(Entity)> _func) const noexcept {
        m_impl->ForEach(std::move(_func));
    }

    Scene* Scene::GetCurrentScene() noexcept {
        return g_scene;
    }
    void Scene::SetCurrentScene(Scene* _scene) noexcept {
        g_scene = _scene;
    }

    bool Scene::IsReady() const noexcept {
        return true;
    }

    uint Scene::GetEntityCount() const noexcept {
        return m_impl->m_entities.size();
    }

    void Scene::RegisterMaterialTextures(UnorderedMap<std::string, Render::TextureRef> _textures) noexcept {
        m_impl->gpu_scene.material_textures.insert(_textures.begin(), _textures.end());
    }

    GpuScene& Scene::GetGpuScene() noexcept {
        return m_impl->GetGpuScene();
    }

    void Scene::UpdateGpuData() {
        m_impl->UpdateGpuData();
    }

    std::span<const Render::GeometryData> Scene::GetGeometryDatas() const noexcept {
        return m_impl->GetGeometryDatas();
    }

    std::span<const Render::InstanceData> Scene::GetInstanceDatas() const noexcept {
        return m_impl->GetInstanceDatas();
    }

    std::span<const StaticArray<Render::VertexBuffer, VETA_Num>> Scene::GetVertexBufferViews() {
        return m_impl->vtx_views;
    }

    std::span<const Render::IndexBuffer> Scene::GetIndexBufferViews() {
        return m_impl->idx_views;
    }

    std::span<const Render::GeometryInstance> Scene::GetGeometryInstances() const noexcept {
        return m_impl->GetGeometryInstances();
    }

    void Scene::SetVertexBuffer(Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.vertex_buffer = _buffer;
    }

    void Scene::SetIndexBuffer(Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.index_buffer = _buffer;
    }
    void Scene::SetInstanceBuffer(Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.global_resources.buffers[(uint32_t)EGpuSceneResource::InstanceInfo] = _buffer;
    }
    Render::BufferRef Scene::GetVertexBuffer() const noexcept {
        return m_impl->gpu_scene.vertex_buffer;
    }
    Render::BufferRef Scene::GetIndexBuffer() const noexcept {
        return m_impl->gpu_scene.index_buffer;
    }
    Render::BufferRef Scene::GetInstanceBuffer() const noexcept {
        return m_impl->gpu_scene.GetGpuBuffer(EGpuSceneResource::InstanceInfo);
    }
    Render::BindlessArrayRef Scene::GetBindlessArray() const noexcept {
        return m_impl->gpu_scene.bindless_array;
    }

    AsyncSceneLoadInfoRef Scene::GetCurrentSceneLoadInfo() noexcept {
        return Impl::GetCurrentSceneLoadInfo();
    }

    bool Scene::RegisterAsyncLoadInfo(AsyncSceneLoadInfoRef _load_info) {
        if (Impl::m_load_info) {
            if (Impl::m_load_info.IsValid() && !Impl::m_load_info->IsReady()) {
                LOG_ERROR("Scene is already loading");
                return false;
            }
            //TODO: release current_scene
        }
        Impl::m_load_info = _load_info;
        return true;
    }

    void Scene::ResetAsyncLoadInfo() noexcept {
        Impl::m_load_info = nullptr;
    }

    Scene* AsyncSceneLoadInfo::TryGetScene() {
        if (progress.load(std::memory_order_acq_rel) == 1) {
            return scene;
        }
        return nullptr;
    }

}// namespace Moer