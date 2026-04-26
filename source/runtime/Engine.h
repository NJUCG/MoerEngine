#pragma once

#include "command/EngineCommandProcessor.h"
#include "renderer/Renderer.h"

namespace Moer {

class EditorUI;
class ConsoleSystem;
class RuntimeAssets;

class RENDER_API Engine {
public:
    Engine();
    virtual ~Engine();

    void Init(const SharedPtr<EditorConfig>& editor_config, bool fullscreen);
    void Run();
    void ShutDown();

private:
    SharedPtr<EditorConfig>  m_editor_config;
    UniquePtr<EditorUI>      m_editor_ui;
    SharedPtr<ConsoleSystem> m_console_system;
    UniquePtr<RuntimeAssets> m_runtime_assets;

    UniquePtr<Render::Renderer>    m_renderer;
    Command::EngineCommandProcessor m_command_processor;

    bool render_device_initialized = false;
    bool runtime_supported = false;
    bool has_shutdown = false;
};

} // namespace Moer