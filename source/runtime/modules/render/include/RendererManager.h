#ifndef MOER_ENGINE_RENDERER_MANAGER_H
#define MOER_ENGINE_RENDERER_MANAGER_H
#include "misc/Singleton.h"
#include "renderer/BackendRenderer.h"
#include <string>

#define MOER_DEFERRED_RENDERER_NAME "DeferredRenderer"
#define MOER_DEFAULT_RENDERER_NAME  MOER_DEFERRED_RENDERER_NAME
namespace Moer {
    using TRendererOutput = void*;
    using TRendererID     = int32_t;
    struct RendererManagerData;
    class RendererManager : public Singleton<RendererManager> {
    public:
        ~RendererManager() = default;
        void Init();
        void ShutDown();
        void DrawFrame();

        void SetRendererPresentResolution(TRendererID _renderer_id, uint32_t _width, uint32_t _height);
        //call from editor UI
        void SetRendererOriginResolution(TRendererID _renderer_id, uint32_t _width, uint32_t _height);
        //call this function to get the renderer output
        TRendererOutput GetRendererOutput(TRendererID _renderer_id);

        TRendererID GetRendererID(const std::string& _renderer_name);

    private:
        friend BackendRenderer;
        void RegisterRenderer(const std::string& _name, BackendRenderer* _renderer);
        void UnregisterRenderer(const std::string& _name);

        BackendRenderer* GetRenderer(TRendererID _renderer_id);

    protected:
        RendererManager() = default;
        RendererManagerData* data;
    };
}// namespace Moer
#endif//MOER_ENGINE_RENDERER_MANAGER_H