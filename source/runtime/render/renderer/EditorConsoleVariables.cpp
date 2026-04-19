#include "renderer/EditorConsoleVariables.h"

#include <algorithm>

#include "config/CVarSystem.h"

namespace Moer::Render::EditorConsoleVariables {
namespace {

bool g_rhi_parallel_translate_enable = true;
int  g_rhi_parallel_translate_num_threads = 4;

CVar::TCVar<bool> s_cvar_rhi_parallel_translate_enable(
    TEXT("RHI.Translate.Parallel"),
    g_rhi_parallel_translate_enable,
    TEXT("Enable parallel translate mode for RHI command translation."),
    TEXT("true: use parallel translate."),
    TEXT("false: force single-thread translate.")
);

CVar::TCVar<int> s_cvar_rhi_parallel_translate_num_threads(
    TEXT("RHI.Translate.NumThreads"),
    g_rhi_parallel_translate_num_threads,
    TEXT("Worker thread count used by RHI parallel translate."),
    TEXT(">=1 to increase parallel workers."),
    TEXT("invalid values are clamped to 1.")
);

} // namespace

void CaptureFromEditorConfig(const EditorConfig& config) {
    s_cvar_rhi_parallel_translate_enable.Set(config.raytracing_config.process_light_cfg.parallel_mode);
    s_cvar_rhi_parallel_translate_num_threads.Set(
        std::max(1, config.raytracing_config.process_light_cfg.num_threads)
    );
}

void ApplyToEditorConfig(EditorConfig& config) {
    config.raytracing_config.process_light_cfg.parallel_mode =
        s_cvar_rhi_parallel_translate_enable.Get();
    config.raytracing_config.process_light_cfg.num_threads =
        std::max(1, s_cvar_rhi_parallel_translate_num_threads.Get());
}

} // namespace Moer::Render::EditorConsoleVariables