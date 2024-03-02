#include "scene/RenderableManager.h"

#include "rhi/RHI.h"
#include "scene/TransformManager.h"

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

    RenderableManager::Builder& RenderableManager::Builder::operator=(Builder&& rhs) noexcept { return *this; }
    RenderableManager::Builder& RenderableManager::Builder::Geometry(EPrimitiveType type, const Moer::Array<float>& vertex_data, const Moer::Array<uint32_t>& index_data, uint32_t offset, uint32_t count) noexcept {
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
        uint32_t       meshlet_count) noexcept {
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
        RenderableManager::Get().Create(*this, entity);

        auto& transfor_manager = TransformManager::Get();
        if (!transfor_manager.HasComponent(entity)) { transfor_manager.Create(entity); }
    }

    void RenderableManager::Create(Builder& builder, Entity entity) {
        m_manager.AddComponent(entity);

        SetCulling(entity, builder->m_culling);
        SetCastShadows(entity, builder->m_cast_shadows);

        m_manager[entity].vertex_data = std::make_unique<Moer::Array<float>>(std::move(builder->vertex_data));
        m_manager[entity].index_data  = std::make_unique<Moer::Array<uint32_t>>(std::move(builder->index_data));
    }

    void RenderableManager::Create(Entity entity) {
        m_manager.AddComponent(entity);
    }

    void RenderableManager::SetRHIRenderPrimitiveRef(Entity entity, RHIRenderPrimitiveRef primitive) {
        m_manager[entity].primitive = primitive;
    }
    void RenderableManager::SetCulling(Entity entity, bool culling) {
        m_manager[entity].culling = culling;
    }
    void RenderableManager::SetCastShadows(Entity entity, bool castShadows) {
        m_manager[entity].cast_shadows = castShadows;
    }
    void RenderableManager::SetMaterialInstance(Entity entity, MaterialInstanceRef material_instance) {
        m_manager[entity].material_instance = material_instance;
    }
    void RenderableManager::SetMeshInfo(Entity entity, const MeshInfo& mesh_info) {
        m_manager[entity].mesh_info = mesh_info;
    }
    const MeshInfo& RenderableManager::GetMeshInfo(Entity entity) {
        return m_manager[entity].mesh_info;
    }
    MaterialInstanceRef RenderableManager::GetMaterialInstance(Entity entity) {
        return m_manager[entity].material_instance;
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

    RHIRenderPrimitiveRef RenderableManager::GetRenderPrimitive(Entity entity) {
        return m_manager[entity].primitive;
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
}// namespace Moer