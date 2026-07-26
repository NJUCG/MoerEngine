// 实现 Moer UI 数据包与 Dear ImGui 绘制数据之间的转换和渲染。

#ifndef MOER_ENGINE_DEAR_IMGUI_RENDERER_H
#define MOER_ENGINE_DEAR_IMGUI_RENDERER_H

#include "misc/STL.h"
#include "renderer/common/UIRenderer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"

#define ImTextureID uint64_t

namespace Moer::Render {
class ImGuiRenderBackend : public UiDrawFrameBackend {
public:
    explicit ImGuiRenderBackend(RenderDevice& _device);
    ~ImGuiRenderBackend() override;

    void RegisterImage(Texture* _texture, Sampler _sampler);
    void UnregisterImage(Texture* _texture);

    void BeginGUIFrame();
    void EndGUIFrame();
    void UpdatePlatformWindows();

    UiDrawFramePacket CaptureDrawFrame();

    void RenderGUI(
        CommandList&           _cmd_list,
        const TextureView&     _main_framebuffer,
        const UiDrawFramePacket& _frame,
        EUiDrawExecutionThread _execution_thread
    ) override;
    void PresentWindows(const UiDrawFramePacket& _frame, EUiDrawExecutionThread _execution_thread) override;

    TextureRef GetWindowFrameBuffer(void* _window);

    BindlessArrayRef             bindless_array;
    RenderDevice&                device;
    UnorderedMap<Texture*, uint> registered_images;
    void*                        backend_data = nullptr;
};
} // namespace Moer::Render

#endif // MOER_ENGINE_DEAR_IMGUI_RENDERER_H
