#pragma once

#include "RaytracingFramePacket.h"
#include "renderer/Renderer.h"

#include "renderer/common/RuntimeAssets.h"

#include <optional>

namespace Moer::Render::Raytracing {
struct FrameResources;
}

namespace Moer::Render::Raytracing {

class RENDER_API RaytracingRenderer : public Renderer {

public:
    RaytracingRenderer(
        uint2&                        _resolution,
        const SharedPtr<EditorConfig> _config,
        RuntimeAssets&                _runtime_assets
    );

    virtual ~RaytracingRenderer() override;

    virtual void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) override;

    bool SupportsSynchronizedRenderThread() const override {
        return true;
    }

    RaytracingFramePacket   PrepareFrame(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);
    RaytracingFrameFeedback RenderFrame(RaytracingFramePacket frame_packet);
    void ApplyFrameFeedback(RaytracingFrameFeedback feedback, RaytracingConfig& target_config);
    void Shutdown(const EngineHooks& hooks);

    bool RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);

private:
    struct RuntimeState;

    RuntimeAssets&          runtime_assets;
    UniquePtr<RuntimeState> runtime_state;

    RaytracingDebugFrameInput              debug_ui_frame_input{};
    ProfileData                            debug_ui_profiler_data{};
    Array<std::string>                     debug_ui_material_texture_names;
    uint64                                 next_frame_id                   = 0;
    bool                                   capture_scene_geometry_snapshot = true;
    bool                                   debug_ui_registered             = false;
    bool                                   export_request_in_flight        = false;

    void EnsureDebugUiRegistered(const EngineHooks& hooks);
    void RefreshSceneRuntimeRefs();
    void ExecuteSceneUpdates(SceneUpdateBatch& batch);
    void RecreateFrameResources(uint2 new_extent);

    void DumpTextureToFile(
        const ExportConfig&    _config,
        FrameResources&        _frame_rt,
        RenderDevice&          _device,
        CommandQueue&          _gfx_queue,
        std::filesystem::path& _exported_file_path,
        std::string_view       _suffix
    );
};

} // namespace Moer::Render::Raytracing
