#include "Editor.h"

#include "Engine.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "trace/Trace.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

// namespace
using namespace Moer::Render;

namespace Moer {
namespace {

bool ContainsNonAscii(const std::filesystem::path& p) {
    const std::wstring wide_path_str = p.generic_wstring();
    for (wchar_t wc : wide_path_str) {
        if (wc > 127) {
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

    LOG_WARNING("Invalid default render method: {}. Use Raster instead.", render_method_name);
    return ERenderMethod::Raster;
}

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
    LogSystem::Init();

    std::filesystem::path workspace_path = argv[0];
    workspace_path =
        workspace_path.filename().string().find(".exe") != std::string::npos ?
            workspace_path.parent_path() :
            workspace_path;

    LOG_INFO("Workspace Path : {}", workspace_path.string());
    if (ContainsNonAscii(workspace_path)) {
        LOG_ERROR(
            "Workspace Path contains non-ASCII characters (e.g., Chinese characters)! This may cause unexpected "
            "issues. Current path: {}",
            workspace_path.string()
        );
    }

    ConfigManager::GetInstance().Init(workspace_path);

    m_engine = MakeUnique<Engine>();
    auto editor_config = MakeShared<EditorConfig>();
    const auto& startup_config = ConfigManager::GetInstance().GetConfig();
    editor_config->SetResolution(startup_config.editor.width, startup_config.editor.height);
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
    trace_config.csv_path         = BuildUniqueTraceCsvPath();
    Moer::Trace::Init(trace_config);
    Moer::Trace::SetThreadName("MainThread");
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
