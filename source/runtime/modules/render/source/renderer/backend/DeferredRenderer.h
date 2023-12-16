#ifndef MOER_ENGINE_DEFERRED_RENDERER_H
#define MOER_ENGINE_DEFERRED_RENDERER_H
#include "renderer/BackendRenderer.h"
namespace Moer {

    class DeferredRenderer : public BackendRenderer {
    public:
        DeferredRenderer()          = default;
        virtual ~DeferredRenderer() = default;
        virtual void Init(const BackendRendererInitInfo& _init_info) override;
        virtual void ShutDown() override;
        virtual void DrawFrame() override;
        virtual void Present() override;
        virtual void SetOriginResolution(uint32_t _width, uint32_t _height) override;
        virtual void SetPresentResolution(uint32_t _width, uint32_t _height) override;

    private:
        class Impl;
        Impl* impl;
    };
}// namespace Moer

#endif//MOER_ENGINE_DEFERRED_RENDERER_H