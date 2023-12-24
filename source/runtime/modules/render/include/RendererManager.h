#ifndef MOER_ENGINE_RENDERER_MANAGER_H
#define MOER_ENGINE_RENDERER_MANAGER_H
#include "RenderAPI.h"
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
        RENDER_API void Init();
        RENDER_API void ShutDown();
        RENDER_API void DrawFrame();
        RENDER_API void Present();

        RENDER_API void SetRendererPresentResolution(TRendererID _renderer_id, uint32_t _width, uint32_t _height);
        //call from editor UI
        RENDER_API void SetRendererOriginResolution(TRendererID _renderer_id, uint32_t _width, uint32_t _height);
        //call this function to get the renderer output
        RENDER_API TRendererOutput GetRendererOutput(TRendererID _renderer_id);

        RENDER_API TRendererID GetRendererID(const std::string& _renderer_name);

    private:
        friend BackendRenderer;
        void RegisterRenderer(const std::string& _name, BackendRenderer* _renderer);
        void UnregisterRenderer(const std::string& _name);

        BackendRenderer* GetRenderer(TRendererID _renderer_id);

    protected:
        friend class Singleton<RendererManager>;
        RendererManager(){};
        RendererManagerData* data;
    };
}// namespace Moer
#endif//MOER_ENGINE_RENDERER_MANAGER_H