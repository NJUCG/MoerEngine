#include "renderer/common/UIRenderer.h"
#include "ImGUIRenderer.h"
#include "rhi/RHI.h"

namespace Moer::Render {
struct UIRenderer::Impl {
    Impl(RenderDevice& _device) : backend(_device) {};
    ~Impl() {}
    void RegisterImage(Texture* _texture, Sampler _sampler) {
        backend.RegisterImage(_texture, _sampler);
    }
    void UnRegisterImage(Texture* _texture) {
        backend.UnRegisterImage(_texture);
    }

    void BeginGUIFrame() {
        backend.BeginGUIFrame();
    }
    void EndGUIFrame() {
        backend.EndGUIFrame();
    }
    const ImGuiIOInputSnapshot& GetInputSnapshot() const {
        return backend.GetInputSnapshot();
    }

    void RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer) {
        backend.RenderGUI(_cmd_list, _framebuffer);
    }
    UIRenderer::RenderOutputSlotHandle RegisterRenderOutputSlot(uint32_t imgui_id) {
        return backend.RegisterRenderOutputSlot(imgui_id);
    }
    uint64_t GetRenderOutputTextureId(UIRenderer::RenderOutputSlotHandle handle) const {
        return backend.GetRenderOutputTextureId(handle);
    }
    void PublishRenderOutput(UIRenderer::RenderOutputSlotHandle handle, TextureView resource) {
        backend.PublishRenderOutput(handle, resource);
    }
    ImGUIRenderBackend backend;
};

UIRenderer::UIRenderer(RenderDevice& _device) : impl(MakeUnique<Impl>(_device)) {}

UIRenderer::~UIRenderer() {}

void UIRenderer::RegisterImage(Texture* _texture, Sampler _sampler) {
    impl->RegisterImage(_texture, _sampler);
}

void UIRenderer::UnRegisterImage(Texture* _texture) {
    impl->UnRegisterImage(_texture);
}

void UIRenderer::BeginGUIFrame() {
    impl->BeginGUIFrame();
}

void UIRenderer::EndGUIFrame() {
    impl->EndGUIFrame();
}

const ImGuiIOInputSnapshot& UIRenderer::GetInputSnapshot() const {
    return impl->GetInputSnapshot();
}

void UIRenderer::RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer) {
    impl->RenderGUI(_cmd_list, _framebuffer);
}

UIRenderer::RenderOutputSlotHandle UIRenderer::RegisterRenderOutputSlot(uint32_t imgui_id) {
    return impl->RegisterRenderOutputSlot(imgui_id);
}

uint64_t UIRenderer::GetRenderOutputTextureId(RenderOutputSlotHandle handle) const {
    return impl->GetRenderOutputTextureId(handle);
}

void UIRenderer::PublishRenderOutput(RenderOutputSlotHandle handle, TextureView resource) {
    impl->PublishRenderOutput(handle, resource);
}

void UIRenderer::PresentWindows() {
    impl->backend.PresentWindows();
}

} // namespace Moer::Render
