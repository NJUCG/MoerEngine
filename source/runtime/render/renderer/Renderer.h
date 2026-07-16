#pragma once

// Defines renderer-wide frame orchestration, window state, and editor hook contracts.

// Runtime
#include "renderer/EditorConfig.h"
#include "renderer/FramePrepareProfile.h"
#include "rhi/RHI.h"
#include "scene/RenderScene.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"

#include "common/UIRenderer.h"
#include "common/UiCombinePass.h"

#include <string_view>

namespace Moer::Render {

struct UiCompositionFrameData {
    bool       enabled         = false;
    bool       separate_window = false;
    uint2      output_resolution{};
    float2     scene_color_position{};
    float2     scene_color_resolution{};
    TextureRef window_frame_buffer{};
};

struct EngineHooks {
    // Common
    std::function<void(Scene&)> on_tick_scripting;
    std::function<void(Scene&)> on_tick_test;
    std::function<void(Scene&)> on_tick_ui;
    std::function<bool()>       on_is_need_reload;

    std::function<TextureRef(UiCombinePass*, CommandList&, TextureView, TextureView, TextureView)>
        on_ui_combine_pass;

    std::function<UiCompositionFrameData()> on_capture_ui_composition;
    std::function<UiDrawFramePacket()>      on_capture_ui_draw_frame;

    std::function<void(std::string, std::function<void()>)> on_register_ui_func;

    std::function<void(std::string)> on_unregister_ui_func;

    std::function<void()> on_show_config_sub_ui;

    // Raster
    std::function<void(const Array<std::string>&)> on_raster_register_frame_buffer_names;
};

class RENDER_API Renderer {

public:
    enum class EWindowState {
        Default = 0,
        Hiding,
        SizeChanged,
        Num,
    };

    struct WindowFrameState {
        EWindowState state      = EWindowState::Default;
        uint2        resolution = uint2(0u, 0u);
    };

    Renderer(uint2 initial_resolution, const SharedPtr<EditorConfig> config);

    virtual ~Renderer();

    void ReleaseResources();

    WindowFrameState TickWindowContext(uint2 current_resolution);
    void             LogSceneLoadStatus(const EditorConfig& config) const;

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    virtual void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) = 0;

    virtual bool SupportsSynchronizedRenderThread() const {
        return false;
    }

    CommandList& GetCommandList() {
        return cmd_list;
    }

protected:
    void               PrepareRenderFrame(const WindowFrameState& window_frame);
    [[nodiscard]] bool IsFramePrepareProfilingEnabled() const {
        return frame_prepare_profile_state != nullptr;
    }
    [[nodiscard]] FramePrepareProfileClock::time_point BeginFramePrepareProfile() const;
    void CaptureFramePrepareUiWorkload(FramePrepareWorkload& workload, const UiDrawFramePacket& ui_draw_frame)
        const;
    void RecordFramePrepareProfile(
        std::string_view                     renderer_name,
        FramePrepareProfileClock::time_point started_at,
        const FramePrepareProfile&           profile,
        const FramePrepareWorkload&          workload
    );

    RenderDevice&  device;
    ShaderManager& manager;
    CommandQueue&  gfx_queue;
    uint2          resolution;

    SwapchainRef     swapchain;
    BindlessArrayRef bindless_array;

    SwapchainCreateInfo    swapchain_create_info;
    Scene                  scene;
    UniquePtr<RenderScene> render_scene;
    CommandList            cmd_list;

    UniquePtr<UiCombinePass> ui_combine_pass;

    // Frame synchronization state shared by concrete renderers.
    FenceRef timeline;
    uint64   time;
    bool     first_load;
    bool     resources_released = false;
    uint     max_frame_in_flight;

    UniquePtr<FramePrepareProfileState> frame_prepare_profile_state;
};

} // namespace Moer::Render
