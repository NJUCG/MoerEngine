#include "Editor.h"

#include "Core.h"
#include "Engine.h"
#include "profile_consumer/ProfileDocument.h"

#include "EditorUI.h"
#include "window/WindowContext.h"

#include <nfd.hpp>

#include <cassert>
#include <stdexcept>
#include <string>

using namespace Moer::Render;

namespace Moer {

Editor::Editor() = default;

Editor::~Editor() {
    ShutDown();
}

void Editor::Init(int argc, const char** argv) {
    Init(argc, argv, StartupHooks{});
}

void Editor::Init(int argc, const char** argv, StartupHooks startup_hooks) {
    m_startup_hooks = std::move(startup_hooks);
    ReportStartupProgress("Starting MoerEditor", "Initializing the engine core");
    m_engine = MakeUnique<Engine>();
    try {
        m_engine->Init(argc, argv, m_startup_hooks.main_window_visible, m_startup_hooks.on_progress);
    } catch (...) {
        m_engine->ShutDown();
        m_engine.reset();
        throw;
    }
}

void Editor::Run() {
    Run(ExtraHooks{});
}

void Editor::Run(const ExtraHooks& extra_hooks) {
    try {
        if (!m_engine) {
            throw std::logic_error("Editor::Run requires a successfully initialized Engine.");
        }
        if (!IsCurrentlyGameThread()) {
            throw std::runtime_error(
                "Editor::Run and native file dialog initialization must run on the Game Thread."
            );
        }

        InitializeNativeFileDialog();
        if (!m_profile_document_loader) {
            m_profile_document_loader = MakeUnique<ProfileDump::ProfileDocumentLoader>();
        }

        // UI 的生命周期限定在 Engine::Run 内，因为其渲染器资源依赖当前运行的 Engine。
        ReportStartupProgress("Preparing editor interface", "Building ImGui fonts and GPU resources");
        m_editor_ui = MakeUnique<EditorUI>(
            MakeUnique<Render::UIRenderer>(RenderDevice::Get()),
            m_engine->GetEditorConfig(),
            m_engine->GetRemoteModuleController(),
            *m_engine,
            *m_profile_document_loader
        );

        m_engine->Run(EngineHooks{
            // Common
            .on_tick_test = extra_hooks.on_tick_test,
            .on_tick_ui =
                [this](Scene& scene) {
                    m_editor_ui->TickUI(scene);
                },
            .on_capture_window_input =
                [this]() {
                    return m_editor_ui->GetWindowInputSnapshot();
                },
            .should_reload =
                [this]() {
                    return m_editor_ui->NeedsReload();
                },
            .on_capture_ui_composition =
                [this]() {
                    return UiCompositionFrameData{
                        .enabled                = true,
                        .separate_window        = m_editor_ui->IsSeparateWindow(),
                        .output_resolution      = m_editor_ui->GetConfig()->GetResolution(),
                        .scene_color_position   = m_editor_ui->GetSceneColorPos(),
                        .scene_color_resolution = m_editor_ui->GetSceneColorResolution(),
                        .window_frame_buffer    = m_editor_ui->GetWindowFrameBuffer()
                    };
                },
            .on_capture_ui_draw_frame =
                [this]() {
                    return m_editor_ui->CaptureDrawFrame();
                },
            .on_register_ui_func =
                [this](std::string name, std::function<void()> callback) {
                    m_editor_ui->RegisterUIFunc(std::move(name), std::move(callback));
                },
            .on_unregister_ui_func =
                [this](std::string name) {
                    m_editor_ui->UnregisterUIFunc(std::move(name));
                },
            .on_show_config_sub_ui =
                [this]() {
                    m_editor_ui->SetShowRenderConfigSubUI(true);
                },
            .on_startup_progress =
                [this](std::string_view title, std::string_view detail) {
                    ReportStartupProgress(title, detail);
                },
            .on_first_main_present =
                [this]() {
                    ReportStartupProgress("Opening MoerEditor", "The first rendered frame is ready");
                    if (!m_startup_hooks.main_window_visible) {
                        WindowContext::ShowMainWindow();
                        m_startup_hooks.main_window_visible = true;
                    }
                    if (m_startup_hooks.on_first_main_present) {
                        try {
                            m_startup_hooks.on_first_main_present();
                        } catch (...) {
                            // Startup presentation is auxiliary. A client hook
                            // must not interrupt the renderer after Present.
                        }
                    }
                },

            // Raster
            .on_raster_register_frame_buffer_names =
                [this](const Array<std::string>& names) {
                    m_editor_ui->m_raster_ui.RegisterFrameBufferNames(names);
                }
        });

        m_editor_ui.reset();
    } catch (...) {
        // Keep failure rollback identical to explicit shutdown: UI resources
        // cannot outlive the loader, and NFD must leave the Game Thread before
        // the Engine tears down its window and worker services.
        ShutDown();
        throw;
    }
}

void Editor::ReportStartupProgress(std::string_view title, std::string_view detail) const noexcept {
    try {
        if (m_startup_hooks.on_progress) {
            m_startup_hooks.on_progress(title, detail);
        }
    } catch (...) {
        // Optional startup UI must not participate in Editor failure paths.
    }
}

void Editor::ShutDown() noexcept {
    m_editor_ui.reset();
    if (m_profile_document_loader) {
        m_profile_document_loader->Shutdown();
        m_profile_document_loader.reset();
    }
    ShutDownNativeFileDialog();
    if (m_engine) {
        m_engine->ShutDown();
        m_engine.reset();
    }
}

void Editor::InitializeNativeFileDialog() {
    if (m_nfd_initialized) {
        return;
    }

    assert(IsCurrentlyGameThread());
    const nfdresult_t result = NFD::Init();
    if (result == NFD_OKAY) {
        m_nfd_initialized = true;
        return;
    }

    const char* error = NFD_GetError();
    throw std::runtime_error(
        std::string("Failed to initialize the native file dialog on the Game Thread: ") +
        (error != nullptr && error[0] != '\0' ? error : "unknown NFD initialization error")
    );
}

void Editor::ShutDownNativeFileDialog() noexcept {
    if (!m_nfd_initialized) {
        return;
    }

    assert(IsCurrentlyGameThread());
    NFD::Quit();
    m_nfd_initialized = false;
}

Engine& Editor::GetEngine() {
    return *m_engine;
}

const Engine& Editor::GetEngine() const {
    return *m_engine;
}

} // namespace Moer
