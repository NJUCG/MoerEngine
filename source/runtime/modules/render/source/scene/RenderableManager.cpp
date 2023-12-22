#include "scene/RenderableManager.h"

#include "rhi/RHI.h"
#include "scene/TransformManager.h"

// struct Entry {
//     EPrimitiveType type;
//     std::vector<float>  vertex_data;
//     std::vector<uint32_t> index_data;
//     uint32_t       offset;
//     uint32_t       count;
// };
namespace Moer {
    
    struct RenderableManager::BuilderDetails {
        EPrimitiveType type;
        std::vector<float>  vertex_data;
        std::vector<uint32_t> index_data;
        uint32_t       offset;
        uint32_t       count;
        uint16_t       m_instance_count{1};
        bool m_culling{};
        bool m_castShadows{};
    };

    RenderableManager::Builder::Builder() noexcept {}

    RenderableManager::Builder::Builder(Builder&& rhs) noexcept {}

    RenderableManager::Builder::~Builder() noexcept {}

    RenderableManager::Builder& RenderableManager::Builder::operator=(Builder&& rhs) noexcept { return *this; }
    RenderableManager::Builder& RenderableManager::Builder::Geometry(EPrimitiveType type, const std::vector<float>& vertex_data, const std::vector<uint32_t>& index_data, uint32_t offset, uint32_t count) noexcept {
        m_impl->type = type;
        m_impl->vertex_data = vertex_data;
        m_impl->index_data = index_data;
        m_impl->offset = offset;
        m_impl->count = count;
        return *this;
    }



    RenderableManager::Builder& RenderableManager::Builder::Culling(bool Culling) {
        m_impl->m_culling = Culling;
        return *this;
    }
    RenderableManager::Builder& RenderableManager::Builder::CastShadows(bool castShadows) {
        m_impl->m_castShadows = castShadows;
        return *this;
    }

    void  RenderableManager::Builder::Build(Entity entity) noexcept {
        RenderableManager::Get().Create(*this, entity);
        
        auto& transforManager = TransformManager::Get();
        if (!transforManager.HasComponent(entity)) { transforManager.Create(entity); }
    }

    void RenderableManager::Create( Builder& builder, Entity entity) {
        m_manager.AddComponent(entity);

        SetCulling(entity, builder->m_culling);
        SetCastShadows(entity, builder->m_castShadows);

        m_manager[entity].vertex_data= std::make_unique<std::vector<float>>(std::move(builder->vertex_data));
        m_manager[entity].index_data = std::make_unique<std::vector<uint32_t>>(std::move(builder->index_data));
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

    RenderableManager& RenderableManager::Get() {
        if(!m_instance)
            m_instance = std::make_unique<RenderableManager>();
        return *m_instance;
    }


    void RenderableManager::Destroy(Entity entity) {
        m_manager.RemoveComponent(entity);
        //todo destroy real obj 
    }

    RHIRenderPrimitiveRef        RenderableManager::GetRenderPrimitive(Entity entity) {
        return m_manager[entity].primitive;
    }
    bool  RenderableManager::GetCulling(Entity entity) {
        return m_manager[entity].culling;
    }
    const std::vector<float>&    RenderableManager::GetVertexData(Entity entity) {
        // return m_manager[entity].vertex_data;
        return *m_manager[entity].vertex_data;
    }
    const std::vector<uint32_t>& RenderableManager::GetIndexData(Entity entity) {
        //   return m_manager[entity].index_data;
        return *m_manager[entity].index_data;
    }
}