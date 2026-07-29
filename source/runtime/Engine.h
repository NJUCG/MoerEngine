#pragma once

#include "ProfileCaptureController.h"
#include "remote/RemoteModuleController.h"
#include "renderer/Renderer.h"
#include "scripting/ScriptExecutionFuture.h"
#include "scripting/ScriptExecutionRequest.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace Moer::scripting {
class ScriptHost;
}

namespace Moer::remote {
class RemoteModule;
}

namespace Moer {

class EditorUI;
class RuntimeAssets;
class RenderThreadService;

class RENDER_API Engine {
public:
    using StartupProgressCallback =
        std::function<void(std::string_view title, std::string_view detail)>;

    Engine();
    virtual ~Engine();

    static void ValidateCommandLine(int argc, const char** argv);

    void Init(
        int                            argc,
        const char**                   argv,
        bool                           main_window_visible = true,
        const StartupProgressCallback& on_startup_progress = {}
    );
    void Run(const Render::EngineHooks& hooks);
    void RequestExit();

    // 提供给 Editor 等外部系统的 Remote 控制句柄
    remote::RemoteModuleController GetRemoteModuleController() const;

    scripting::ScriptExecutionFuture SubmitScriptExecution(scripting::ScriptExecutionRequest request);
    [[nodiscard]] Render::ProfileCaptureRequestSubmission
    RequestProfileCaptureStart(Render::ProfileCaptureStartOptions options) noexcept;
    [[nodiscard]] Render::ProfileCaptureRequestSubmission
    RequestProfileCaptureStop(std::uint64_t expected_generation) noexcept;
    [[nodiscard]] Render::ProfileCaptureControllerSnapshot
    GetProfileCaptureSnapshot() const noexcept;
    void                             ShutDown() noexcept;

    uint2& GetResolution() {
        return m_editor_config->GetResolution();
    }

    SharedPtr<EditorConfig> GetEditorConfig() {
        return m_editor_config;
    }

private:
    enum class ERendererSwitchValidationStage : uint8 {
        Disabled,
        InitialRaster,
        ReloadedRaster,
        Raytracing,
        FinalRaster,
        Complete,
        Failed,
    };

    enum class EProfileCaptureLifecycleValidationStage : uint8 {
        Disabled,
        WaitingForFirstGenerationFrames,
        WaitingForFirstStop,
        WaitingForRestart,
        WaitingForStaleStop,
        WaitingForSecondGenerationFrames,
        WaitingForSecondStop,
        Complete,
        Failed,
    };

    void InitializeProfileDump() noexcept;
    void InitializeProfileCaptureLifecycleValidation() noexcept;
    void TickProfileCaptureLifecycleValidation(
        Render::ProfileCaptureTickResult tick_result
    ) noexcept;
    void FailProfileCaptureLifecycleValidation(std::string_view reason) noexcept;

    void TickRendererSwitchValidation(Scene& scene);
    void TickRasterLifecycleValidation(Scene& scene);
    bool ConsumeRendererSwitchValidationReloadRequest();

    SharedPtr<EditorConfig>  m_editor_config;
    UniquePtr<RuntimeAssets> m_runtime_assets;

    UniquePtr<scripting::ScriptHost> m_script_host;
    UniquePtr<remote::RemoteModule>  m_remote_module;
    UniquePtr<Render::Renderer>      m_renderer;
    UniquePtr<RenderThreadService>   m_render_thread_service;
    UniquePtr<Render::RenderProfileCapture> m_render_profile_capture;
    UniquePtr<Render::ProfileCaptureController> m_profile_capture_controller;

    uint m_max_frame_lag = 0;
    bool m_has_shutdown = false;
    bool m_first_main_present_notified = false;
    bool m_task_system_initialized     = false;
    bool m_render_device_initialized   = false;
    bool m_shader_manager_initialized  = false;
    bool m_window_context_initialized  = false;

    // Validation state is accessed only by Game Thread hooks. Render work is drained before reload.
    ERendererSwitchValidationStage m_renderer_switch_validation_stage =
        ERendererSwitchValidationStage::Disabled;
    uint m_renderer_switch_validation_ready_frames = 0;
    bool m_renderer_switch_validation_reload_requested = false;
    bool m_raster_lifecycle_validation_enabled = false;
    uint m_raster_lifecycle_validation_ready_frames = 0;

    bool m_profile_capture_lifecycle_validation_enabled = false;
    bool m_profile_capture_validation_exit_requested    = false;
    EProfileCaptureLifecycleValidationStage
        m_profile_capture_lifecycle_validation_stage =
            EProfileCaptureLifecycleValidationStage::Disabled;
    Render::ProfileCaptureRequestTicket m_profile_capture_validation_ticket;
    Render::ProfileCaptureStartOptions  m_profile_capture_validation_restart_options;
    std::uint64_t m_profile_capture_validation_first_generation  = 0;
    std::uint64_t m_profile_capture_validation_second_generation = 0;
    std::uint64_t m_profile_capture_validation_gt_ticks          = 0;
    std::chrono::steady_clock::time_point
        m_profile_capture_validation_deadline{};
};

} // namespace Moer
