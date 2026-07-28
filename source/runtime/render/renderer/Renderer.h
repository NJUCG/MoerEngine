#pragma once

// 定义渲染器公共的帧编排、窗口状态与编辑器 Hook 契约。

// Runtime
#include "renderer/EditorConfig.h"
#include "renderer/FramePrepareProfile.h"
#include "rhi/RHI.h"
#include "scene/RenderScene.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"

#include "common/UIRenderer.h"
#include "common/UiCombinePass.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string_view>

namespace Moer::Render {

class RenderProfileCapture;

struct EngineHooks {
    // Common
    std::function<void(Scene&)> on_tick_scripting;
    std::function<void(Scene&)> on_tick_test;
    std::function<void(Scene&)> on_tick_ui;
    std::function<bool()>       should_reload;

    std::function<UiCompositionFrameData()> on_capture_ui_composition;
    std::function<UiDrawFramePacket()>      on_capture_ui_draw_frame;

    std::function<void(std::string, std::function<void()>)> on_register_ui_func;

    std::function<void(std::string)> on_unregister_ui_func;

    std::function<void()> on_show_config_sub_ui;

    // Reports coarse startup stages without coupling Runtime to an editor UI.
    std::function<void(std::string_view title, std::string_view detail)> on_startup_progress;

    // Invoked on the Game Thread after the first submitted main-swapchain Present.
    // Engine gates this hook for its full lifetime, including renderer reloads.
    std::function<void()> on_first_main_present;

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

    Renderer(
        uint2                         initial_resolution,
        const SharedPtr<EditorConfig> config,
        RenderProfileCapture*         render_profile_capture = nullptr
    );

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
    PresentReceiptRef  CreateMainPresentReceipt(bool scene_content_ready);
    void ApplyMainPresentReceipt(const PresentReceiptRef& receipt, const EngineHooks& hooks);
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
    // Engine owns the capture bridge and outlives every renderer instance.
    RenderProfileCapture* render_profile_capture = nullptr;

    SwapchainRef     swapchain;
    BindlessArrayRef bindless_array;

    SwapchainCreateInfo    swapchain_create_info;
    Scene                  scene;
    UniquePtr<RenderScene> render_scene;
    CommandList            cmd_list;

    UniquePtr<UiCombinePass> ui_combine_pass;

    // 由具体渲染器共享的帧同步状态。
    FenceRef timeline;
    uint64   time;
    bool     first_load;
    bool     resources_released = false;
    mutable std::atomic<bool> first_main_present_confirmed{false};
    std::atomic<bool> main_swapchain_recreate_requested{false};
    std::optional<std::chrono::steady_clock::time_point> first_present_candidate_started_at;
    bool first_present_receipt_logged = false;
    uint     max_frame_in_flight;

    UniquePtr<FramePrepareProfileState> frame_prepare_profile_state;
};

} // namespace Moer::Render
