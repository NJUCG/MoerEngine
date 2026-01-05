#pragma once

#include "renderer/Renderer.h"

namespace Moer::Render::Raytracing {

class RENDER_API RaytracingRenderer : public Renderer {

public:
    RaytracingRenderer(
        uint2&                                                    _resolution,
        const SharedPtr<EditorConfig>                             _config,
        const EngineHooks&                                        _hooks,
        std::function<void(const std::filesystem::path&, Scene*)> _load_scene_async,
        RuntimeAssets&                                            _runtime_assets
    );

    virtual void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) override;

private:
    RuntimeAssets& runtime_assets;

    void DumpTextureToFile(
        ExportConfig&          _config,
        FrameResources&        _frame_rt,
        RenderDevice&          _device,
        CommandQueue&          _gfx_queue,
        std::filesystem::path& _exported_file_path,
        std::string_view       _suffix
    );
};

} // namespace Moer::Render::Raytracing