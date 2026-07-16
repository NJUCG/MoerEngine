#ifndef MOER_ENGINE_UI_RENDERER_H
#define MOER_ENGINE_UI_RENDERER_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include <cstdint>
#include <type_traits>

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
class UiDrawFrameBackend;

class RENDER_API UiViewportRenderResources {
public:
    virtual ~UiViewportRenderResources() = default;
};

enum class EUiDrawExecutionThread : uint8_t {
    Game,
    Render
};

struct UiDrawVertex {
    float2 position{};
    float2 uv{};
    uint32 color = 0;
};

using UiDrawIndex = uint16;

struct UiDrawCommand {
    float2 clip_min{};
    float2 clip_max{};
    uint32 texture_handle = 0;
    uint32 element_count  = 0;
    uint32 vertex_offset  = 0;
    uint32 index_offset   = 0;
};

struct UiViewportDrawPacket {
    float2 display_position{};
    float2 display_size{};
    float2 framebuffer_scale{1.f, 1.f};

    Array<UiDrawVertex>  vertices;
    Array<UiDrawIndex>   indices;
    Array<UiDrawCommand> commands;

    SharedPtr<UiViewportRenderResources> render_resources;
    TextureRef                           framebuffer;
    SwapchainRef                         swapchain;
};

struct UiDrawFramePacket {
    UiDrawFramePacket()                                        = default;
    UiDrawFramePacket(UiDrawFramePacket&&) noexcept            = default;
    UiDrawFramePacket& operator=(UiDrawFramePacket&&) noexcept = default;
    UiDrawFramePacket(const UiDrawFramePacket&)                = delete;
    UiDrawFramePacket& operator=(const UiDrawFramePacket&)     = delete;

    SharedPtr<UiDrawFrameBackend> backend;
    UiViewportDrawPacket          main_viewport;
    Array<UiViewportDrawPacket>   platform_viewports;
};

static_assert(std::is_move_constructible_v<UiDrawFramePacket>);
static_assert(!std::is_copy_constructible_v<UiDrawFramePacket>);

class RENDER_API UiDrawFrameBackend {
public:
    virtual ~UiDrawFrameBackend() = default;

    virtual void RenderGUI(
        CommandList&           _cmd_list,
        const TextureView&     _main_framebuffer,
        UiDrawFramePacket&     _frame,
        EUiDrawExecutionThread _execution_thread
    ) = 0;
    virtual void
    PresentWindows(const UiDrawFramePacket& _frame, EUiDrawExecutionThread _execution_thread) = 0;
};

RENDER_API void RenderUiDrawFrame(
    CommandList&           _cmd_list,
    const TextureView&     _main_framebuffer,
    UiDrawFramePacket&     _frame,
    EUiDrawExecutionThread _execution_thread
);
RENDER_API void PresentUiDrawFrame(const UiDrawFramePacket& _frame, EUiDrawExecutionThread _execution_thread);

class UIRenderer {
public:
    struct Impl;
    RENDER_API UIRenderer(RenderDevice& _device);

    RENDER_API virtual ~UIRenderer();

    RENDER_API void BeginGUIFrame();

    RENDER_API void EndGUIFrame();

    RENDER_API void UpdatePlatformWindows();

    RENDER_API UiDrawFramePacket CaptureDrawFrame();
    RENDER_API void              RegisterImage(Texture* _texture, Sampler _sampler);
    RENDER_API void              UnRegisterImage(Texture* _texture);

    RENDER_API TextureRef GetWindowFrameBuffer(void* _window);

private:
    UniquePtr<Impl> impl;
    // RENDER_API virtual void UploadFonts(FontDesc _font_desc) = 0;
};
}; // namespace Moer::Render

#endif //MOER_ENGINE_UI_RENDERER_H
