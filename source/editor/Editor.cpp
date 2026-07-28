#include "Editor.h"

#include "Engine.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "trace/Trace.h"

#include <cassert>
#include <filesystem>
#include <string>

// namespace
using namespace Moer::Render;

namespace Moer {
namespace {

bool ContainsNonAscii(const std::filesystem::path& p) {
    const auto& native_path = p.native();
    for (const auto ch : native_path) {
        if (static_cast<uint32_t>(ch) > 127u) {
            return true;
        }
    }
    return false;
}

ERenderMethod ResolveDefaultRenderMethod(std::string_view render_method_name) {
    if (render_method_name == "Raster") {
        return ERenderMethod::Raster;
    }
    if (render_method_name == "Raytracing") {
        return ERenderMethod::Raytracing;
    }

    LOG_WARNING(MOER_TEXT("Invalid default render method: {}. Use Raster instead."), render_method_name);
    return ERenderMethod::Raster;
}

} // namespace

Editor::Editor() {}

Editor::~Editor() {}

void Editor::Init(int argc, const char** argv) {
    LogSystem::Init();

    std::filesystem::path workspace_path = argv[0];
    if (workspace_path.has_extension() && workspace_path.extension() == std::filesystem::path(MOER_TEXT(".exe"))) {
        workspace_path = workspace_path.parent_path();
    }

    const String workspace_path_text = String(workspace_path.native());
    LOG_INFO(MOER_TEXT("Workspace Path : {}"), workspace_path_text);
    if (ContainsNonAscii(workspace_path)) {
        LOG_ERROR(
            MOER_TEXT("Workspace Path contains non-ASCII characters (e.g., Chinese characters)! This may cause unexpected ")
            "issues. Current path: {}",
            workspace_path_text
        );
    }

    ConfigManager::GetInstance().Init(workspace_path);

    m_engine = MakeUnique<Engine>();
    auto editor_config = MakeShared<EditorConfig>();
    const auto& startup_config = ConfigManager::GetInstance().GetConfig();
    editor_config->SetResolution(startup_config.editor.width, startup_config.editor.height);
    editor_config->SetRenderResolution(startup_config.editor.width, startup_config.editor.height);
    editor_config->scene_path = startup_config.engine.scene.scene_path;
    editor_config->selected_render_method =
        ResolveDefaultRenderMethod(startup_config.engine.render.default_render_method);
    m_engine->Init(editor_config, startup_config.editor.fullscreen);

    Moer::Trace::Config trace_config{};
    trace_config.enable_streaming = true;
    trace_config.host             = "127.0.0.1";
    trace_config.port             = 19090;
    trace_config.queue_limit      = 1u << 16;
    trace_config.enable_csv       = true;
    trace_config.start_recording  = false;
    trace_config.session_name     = "MoerEditor";
    Moer::Trace::Init(trace_config);
    Moer::Trace::SetThreadName(MOER_ASCII_TEXT("MainThread"));
}

void Editor::Run() {
    m_engine->Run();
}

void Editor::ShutDown() {
    Moer::Trace::Shutdown();
    m_engine->ShutDown();
    m_engine.reset();
}

} // namespace Moer
