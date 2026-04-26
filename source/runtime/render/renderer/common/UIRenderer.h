#ifndef MOER_ENGINE_UI_RENDERER_H
#define MOER_ENGINE_UI_RENDERER_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "renderer/common/ui/ImGuiIOInput.h"
#include "rhi/RHI.h"
#include <cstdint>

namespace Moer::Render {
class UIRenderer {
public:
    struct RenderOutputSlotHandle {
        static constexpr uint32_t InvalidIndex = 0xFFFFFFFFu;

        uint32_t slot_index = InvalidIndex;
        uint32_t generation = 0;

        bool IsValid() const {
            return slot_index != InvalidIndex && generation != 0;
        }
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

    RENDER_API RenderOutputSlotHandle RegisterRenderOutputSlot(uint32_t imgui_id);
    RENDER_API uint64_t GetRenderOutputTextureId(RenderOutputSlotHandle handle) const;
    RENDER_API void PublishRenderOutput(RenderOutputSlotHandle handle, TextureView resource);
    RENDER_API void        PresentWindows();

private:
    UniquePtr<Impl> impl;
    // RENDER_API virtual void UploadFonts(FontDesc _font_desc) = 0;
};
}; // namespace Moer::Render

#endif //MOER_ENGINE_UI_RENDERER_H
