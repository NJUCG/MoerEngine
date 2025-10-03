#pragma once
#include "ECS.h"
#include "Entity.h"
#include "MaterialInstance.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shaderheaders/shared/Geometry.h"
#include <functional>
#include <memory>

namespace Moer {
    class RENDER_API RenderableManager {
        struct Proxy {
            std::unique_ptr<Moer::Array<float>>              vertex_data{};
            std::unique_ptr<Moer::Array<uint32_t>>           index_data{};
            bool                                             culling{false};
            bool                                             cast_shadows{false};
            SharedPtr<MeshInfo>                              mesh_info{};
            Array<StaticArray<Render::VertexBuffer, VA_NUM>> vertex_buffers{};
            // MaterialInstanceRef                    material_instance{nullptr};
            Array<MaterialInstanceRef> material_instances{};
            int                        instance_id      = -1;
            int                        geom_instance_id = -1;
            Proxy()                                     = default;
        };

        struct RENDER_API            BuilderDetails;
        EntityComponentManger<Proxy> m_manager;
        Array<Render::GeometryData>  geometry_datas;
        Array<Render::InstanceData>  instance_datas;

    public:
        class RENDER_API Builder : public PrivateImplementation<BuilderDetails> {
        public:
            explicit Builder() noexcept;

            /*! \cond PRIVATE */
            Builder(const Builder& rhs) = delete;
            Builder(Builder&& rhs) noexcept;
            ~Builder() noexcept;
            Builder& operator=(Builder& rhs) = delete;
            Builder& operator=(Builder&& rhs) noexcept;

            // Builder & Geometry(EPrimitiveType type,RHIBufferRef vbh,RHIBufferRef ibh) noexcept;
            Builder& Geometry(EPrimitiveType type, const Moer::Array<float>& vertex_data, const Moer::Array<uint32_t>& index_data, uint32_t offset, uint32_t count) noexcept;
            Builder& Geometry(
                EPrimitiveType type,
                uint32_t       vertex_count,
                uint32_t       index_count,
                uint32_t       vertex_offset,
                uint32_t       index_offset,
                uint32_t       meshlet_offset,
                uint32_t       meshlet_count) noexcept;
            Builder& Culling(bool Culling);
            Builder& CastShadows(bool castShadows);

            void Build(Entity entity) noexcept;

        private:
            friend class RenderableManager;
        };

        void CreateMesh(Builder& _builder, Entity _entity);
        void CreateMeshInstance(Entity entity);
        void Destroy(Entity entity);
        bool Contains(Entity entity);

        void SetCulling(Entity entity, bool culling);
        void SetCastShadows(Entity entity, bool castShadows);
        void SetMaterialInstances(Entity _entity, Array<MaterialInstanceRef>&& _material_instances);
        void SetMeshInfo(Entity entity, SharedPtr<MeshInfo> _mesh_info);
        void SetInstanceID(Entity entity, int instance_id);
        void SetGeomInstanceID(Entity entity, int geom_instance_id);

        bool                                                       GetCulling(Entity entity);
        const Moer::Array<float>&                                  GetVertexData(Entity entity);
        const Moer::Array<uint32_t>&                               GetIndexData(Entity entity);
        std::span<MaterialInstanceRef>                             GetMaterialInstances(Entity _entity);
        const SharedPtr<MeshInfo>&                                 GetMeshInfo(Entity entity);
        void                                                       ModifyMeshInfo(Entity entity, std::function<void(MeshInfo&)>&& _func);
        int                                                        GetInstanceID(Entity entity);
        int                                                        GetGeomInstanceID(Entity entity);
        std::span<const StaticArray<Render::VertexBuffer, VA_NUM>> GetVertexBuffer(Entity _entity);

        static RenderableManager& Get();

    protected:
    };

}// namespace Moer