#include "scene/Scene.h"

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
        void         AddEntity(Entity _entity) noexcept { m_entities.AddEntity(_entity); }
        void         AddCamera(Entity _entity) noexcept { m_cameras.AddEntity(_entity); }
        void         AddLight(Entity _entity) noexcept { m_lights.AddEntity(_entity); }
        void         RemoveLight(Entity _entity) noexcept { m_lights.RemoveEntity(_entity); }
        void         RemoveEntity(Entity _entity) noexcept { m_entities.RemoveEntity(_entity); };
        void         SetBuffer(const std::string& _name, RHIBufferRef _buffer) { m_buffers[_name] = _buffer; }
        RHIBufferRef GetBuffer(const std::string& _name) const { return m_buffers.at(_name); }
        RHIUAVRef    GetUAV(const std::string& _name) const { return m_uavs.at(_name); }
        RHISRVRef    GetSRV(const std::string& _name) const { return m_srvs.at(_name); }
        void         ForEach(std::function<void(Entity)> _func) const noexcept {
            auto span = m_entities.GetEntities();
            for (auto& entity : span) {
                _func(entity);
            }
        }
        std::span<const Entity>      GetEntities() const noexcept;
        std::span<const Entity>      GetCameras() const noexcept;
        std::span<const Entity>      GetLights() const noexcept;
        bool                         IsEntitiesEmpty() const noexcept { return m_entities.GetEntities().empty(); }
        bool                         IsCamerasEmpty() const noexcept { return m_cameras.GetEntities().empty(); }
        bool                         IsLightsEmpty() const noexcept { return m_lights.GetEntities().empty(); }
        static AsyncSceneLoadInfoRef GetCurrentSceneLoadInfo() noexcept { return m_load_info; }

        GpuScene& GetGpuScene() noexcept { return gpu_scene; }

        void                                      UpdateGpuData();
        std::span<const Render::GeometryData>     GetGeometryDatas() const noexcept { return geometry_datas; }
        std::span<const Render::InstanceData>     GetInstanceDatas() const noexcept { return instance_datas; }
        std::span<const Render::GeometryInstance> GetGeometryInstances() { return geom_instances; }
        std::span<const Render::BufferRef>        GetIOPendingBuffers() const noexcept { return io_pending_buffers; }
        void                                      ClearIOPendingBuffers() noexcept { io_pending_buffers.clear(); }

        void           SetCurrentEnvMap(EnvMapResource _env_map) { cur_env_map = _env_map; }
        EnvMapResource GetCurrentEnvMap() const { return cur_env_map; }
        void           EmplaceIOImportedBuffer(Render::BufferRef _buffer) { io_pending_buffers.emplace_back(_buffer); }

    protected:
    private:
        Map<std::string, RHIBufferRef> m_buffers;
        Map<std::string, RHIUAVRef>    m_uavs;
        Map<std::string, RHISRVRef>    m_srvs;

        UniqueEntityArray m_entities;
        UniqueEntityArray m_cameras;
        UniqueEntityArray m_lights;

        static AsyncSceneLoadInfoRef m_load_info;
        GpuScene                     gpu_scene;

        Array<Render::GeometryData>                                               geometry_datas;
        Array<Render::InstanceData>                                               instance_datas;
        Array<UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>> vtx_views;
        Array<UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>>         idx_views;
        Array<Render::GeometryInstance>                                           geom_instances;

        EnvMapResource cur_env_map{};

        Array<Render::BufferRef> io_pending_buffers;
    };
    AsyncSceneLoadInfoRef Scene::Impl::m_load_info{nullptr};

    Scene::Impl::Impl() noexcept {
        gpu_scene.bindless_array = Render::RenderDevice::Get().CreateBindlessArray();
    }
    std::span<const Entity> Scene::Impl::GetEntities() const noexcept {
        return m_entities.GetEntities();
    }

    std::span<const Entity> Scene::Impl::GetCameras() const noexcept {
        return m_cameras.GetEntities();
    }

    std::span<const Entity> Scene::Impl::GetLights() const noexcept {
        return m_lights.GetEntities();
    }

    void Scene::Impl::UpdateGpuData() {
        uint geom_instance_cnt = 0;
        uint instance_count    = 0;
        for (auto& entity : m_entities.GetEntities()) {
            const MeshInfo& info = *RenderableManager::Get().GetMeshInfo(entity);
            geom_instance_cnt += info.geometries.size();
            instance_count += 1;
        }

        LOG_INFO("UpdateGpuData, geometry_count:{}, instance_count:{}", geom_instance_cnt, instance_count);

        geometry_datas.resize(geom_instance_cnt);//reserve more space than needed
        geom_instances.resize(geom_instance_cnt);
        vtx_views.resize(instance_count);
        idx_views.resize(instance_count);
        instance_datas.resize(instance_count);

        geom_instance_cnt = 0;
        for (auto& entity : m_entities.GetEntities()) {
            const MeshInfo&                info          = *RenderableManager::Get().GetMeshInfo(entity);
            std::span<MaterialInstanceRef> mat_instances = RenderableManager::Get().GetMaterialInstances(entity);

            UnorderedMap<VertexAttributesBitmask, SharedPtr<MeshBuffers>> bitmask_to_mesh_buffers_map;

            for (auto& geo : info.geometries) {
                // uint               vtx_offset = geo->local_vtx_offset + info.vtx_offset;
                uint               vtx_offset = geo->local_vtx_offset;// FIXME
                uint               geo_idx    = &geo - info.geometries.data();
                auto&              geo_data   = geometry_datas[geo->global_geom_idx];
                const MeshBuffers& buffers    = *geo->mesh_buffers;

                bitmask_to_mesh_buffers_map[buffers.vertex_factory_buffers.GetAttributesBitmask()] = geo->mesh_buffers;

                geo_data.num_indices          = geo->local_idx_count;
                geo_data.num_vertices         = geo->local_vtx_count;
                geo_data.vertex_offset        = vtx_offset * sizeof(float3);
                geo_data.prev_vertex_offset   = ~0u;
                geo_data.normal_offset        = buffers.GetAttributeRange(EVertexAttributes::VA_NORMAL).offset + vtx_offset * VertexAttributesTool::GetSize(EVertexAttributes::VA_NORMAL);
                geo_data.tangent_offset       = buffers.GetAttributeRange(EVertexAttributes::VA_TANGENT).offset + vtx_offset * VertexAttributesTool::GetSize(EVertexAttributes::VA_TANGENT);
                geo_data.texcoord0_offset     = buffers.GetAttributeRange(EVertexAttributes::VA_TEXCOORD0).offset + vtx_offset * VertexAttributesTool::GetSize(EVertexAttributes::VA_TEXCOORD0);
                geo_data.texcoord1_offset     = buffers.GetAttributeRange(EVertexAttributes::VA_TEXCOORD1).offset + vtx_offset * VertexAttributesTool::GetSize(EVertexAttributes::VA_TEXCOORD1);
                geo_data.mat_idx_and_type     = geo->material_id << 8 | (uint)mat_instances[geo_idx]->GetMaterial()->GetType();
                geo_data.index_offset         = geo->local_idx_offset * sizeof(uint);
                geo_data.index_buffer_handle  = buffers.idx_bdls_handle;
                geo_data.vertex_buffer_handle = buffers.vtx_bdls_handle;

                auto& geom_instance        = geom_instances[geom_instance_cnt];
                geom_instance.instance_idx = RenderableManager::Get().GetInstanceID(entity);
                geom_instance.geom_idx     = geo->global_geom_idx;

                geom_instance_cnt++;
            }

            {
                // Initialize vtx_view & idx_view
                UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>& vtx_view = vtx_views[info.global_mesh_idx];
                UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>&         idx_view = idx_views[info.global_mesh_idx];

                for (auto& [bitmask, mesh_buffers] : bitmask_to_mesh_buffers_map) {
                    Array<Render::VertexBuffer> vtxs;

                    // 注意，这里vtxs的顺序不能被改变！具体顺序应当由VertexAttributesTool::GetArrayFromBitmask返回的数组决定！
                    // => 逻辑关联处：RHICommand.h -> MeshDrawData
                    auto attrs = VertexAttributesTool::GetArrayFromBitmask(bitmask);

                    vtxs.reserve(attrs.size());
                    for (auto& attr : attrs) {
                        vtxs.push_back({mesh_buffers->vertex_buffer.Get(), mesh_buffers->GetAttributeRange(attr).offset});
                    }
                    vtx_view[bitmask] = std::move(vtxs);

                    idx_view[bitmask] = {mesh_buffers->index_buffer->GetView(),
                                         EIndexElementType::IET_UINT32};
                }
            }

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

        for (auto& entity : m_entities.GetEntities()) {
            MeshInfo& info = *RenderableManager::Get().GetMeshInfo(entity);
            for (auto& geo : info.geometries) {
                MeshBuffers& buffers = *geo->mesh_buffers;// 这里会重复访问同一个mesh_buffersss，重复清空，但是只会略微影响析构效率，问题不大

                buffers.index_buffer  = nullptr;
                buffers.vertex_buffer = nullptr;
            }
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

    void Scene::RemoveLight(Entity entity) noexcept {
        m_impl->RemoveLight(entity);
    }

    std::span<const Entity> Scene::GetEntities() const noexcept {
        return m_impl->GetEntities();
    }

    std::span<const Entity> Scene::GetLights() const noexcept {
        return m_impl->GetLights();
    }

    std::span<const Entity> Scene::GetCameras() const noexcept {
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
        return m_impl->m_entities.GetEntities().size();
    }

    EnvMapResource Scene::GetCurrentEnvMap() const noexcept {
        return m_impl->GetCurrentEnvMap();
    }

    void Scene::SetCurrentEnvMap(EnvMapResource _env_map) {
        m_impl->SetCurrentEnvMap(_env_map);
    }

    void Scene::RegisterMaterialTextures(UnorderedMap<std::string, SceneTexture> _textures) noexcept {
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

    std::span<const UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>> Scene::GetVertexBufferViews() {
        return m_impl->vtx_views;
    }

    std::span<const UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>> Scene::GetIndexBufferViews() {
        return m_impl->idx_views;
    }

    std::span<const Render::GeometryInstance> Scene::GetGeometryInstances() const noexcept {
        return m_impl->GetGeometryInstances();
    }

    void Scene::SetInstanceBuffer(Render::BufferRef _buffer) noexcept {
        m_impl->gpu_scene.global_resources.buffers[(uint32_t)EGpuSceneResource::InstanceInfo] = _buffer;
    }

    Render::BufferRef Scene::GetInstanceBuffer() const noexcept {
        return m_impl->gpu_scene.GetGpuBuffer(EGpuSceneResource::InstanceInfo);
    }
    Render::BindlessArrayRef Scene::GetBindlessArray() const noexcept {
        return m_impl->gpu_scene.bindless_array;
    }

    std::span<const Render::BufferRef> Scene::GetIOPendingBuffers() const noexcept {
        return m_impl->GetIOPendingBuffers();
    }

    void Scene::ClearIOPendingBuffers() noexcept {
        m_impl->ClearIOPendingBuffers();
    }

    void Scene::EmplaceIOImportedBuffer(Render::BufferRef _buffer) {
        m_impl->EmplaceIOImportedBuffer(_buffer);
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