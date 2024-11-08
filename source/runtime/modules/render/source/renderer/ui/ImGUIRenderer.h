#ifndef MOER_ENGINE_IMGUI_RENDERER_H
#define MOER_ENGINE_IMGUI_RENDERER_H
#include "misc/STL.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#define ImTextureID uint64_t
struct UIFrameData;

class ImGUIRenderer : public UIRenderer {

public:
    ImGUIRenderer()          = default;
    virtual ~ImGUIRenderer() = default;
    virtual void Init() override {};
    virtual void ShutDown() override {};

    virtual void BeginRenderFrame() override {};
    virtual void EndRenderFrame() override {};

    virtual void RegisterImage(uint64_t _handle) override {};
    virtual void UnRegisterImage(uint64_t _handle) override {};

private:
    UIFrameData* frame_data;
    class Impl;
    Impl* impl;
};

namespace Moer::Render {
    class ImGUIRenderBackend {
    public:
        ImGUIRenderBackend(RenderDevice& _device);
        ~ImGUIRenderBackend();

        void RegisterImage(Texture* _texture, Sampler _sampler);
        void UnRegisterImage(Texture* _texture);

        void BeginGUIFrame();
        void EndGUIFrame();

        void RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer);

        void PresentWindows();

        TextureView GetWindowFrameBuffer(void* _window);

        BindlessArrayRef             bindless_array;
        RenderDevice&                device;
        UnorderedMap<Texture*, uint> registered_images;
    };
}// namespace Moer::Render

#endif