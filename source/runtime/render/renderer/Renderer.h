#pragma once

// Runtime
#include "renderer/EditorConfig.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"

#include "common/UiCombinePass.h"

namespace Moer::Render {

struct EngineHooks {
    // Common
    std::function<void(Scene&)>                   on_tick_scripting;
    std::function<void(Scene&)>                   on_tick_test;
    std::function<void(Scene&)>                   on_tick_ui;
    std::function<void(CommandList&, TextureRef)> on_render_gui;
    std::function<void(void)>                     on_present_windows;
    std::function<bool(void)>                     on_is_need_reload;

    std::function<TextureRef(UiCombinePass*, CommandList&, TextureView, TextureView, TextureView)>
        on_ui_combine_pass;

    std::function<void(std::string, std::function<void(void)>)> on_register_ui_func;

    std::function<void(std::string)> on_unregister_ui_func;

    std::function<void(void)> on_show_config_sub_ui;

    // Raster
    std::function<void(const Array<TextureView>&)> on_raster_register_frame_buffers;
};

class RENDER_API Renderer {

public:
    enum class EWindowState {
        Default = 0,
        Hiding,
        SizeChanged,
        Num,
    };

    Renderer(uint2& _resolution, const SharedPtr<EditorConfig> _config, const EngineHooks& hooks);

    virtual ~Renderer();

    void ReleaseResources();

    EWindowState TickWindowContext(const EngineHooks& hooks);
    void         LogSceneLoadStatus(const EditorConfig& config) const;

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    virtual void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) = 0;

    CommandList& GetCommandList() {
        return cmd_list;
    }

protected:
    RenderDevice&  device;
    ShaderManager& manager;
    CommandQueue&  gfx_queue;
    uint2&         resolution; // 数据源位于EditorConfig中

    SwapchainRef     swapchain;
    BindlessArrayRef bindless_array;

    SwapchainCreateInfo swapchain_createinfo;
    Scene               scene;
    CommandList         cmd_list;

    UniquePtr<UiCombinePass> ui_combine_pass;

    // Other vars
    FenceRef timeline;
    uint64   time;
    bool     first_load;
    bool     released = false;
    uint     max_frame_in_flight;
};

} // namespace Moer::Render
