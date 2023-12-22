#pragma once
#include "ECS.h"
#include "Entity.h"
#include "rhi/RHIResource.h"

namespace Moer {
    class RENDER_API RenderableManager  {
        struct Proxy {
            RHIRenderPrimitiveRef primitive{nullptr};
            std::unique_ptr<std::vector<float>> vertex_data{};
            std::unique_ptr<std::vector<uint32_t>> index_data{};
            bool                  culling{false};
            bool                  cast_shadows{false};
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
            Builder& Geometry(EPrimitiveType type, const std::vector<float> & vertex_data, const std::vector<uint32_t> & index_data, uint32_t offset, uint32_t count) noexcept;
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

        RHIRenderPrimitiveRef GetRenderPrimitive(Entity entity);
        bool GetCulling(Entity entity);
        const std::vector<float>& GetVertexData(Entity entity);
        const std::vector<uint32_t>& GetIndexData(Entity entity);

        static RenderableManager& Get();

    protected:
        inline static std::unique_ptr<RenderableManager> m_instance = nullptr;
    };

}