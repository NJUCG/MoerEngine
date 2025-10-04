#pragma once

#include "renderer/Renderer.h"

namespace Moer {

class EditorUI;
class RuntimeAssets;

class RENDER_API Engine {
public:
    Engine();
    virtual ~Engine();

    void Init(int argc, const char** argv);
    void Run(const Render::EngineHooks& hooks);
    void ShutDown();

    SharedPtr<uint2> GetResolution() {
        return m_editor_config->resolution;
    }

    SharedPtr<EditorConfig> GetEditorConfig() {
        return m_editor_config;
    }

private:
    void Init3rdParty();
    void ShutDown3rdParty();

    SharedPtr<EditorConfig>  m_editor_config;
    UniquePtr<RuntimeAssets> m_runtime_assets;

    UniquePtr<Render::Renderer> m_renderer;
};

} // namespace Moer