#pragma once
#include "ECS.h"
#include "Entity.h"
#include "MaterialInstance.h"
#include "rhi/RHIResource.h"

namespace Moer {
    class RENDER_API RenderableManager  {
        struct Proxy {
            RHIRenderPrimitiveRef primitive{nullptr};
            std::unique_ptr<Moer::Array<float>> vertex_data{};
            std::unique_ptr<Moer::Array<uint32_t>> index_data{};
            bool                  culling{false};
            bool                  cast_shadows{false};
            MaterialInstanceRef   material_instance{nullptr};
            Proxy() = default;
        };

        struct RENDER_API BuilderDetails;
        EntityComponentManger<Proxy> m_manager;

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
            Builder& Geometry(EPrimitiveType type, const Moer::Array<float> & vertex_data, const Moer::Array<uint32_t> & index_data, uint32_t offset, uint32_t count) noexcept;
            Builder& Culling(bool Culling);
            Builder& CastShadows(bool castShadows);

            void Build(Entity entity) noexcept;

        private:
            friend class RenderableManager;
        };

        void Create( Builder& builder, Entity entity);
        void Create(Entity entity);
        void Destroy(Entity entity);

        void SetRHIRenderPrimitiveRef(Entity entity, RHIRenderPrimitiveRef primitive);
        void SetCulling(Entity entity, bool culling);
        void SetCastShadows(Entity entity, bool castShadows);
        void SetMaterialInstance(Entity entity, MaterialInstanceRef material_instance);

        RHIRenderPrimitiveRef GetRenderPrimitive(Entity entity);
        bool GetCulling(Entity entity);
        const Moer::Array<float>& GetVertexData(Entity entity);
        const Moer::Array<uint32_t>& GetIndexData(Entity entity);
        MaterialInstanceRef GetMaterialInstance(Entity entity);

        static RenderableManager& Get();

    protected:
        inline static std::unique_ptr<RenderableManager> m_instance = nullptr;
    };

}