#include "Editor.h"

#include "Engine.h"
#include "config/ConfigManager.h"

#include "EditorUI.h"
#include "console/ConsoleSystem.h"
#include "trace/Trace.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <nfd.hpp>
#include <string>

// namespace
using namespace Moer::Render;

namespace Moer {
namespace {

std::string BuildUniqueTraceCsvPath() {
    const std::filesystem::path trace_dir = ConfigManager::GetInstance().GetWorkspacePath() / "trace";
    std::filesystem::create_directories(trace_dir);

    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto tt  = clock::to_time_t(now);
    std::tm    tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    const int millis = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000
    );

    char base_name[128]{};
    std::snprintf(
        base_name,
        sizeof(base_name),
        "editor_trace_%04d%02d%02d_%02d%02d%02d_%03d",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        millis
    );

    std::filesystem::path candidate = trace_dir / (std::string(base_name) + ".csv");
    for (uint32_t suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        char unique_name[160]{};
        std::snprintf(unique_name, sizeof(unique_name), "%s_%u", base_name, suffix);
        candidate = trace_dir / (std::string(unique_name) + ".csv");
    }
    return candidate.string();
}

} // namespace

Editor::Editor() {}

Editor::~Editor() {}

void Editor::Init(int argc, const char** argv) {
    m_engine = MakeUnique<Engine>();
    m_engine->Init(argc, argv);

    Moer::Trace::Config trace_config{};
    trace_config.enable_streaming = true;
    trace_config.host             = "127.0.0.1";
    trace_config.port             = 19090;
    trace_config.queue_limit      = 1u << 16;
    trace_config.enable_csv       = true;
    trace_config.start_recording  = false;
    trace_config.session_name     = "MoerEditor";
    trace_config.csv_path         = BuildUniqueTraceCsvPath();
    Moer::Trace::Init(trace_config);
    Moer::Trace::SetThreadName("MainThread");
}

void Editor::Run() {
    // init
    auto   ui_renderer = MakeUnique<Render::UIRenderer>(RenderDevice::Get());
    uint2& resolution  = m_engine->GetResolution();

    auto editor_ui = MakeUnique<EditorUI>(std::move(ui_renderer), m_engine->GetEditorConfig());
    auto console   = MakeShared<ConsoleSystem>(m_engine->GetEditorConfig());
    editor_ui->RegisterOverlayFunc(
        "Console",
        [console]() {
            console->TickUI();
        }
    );

    // run
    m_engine->Run(
        EngineHooks{
            // Common
            .on_tick_ui =
                [&editor_ui]() {
                    editor_ui->TickUI();
                },
            .on_render_gui =
                [&editor_ui](CommandList& cmd_list, TextureRef output_image) {
                    editor_ui->RenderGUI(cmd_list, output_image);
                },
            .on_present_windows =
                [&editor_ui]() {
                    editor_ui->PresentWindows();
                },
            .on_is_need_reload =
                [&editor_ui]() {
                    return editor_ui->IsNeedReload();
                },
            .on_ui_combine_pass =
                [&editor_ui](
                    UiCombinePass* ui_combine_pass,
                    CommandList&   cmd_list,
                    TextureView    input_color_texture,
                    TextureView    input_ui_texture, // TODO: is this necessary?
                    TextureView    default_output_texture
                ) {
                    auto scene_window_target = editor_ui->GetSceneWindowTarget();
                    return ui_combine_pass->Process(
                        cmd_list,
                        scene_window_target.is_separate_window,
                        editor_ui->GetConfig()->GetResolution(),
                        editor_ui->GetSceneColorPos(),
                        editor_ui->GetSceneColorResolution(),
                        scene_window_target.frame_buffer,
                        input_color_texture,
                        input_ui_texture,
                        default_output_texture
                    );
                },
            .on_register_ui_func =
                [&editor_ui](std::string name, std::function<void(void)> lambda) {
                    editor_ui->RegisterUIFunc(name, std::move(lambda));
                },
            .on_unregister_ui_func =
                [&editor_ui](std::string name) {
                    editor_ui->UnregisterUIFunc(name);
                },
            .on_show_config_sub_ui =
                [&editor_ui]() {
                    editor_ui->SetShowSubUI(true);
                },

            // Raster
            .on_raster_register_frame_buffers =
                [&editor_ui](const Array<TextureView>& textures) {
                    editor_ui->m_raster_ui.RegisterFrameBuffers(textures);
                }
        }
    );

    // release
    m_editor_ui.reset(); // 释放EditorUI资源
}

void Editor::ShutDown() {
    Moer::Trace::Shutdown();
    m_engine->ShutDown();
    m_engine.reset();
}

} // namespace Moer
