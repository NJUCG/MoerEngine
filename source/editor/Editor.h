#pragma once

// 管理 Engine 外层的编辑器生命周期，并将编辑器 UI 回调接入运行时循环。

#include "misc/STL.h"

#include <functional>
#include <string_view>

namespace Moer {

class EditorUI;
class Engine;
class Scene;

namespace ProfileDump {
class ProfileDocumentLoader;
}

class Editor {
public:
    struct StartupHooks {
        bool                                                                 main_window_visible = true;
        std::function<void(std::string_view title, std::string_view detail)> on_progress;
        std::function<void()>                                                on_first_main_present;
    };

    struct ExtraHooks {
        std::function<void(Scene&)> on_tick_test;
    };

    Editor();
    virtual ~Editor();

    void Init(int argc, const char** argv);
    void Init(int argc, const char** argv, StartupHooks startup_hooks);
    void Run();
    void Run(const ExtraHooks& extra_hooks);
    // Run and ShutDown are Game Thread operations. Native file-dialog
    // initialization and teardown must remain on that owning thread.
    void ShutDown() noexcept;

    Engine&       GetEngine();
    const Engine& GetEngine() const;

private:
    void ReportStartupProgress(std::string_view title, std::string_view detail) const noexcept;
    void InitializeNativeFileDialog();
    void ShutDownNativeFileDialog() noexcept;

    UniquePtr<Engine>                             m_engine;
    UniquePtr<ProfileDump::ProfileDocumentLoader> m_profile_document_loader;
    UniquePtr<EditorUI>                           m_editor_ui;
    StartupHooks                                  m_startup_hooks;
    bool                                          m_nfd_initialized = false;
};

} // namespace Moer
