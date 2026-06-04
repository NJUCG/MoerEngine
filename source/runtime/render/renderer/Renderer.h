#pragma once

// Runtime
#include "renderer/EditorConfig.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"

#include "renderer/common/PresentationSurface.h"

namespace Moer {
class RuntimeAssets;
}

namespace Moer::Synapse {
class Context;
}

namespace Moer::Render {

struct EngineHooks {
    // Common
    std::function<void(Scene&)>                   on_tick_scripting;
    std::function<void(Scene&)>                   on_tick_test;
    std::function<void(Scene&)>                   on_tick_ui;
    std::function<void(CommandList&, TextureRef)> on_render_gui;
    std::function<void(void)>                     on_present_windows;
    std::function<bool(void)>                     on_is_need_reload;
    std::function<void(TextureView)>              on_publish_scene_output;

    std::function<void(std::string, std::string, std::function<void(Synapse::Context&)>)>
        on_register_renderer_config_section;
    std::function<void(std::string, std::string)> on_unregister_renderer_config_section;
};

class RENDER_API Renderer {

public:
    enum class EWindowState {
        Default = 0,
        Hiding,
        SizeChanged,
        Num,
    };

    Renderer(
        uint2&                 _resolution,
        const SharedPtr<EditorConfig> _config,
        const EngineHooks&     hooks,
        ::Moer::RuntimeAssets& _runtime_assets
    );

    virtual ~Renderer();

    void ReleaseResources();

    EWindowState TickWindowContext(const EngineHooks& hooks);
    void         LogSceneLoadStatus(const EditorConfig& config) const;
    void         PumpAsyncLoads();

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
    ::Moer::RuntimeAssets& runtime_assets;

    UniquePtr<PresentationSurface> presentation_surface;
    BindlessArrayRef               bindless_array;

    Scene               scene;
    CommandList         cmd_list;

    // Other vars
    FenceRef timeline;
    uint64   time;
    bool     first_load;
    bool     released = false;
    uint     max_frame_in_flight;
};

} // namespace Moer::Render
