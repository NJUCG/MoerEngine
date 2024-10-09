#include "renderer/UIRenderer.h"
#include "ImGUIRenderer.h"
#include "rhi/RHI.h"

namespace Moer::Render {
    struct UIRenderer::Impl {
        Impl(RenderDevice& _device) : backend(_device){};
        ~Impl() {
        }
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

        void RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer) {
            backend.RenderGUI(_cmd_list, _framebuffer);
        }
        ImGUIRenderBackend backend;
    };

    UIRenderer::UIRenderer(RenderDevice& _device) : impl(MakeUnique<Impl>(_device)) {
    }

    UIRenderer::~UIRenderer() {
    }

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

    void UIRenderer::RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer) {
        impl->RenderGUI(_cmd_list, _framebuffer);
    }

}// namespace Moer::Render