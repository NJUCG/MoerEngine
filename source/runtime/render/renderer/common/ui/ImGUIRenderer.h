#ifndef MOER_ENGINE_IMGUI_RENDERER_H
#define MOER_ENGINE_IMGUI_RENDERER_H
#include "misc/STL.h"
#include "renderer/common/UIRenderer.h"
#include "renderer/common/ui/ImGuiIOInput.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include <string_view>
#define ImTextureID uint64_t
struct UIFrameData;

namespace Moer::Render {
class ImGUIRenderBackend {
public:
    ImGUIRenderBackend(RenderDevice& _device);
    ~ImGUIRenderBackend();

    void RegisterImage(Texture* _texture, Sampler _sampler);
    void UnRegisterImage(Texture* _texture);

    void BeginGUIFrame();
    void EndGUIFrame();
    const ImGuiIOInputSnapshot& GetInputSnapshot() const;

    void RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer);

    void PresentWindows();

    UIRenderer::WindowRenderTarget GetWindowRenderTarget(std::string_view window_name);

    BindlessArrayRef             bindless_array;
    RenderDevice&                device;
    UnorderedMap<Texture*, uint> registered_images;
    ImGuiIOInputSnapshot         input_snapshot;
};
} // namespace Moer::Render

#endif
