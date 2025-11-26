#include "scene/RenderableManager.h"

#include "rhi/RHI.h"
#include "scene/Material.h"
#include "scene/Scene.h"
#include "scene/TransformManager.h"
#include "shaderheaders/shared/Geometry.h"

// struct Entry {
//     EPrimitiveType type;
//     Moer::Array<float>  vertex_data;
//     Moer::Array<uint32_t> index_data;
//     uint32_t       offset;
//     uint32_t       count;
// };
namespace Moer {

struct RenderableManager::BuilderDetails {
    EPrimitiveType        type;
    Moer::Array<float>    vertex_data;
    Moer::Array<uint32_t> index_data;
    uint32_t              vertex_count;
    uint32_t              index_count;
    uint32_t              vertex_offset;
    uint32_t              index_offset;
    uint32_t              meshlet_offset;
    uint32_t              meshlet_count;
    uint16_t              m_instance_count{1};
    bool                  m_culling{};
    bool                  m_cast_shadows{};
};

RenderableManager::Builder::Builder() noexcept {}

RenderableManager::Builder::Builder(Builder&& rhs) noexcept {}

RenderableManager::Builder::~Builder() noexcept {}

RenderableManager::Builder& RenderableManager::Builder::operator=(Builder&& rhs) noexcept {
    return *this;
}
RenderableManager::Builder& RenderableManager::Builder::Geometry(
    EPrimitiveType               type,
    const Moer::Array<float>&    vertex_data,
    const Moer::Array<uint32_t>& index_data,
    uint32_t                     offset,
    uint32_t                     count
) noexcept {
    m_impl->type          = type;
    m_impl->vertex_data   = vertex_data;
    m_impl->index_data    = index_data;
    m_impl->vertex_offset = offset;
    m_impl->index_offset  = count;
    return *this;
}
RenderableManager::Builder& RenderableManager::Builder::Geometry(
    EPrimitiveType type,
    uint32_t       vertex_count,
    uint32_t       index_count,
    uint32_t       vertex_offset,
    uint32_t       index_offset,
    uint32_t       meshlet_offset,
    uint32_t       meshlet_count
) noexcept {
    m_impl->type           = type;
    m_impl->vertex_count   = vertex_count;
    m_impl->index_count    = index_count;
    m_impl->vertex_offset  = vertex_offset;
    m_impl->index_offset   = index_offset;
    m_impl->meshlet_offset = meshlet_offset;
    m_impl->meshlet_count  = meshlet_count;
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::Culling(bool Culling) {
    m_impl->m_culling = Culling;
    return *this;
}
RenderableManager::Builder& RenderableManager::Builder::CastShadows(bool castShadows) {
    m_impl->m_cast_shadows = castShadows;
    return *this;
}

void RenderableManager::Builder::Build(Entity entity) noexcept {
    RenderableManager::Get().CreateMesh(*this, entity);

    auto& transfor_manager = TransformManager::Get();
    if (!transfor_manager.HasComponent(entity)) {
        transfor_manager.Create(entity);
    }
}

void RenderableManager::CreateMesh(Builder& _builder, Entity _entity) {
    m_manager.AddComponent(_entity);

    SetCulling(_entity, _builder->m_culling);
    SetCastShadows(_entity, _builder->m_cast_shadows);

    m_manager[_entity].vertex_data = std::make_unique<Moer::Array<float>>(std::move(_builder->vertex_data));
    m_manager[_entity].index_data  = std::make_unique<Moer::Array<uint32_t>>(std::move(_builder->index_data));
}

void RenderableManager::CreateMeshInstance(Entity _entity) {
    m_manager.AddComponent(_entity);
}

void RenderableManager::SetCulling(Entity entity, bool culling) {
    m_manager[entity].culling = culling;
}
void RenderableManager::SetCastShadows(Entity entity, bool castShadows) {
    m_manager[entity].cast_shadows = castShadows;
}
void RenderableManager::SetMaterialInstances(
    Entity                       _entity,
    Array<MaterialInstanceRef>&& _material_instance
) {
    m_manager[_entity].material_instances = std::move(_material_instance);
}
void RenderableManager::SetMeshInfo(Entity entity, SharedPtr<MeshInfo> _mesh_info) {
    // m_manager[entity]. = mesh_info;
    m_manager[entity].mesh_info = _mesh_info;
}

void RenderableManager::SetInstanceID(Entity entity, int instance_id) {
    m_manager[entity].instance_id = instance_id;
}

void RenderableManager::SetGeomInstanceID(Entity entity, int geom_instance_id) {
    m_manager[entity].geom_instance_id = geom_instance_id;
}

void RenderableManager::ModifyMeshInfo(Entity entity, std::function<void(MeshInfo&)>&& _func) {
    _func(*m_manager[entity].mesh_info);
}

const SharedPtr<MeshInfo>& RenderableManager::GetMeshInfo(Entity entity) {
    return m_manager[entity].mesh_info;
}

std::span<MaterialInstanceRef> RenderableManager::GetMaterialInstances(Entity _entity) {
    return m_manager[_entity].material_instances;
}

RenderableManager& RenderableManager::Get() {
    static UniquePtr<RenderableManager> m_instance = nullptr;
    if (!m_instance)
        m_instance = std::move(UniquePtr<RenderableManager>(MoerNew(RenderableManager)()));
    return *m_instance;
}

void RenderableManager::Destroy(Entity entity) {
    m_manager.RemoveComponent(entity);
    //todo destroy real obj
}

bool RenderableManager::Contains(Entity entity) {
    return m_manager.HasComponent(entity);
}

bool RenderableManager::GetCulling(Entity entity) {
    return m_manager[entity].culling;
}
const Moer::Array<float>& RenderableManager::GetVertexData(Entity entity) {
    // return m_manager[entity].vertex_data;
    return *m_manager[entity].vertex_data;
}
const Moer::Array<uint32_t>& RenderableManager::GetIndexData(Entity entity) {
    //   return m_manager[entity].index_data;
    return *m_manager[entity].index_data;
}

int RenderableManager::GetInstanceID(Entity _entity) {
    return m_manager[_entity].instance_id;
}

int RenderableManager::GetGeomInstanceID(Entity _entity) {
    return m_manager[_entity].geom_instance_id;
}

std::span<const StaticArray<Render::VertexBuffer, VA_NUM>> RenderableManager::GetVertexBuffer(Entity _entity
) {
    return m_manager[_entity].vertex_buffers;
}

} // namespace Moer