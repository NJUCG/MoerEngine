#pragma once

#include "remote/RemoteModuleController.h"
#include "renderer/Renderer.h"
#include "scripting/ScriptExecutionFuture.h"
#include "scripting/ScriptExecutionRequest.h"

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

    void Init3rdParty();
    void ShutDown3rdParty();

    void TickRendererSwitchValidation(Scene& scene);
    void TickRasterLifecycleValidation(Scene& scene);
    bool ConsumeRendererSwitchValidationReloadRequest();

    SharedPtr<EditorConfig>  m_editor_config;
    UniquePtr<RuntimeAssets> m_runtime_assets;

    UniquePtr<scripting::ScriptHost> m_script_host;
    UniquePtr<remote::RemoteModule>  m_remote_module;
    UniquePtr<Render::Renderer>      m_renderer;
    UniquePtr<RenderThreadService>   m_render_thread_service;

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
};

} // namespace Moer
