#pragma once

#include "RaytracingFramePacket.h"
#include "renderer/Renderer.h"

#include "renderer/common/RuntimeAssets.h"
#include "scene/GpuScene.h"

#include <functional>
#include <optional>

namespace Moer::Render::Raytracing {
class ExportSubmissionTransaction;
struct FrameResources;
}

namespace Moer::Render::Raytracing {

class RENDER_API RaytracingRenderer : public Renderer {

public:
    RaytracingRenderer(
        uint2&                        _resolution,
        const SharedPtr<EditorConfig> _config,
        RuntimeAssets&                _runtime_assets,
        RenderProfileCapture*         _render_profile_capture
    );

    virtual ~RaytracingRenderer() override;

    virtual void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) override;

    bool SupportsSynchronizedRenderThread() const override {
        return true;
    }

    RaytracingFramePacket   PrepareFrame(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);
    RaytracingFrameFeedback RenderFrame(RaytracingFramePacket frame_packet);
    // Retained for source/ABI compatibility with tools that apply feedback
    // without editor lifecycle hooks.
    void ApplyFrameFeedback(RaytracingFrameFeedback feedback, RaytracingConfig& target_config);
    void ApplyFrameFeedback(
        RaytracingFrameFeedback feedback,
        RaytracingConfig&       target_config,
        const EngineHooks&      hooks
    );
    void Shutdown(const EngineHooks& hooks);

    bool RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);

private:
    struct RuntimeState;
    struct SceneUpdateResult {
        bool gpu_resources_updated = false;
        bool tlas_built             = false;
        bool rt_scene_replaced      = false;
    };

    RuntimeAssets&           runtime_assets;
    SceneRenderExtentTracker scene_render_extent_tracker;
    UniquePtr<RuntimeState>  runtime_state;

    RaytracingDebugFrameInput              debug_ui_frame_input{};
    ProfileData                            debug_ui_profiler_data{};
    Array<std::string>                     debug_ui_material_texture_names;
    uint64                                 next_frame_id                    = 0;
    uint64                                 render_extent_generation         = 0;
    bool                                   capture_scene_geometry_snapshot = true;
    bool                                   debug_ui_registered             = false;
    bool                                   export_request_in_flight        = false;

    void EnsureDebugUiRegistered(const EngineHooks& hooks);
    bool RefreshSceneRuntimeRefs();
    SceneUpdateResult ExecuteSceneUpdates(
        SceneUpdateBatch&                                batch,
        const GpuScene::PendingCommandListSetupCallback& setup_command_lists
    );
    bool RecreateSceneResources(uint2 new_extent);
    void RecreateOutputResources(uint2 new_extent);

    [[nodiscard]] bool DumpTextureToFile(
        const ExportConfig&                       _config,
        FrameResources&                           _frame_rt,
        RenderDevice&                             _device,
        CommandQueue&                             _gfx_queue,
        std::filesystem::path&                    _exported_file_path,
        std::string_view                          _suffix,
        ExportSubmissionTransaction&              _export_submission,
        const std::function<bool(CommandList&)>&  _setup_command_list
    );
};

} // namespace Moer::Render::Raytracing
