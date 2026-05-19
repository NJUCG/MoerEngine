#pragma once

#include "renderer/Renderer.h"
#include "scripting/ScriptExecutionFuture.h"
#include "scripting/ScriptExecutionRequest.h"

namespace Moer::scripting {
class ScriptHost;
}

namespace Moer::remote {
class RemoteModule;
}

namespace Moer {

class EditorUI;
class RuntimeAssets;

class RENDER_API Engine {
public:
    Engine();
    virtual ~Engine();

    void Init(int argc, const char** argv);
    void Run(const Render::EngineHooks& hooks);
    void RequestExit();
    bool SetRemoteEnabled(bool enabled);
    bool IsRemoteEnabled() const;
    bool IsRemoteRunning() const;
    scripting::ScriptExecutionFuture SubmitScriptExecution(
        scripting::ScriptExecutionRequest request
    );
    void ShutDown();

    uint2& GetResolution() {
        return m_editor_config->GetResolution();
    }

    SharedPtr<EditorConfig> GetEditorConfig() {
        return m_editor_config;
    }

private:
    void Init3rdParty();
    void ShutDown3rdParty();

    SharedPtr<EditorConfig>  m_editor_config;
    UniquePtr<RuntimeAssets> m_runtime_assets;

    UniquePtr<scripting::ScriptHost> m_script_host;
    UniquePtr<remote::RemoteModule>  m_remote_module;
    UniquePtr<Render::Renderer>      m_renderer;

    bool m_has_shutdown = false;
};

} // namespace Moer