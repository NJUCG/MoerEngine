#include "Engine.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace Moer;
using namespace Moer::Render;

// Exercise the actual render graph and shader bindings across AO history changes.
// Numerical shader regression probes are separate; this checks integration/lifetime.
int main(int argc, const char** argv) {
    struct Phase {
        const char* name;
        EAoMode     ao;
        bool        half_resolution;
        bool        denoise;
        float       indirect_intensity;
        uint32      ssr_samples;
    };
    constexpr std::array phases{
        Phase{"rtao_full", EAoMode::RTAO, false, true, 1.f, 32},
        Phase{"rtao_half", EAoMode::RTAO, true, true, 1.f, 32},
        Phase{"rtao_history_disabled", EAoMode::RTAO, true, false, 1.f, 32},
        Phase{"rtao_history_reenabled", EAoMode::RTAO, true, true, 1.f, 32},
        Phase{"ssdo_full_indirect_off", EAoMode::SSDO, false, true, 0.f, 32},
        Phase{"ssdo_full_indirect_on", EAoMode::SSDO, false, true, 2.f, 32},
        Phase{"ssdo_half", EAoMode::SSDO, true, true, 2.f, 32},
        Phase{"ssdo_ao_only", EAoMode::SSDO_AO_ONLY, true, true, 2.f, 32},
        Phase{"ssao", EAoMode::SSAO, true, true, 1.f, 32},
        Phase{"rtao_after_mode_switch", EAoMode::RTAO, true, true, 1.f, 32},
        Phase{"ssr_budget_exhausted", EAoMode::RTAO, false, true, 1.f, 1},
    };
    constexpr uint32 frames_per_phase = 24;
    Engine engine;
    try {
        engine.Init(argc, argv, false);
        auto config = engine.GetEditorConfig();
        config->selected_render_method = ERenderMethod::Raster;
        config->active_viewport_mode = EEditorViewportMode::Game;
        auto& raster = config->raster_config;
        raster.probe_gi_enabled = false;
        raster.ssr_is_ssr_enabled = true;
        raster.shadow_pcss_enabled = true;
        raster.tonemapping_ae.enabled = true;
        uint32 ready_frames = 0;
        engine.Run(EngineHooks{.on_tick_test = [&](Scene& scene) {
            if (!scene.IsReady()) {
                return;
            }
            const size_t phase_index = ready_frames / frames_per_phase;
            if (phase_index == phases.size()) {
                engine.RequestExit();
                return;
            }
            const auto& phase = phases[phase_index];
            if (ready_frames % frames_per_phase == 0) {
                raster.ao_mode = phase.ao;
                raster.ao_half_resolution = phase.half_resolution;
                raster.rtao_denoiser_enable = phase.denoise;
                raster.ssdo_indirect_intensity = phase.indirect_intensity;
                raster.ssr_sample_count = phase.ssr_samples;
                std::cout << "[ShaderEffectsSmoke] " << phase.name << std::endl;
            }
            ++ready_frames;
        }, .on_capture_window_input = [&] {
            // Use the normal frame-input path; no native input injection is needed.
            WindowInputFrameSnapshot input{};
            input.delta_time = 1.f / 60.f;
            input.cursor_hidden = true;
            if (ready_frames % frames_per_phase >= frames_per_phase / 2) {
                input.camera_right = (ready_frames / frames_per_phase) % 2 == 0;
                input.camera_left = !input.camera_right;
            }
            return input;
        }});
        if (ready_frames != phases.size() * frames_per_phase) {
            throw std::runtime_error("Shader effect phases did not complete");
        }
        engine.ShutDown();
        std::cout << "[TESTCASE][PASS] ShaderEffectsSmoke completed all phases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[TESTCASE][FAIL] " << error.what() << '\n';
        engine.ShutDown();
        return 1;
    }
}
