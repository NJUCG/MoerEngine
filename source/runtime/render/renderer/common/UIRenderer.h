#ifndef MOER_ENGINE_UI_RENDERER_H
#define MOER_ENGINE_UI_RENDERER_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "renderer/common/ui/ImGuiIOInput.h"
#include "rhi/RHI.h"
#include <cstdint>
#include <string_view>

namespace Moer::Render {
class UIRenderer {
public:
    struct WindowRenderTarget {
        bool        is_separate_window = false;
        TextureView frame_buffer;
    };

    struct Impl;
    RENDER_API UIRenderer(RenderDevice& _device);

    RENDER_API virtual ~UIRenderer();

    RENDER_API void BeginGUIFrame();

    RENDER_API void EndGUIFrame();

    RENDER_API const ImGuiIOInputSnapshot& GetInputSnapshot() const;
    RENDER_API void RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer);
    RENDER_API void RegisterImage(Texture* _texture, Sampler _sampler);
    RENDER_API void UnRegisterImage(Texture* _texture);

    RENDER_API WindowRenderTarget GetWindowRenderTarget(std::string_view window_name);
    RENDER_API void        PresentWindows();

private:
    UniquePtr<Impl> impl;
    // RENDER_API virtual void UploadFonts(FontDesc _font_desc) = 0;
};
}; // namespace Moer::Render

#endif //MOER_ENGINE_UI_RENDERER_H
