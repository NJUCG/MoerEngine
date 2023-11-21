#ifndef MOER_ENGINE_UI_RENDERER_H
#define MOER_ENGINE_UI_RENDERER_H

#include "RenderAPI.h"
#include <cstdint>

class UIRenderer {
public:
    RENDER_API UIRenderer() = default;

    RENDER_API static UIRenderer* GetRenderer();

    RENDER_API virtual ~UIRenderer()   = default;
    RENDER_API virtual void Init()     = 0;
    RENDER_API virtual void ShutDown() = 0;

    RENDER_API virtual void BeginRenderFrame() = 0;

    RENDER_API virtual void EndRenderFrame() = 0;
};

#endif//MOER_ENGINE_UI_RENDERER_H