#ifndef MOER_ENGINE_IMGUI_RENDERER_H
#define MOER_ENGINE_IMGUI_RENDERER_H
#include "misc/STL.h"
#include "renderer/common/UIRenderer.h"
#include "renderer/common/ui/ImGuiIOInput.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include <mutex>
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

    UIRenderer::RenderOutputSlotHandle RegisterRenderOutputSlot(uint32_t imgui_id);
    uint64_t GetRenderOutputTextureId(UIRenderer::RenderOutputSlotHandle handle) const;
    void PublishRenderOutput(UIRenderer::RenderOutputSlotHandle handle, TextureView resource);

    void DrainRenderOutputUpdates();
    uint ResolveTextureHandle(uint64_t texture_id);

    BindlessArrayRef             bindless_array;
    RenderDevice&                device;
    UnorderedMap<Texture*, uint> registered_images;
    ImGuiIOInputSnapshot         input_snapshot;

    struct RenderOutputResource {
        uint32_t    generation = 0;
        TextureView resource;
    };

    struct PendingRenderOutputUpdate {
        UIRenderer::RenderOutputSlotHandle handle;
        TextureView                        resource;
    };

    std::mutex render_output_mutex;
    UnorderedMap<uint32_t, UIRenderer::RenderOutputSlotHandle> render_output_slots_by_id;
    Array<uint32_t> render_output_generations;
    Array<PendingRenderOutputUpdate> pending_render_output_updates;
    Array<RenderOutputResource> render_output_snapshot;

    TextureRef transparent_texture;
    uint       transparent_texture_handle = 0;
};
} // namespace Moer::Render

#endif
