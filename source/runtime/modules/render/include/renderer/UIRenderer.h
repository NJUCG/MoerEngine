#ifndef MOER_ENGINE_UI_RENDERER_H
#define MOER_ENGINE_UI_RENDERER_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include <cstdint>

struct FontUpdateEvent {
    void* font_data;
};
class UIRenderer {
public:
    RENDER_API UIRenderer() = default;

    RENDER_API static UIRenderer* GetRenderer();

    RENDER_API virtual ~UIRenderer()   = default;
    RENDER_API virtual void Init()     = 0;
    RENDER_API virtual void ShutDown() = 0;

    RENDER_API virtual void RegisterImage(uint64_t _handle)   = 0;
    RENDER_API virtual void UnRegisterImage(uint64_t _handle) = 0;

    RENDER_API virtual void BeginRenderFrame() = 0;

    RENDER_API virtual void EndRenderFrame() = 0;

    // RENDER_API virtual void UploadFonts(FontDesc _font_desc) = 0;
};

namespace Moer::Render {
    class UIRenderer {
    public:
        struct Impl;
        RENDER_API UIRenderer(RenderDevice& _device);

        RENDER_API virtual ~UIRenderer();

        RENDER_API void BeginGUIFrame();

        RENDER_API void EndGUIFrame();

        RENDER_API void RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer);
        RENDER_API void RegisterImage(Texture* _texture, Sampler _sampler);
        RENDER_API void UnRegisterImage(Texture* _texture);

        RENDER_API TextureView GetWindowFrameBuffer(void* _window);
        RENDER_API void        PresentWindows();

    private:
        UniquePtr<Impl> impl;
        // RENDER_API virtual void UploadFonts(FontDesc _font_desc) = 0;
    };
};// namespace Moer::Render

#endif//MOER_ENGINE_UI_RENDERER_H