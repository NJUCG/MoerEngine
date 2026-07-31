#include "EngineConsoleControl.h"

#include "command/EngineCommandProcessor.h"
#include "config/CVarSystem.h"
#include "renderer/EditorConfig.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace Moer;

[[noreturn]] void Fail(const char* message) {
    std::cerr << "EngineConsoleControlContract failed: " << message << '\n';
    std::exit(1);
}

void Expect(bool condition, const char* message) {
    if (!condition) {
        Fail(message);
    }
}

double SnapshotFloat(std::string_view name) {
    const auto snapshot = CVar::Find(name);
    Expect(snapshot.has_value(), "expected cvar snapshot is missing");
    return std::stod(snapshot->value);
}

} // namespace

int main() {
    EngineConsoleStartupConfig startup_config{
        .render_thread                          = true,
        .rhi_thread                             = true,
        .rhi_bypass                             = false,
        .parallel_recording                     = true,
        .parallel_record_workers                = 3,
        .parallel_record_verify                 = true,
        .parallel_record_profile                = false,
        .parallel_record_min_work_units_per_job = 96,
        .configured_submission_batch_window     = 5,
        .rhi_heartbeat_enabled                  = true,
        .rhi_heartbeat_stall_timeout_ms         = 7000,
        .rhi_heartbeat_poll_interval_ms         = 750,
        .raster_rdg_enabled                     = true,
        .raster_rdg_parallel_recording          = true,
        .raytracing_rdg_enabled                 = true,
        .raytracing_rdg_debug_dump              = true,
    };

    std::shared_ptr<EngineCommandEndpoint> retained_endpoint;
    {
        EngineConsoleControl control(startup_config, 4);
        CVar::SealStartupOnlyCVars();

        const auto submission = CVar::Find("RHI.Submission.BatchWindow.PolicyClamped");
        Expect(
            submission.has_value() && submission->value == "4" &&
                CVar::HasFlag(submission->flags, CVar::EFlags::ReadOnly) &&
                CVar::HasFlag(submission->flags, CVar::EFlags::StartupOnly) && submission->startup_sealed,
            "policy-clamped startup topology was not exposed as read-only metadata"
        );
        Expect(
            CVar::Find("RHI.CommandRecording.Parallel.Configured")->value == "true" &&
                CVar::Find("Render.Raster.RDG.ParallelRecording.Configured")->value == "true" &&
                CVar::Find("Render.Raytracing.RDG.DebugDump.Configured")->value == "true",
            "startup topology cvars do not reflect GlobalConfig"
        );

        EditorConfig editor_config{};
        editor_config.raster_config.bloom_enabled                      = true;
        editor_config.raster_config.tonemapping_exposure_ev            = -2.0f;
        editor_config.raytracing_config.directional_light_intensity    = 6.0f;
        editor_config.raytracing_config.tone_mapping_cfg.exposure_bias = -0.5f;
        control.BindEditorConfig(editor_config);
        Expect(
            !CVar::Find("Render.Raster.Bloom.Enabled")->startup_sealed,
            "global startup seal incorrectly sealed a live cvar"
        );

        retained_endpoint = control.GetCommandEndpoint();
        Expect(
            retained_endpoint->SubmitText("Render.Raster.Bloom.Enabled false") ==
                    Command::ESubmitStatus::Accepted &&
                retained_endpoint->SubmitText("Render.Raster.Tonemapping.ExposureEV 1.5") ==
                    Command::ESubmitStatus::Accepted &&
                retained_endpoint->SubmitText("Render.Raytracing.Tonemapping.ExposureBiasEV 3.25") ==
                    Command::ESubmitStatus::Accepted,
            "live cvar commands were not admitted"
        );
        Expect(
            control.TickGameThread(editor_config, 2) == 2 && !editor_config.raster_config.bloom_enabled &&
                std::abs(editor_config.raster_config.tonemapping_exposure_ev - 1.5f) < 0.0001f &&
                std::abs(editor_config.raytracing_config.tone_mapping_cfg.exposure_bias + 0.5f) < 0.0001f &&
                std::abs(editor_config.raytracing_config.directional_light_intensity - 6.0f) < 0.0001f,
            "bounded Game Thread drain did not apply exactly its first two commands"
        );
        Expect(
            control.TickGameThread(editor_config, 2) == 1 &&
                std::abs(editor_config.raytracing_config.tone_mapping_cfg.exposure_bias - 3.25f) < 0.0001f &&
                std::abs(editor_config.raytracing_config.directional_light_intensity - 6.0f) < 0.0001f,
            "second Game Thread drain did not apply presentation exposure without changing light intensity"
        );

        editor_config.raster_config.tonemapping_exposure_ev = -4.5f;
        Expect(
            control.TickGameThread(editor_config, 0) == 0 &&
                std::abs(SnapshotFloat("Render.Raster.Tonemapping.ExposureEV") + 4.5) < 0.0001,
            "Editor-owned mutation did not synchronize back to the cvar registry"
        );

        editor_config.raytracing_config.tone_mapping_cfg.exposure_bias = -1.25f;
        Expect(
            control.TickGameThread(editor_config, 0) == 0 &&
                std::abs(SnapshotFloat("Render.Raytracing.Tonemapping.ExposureBiasEV") + 1.25) < 0.0001 &&
                std::abs(editor_config.raytracing_config.directional_light_intensity - 6.0f) < 0.0001f,
            "Raytracing presentation exposure did not synchronize without changing light intensity"
        );

        CVar::CVarSetResult threaded_set;
        std::jthread        producer([&threaded_set] {
            threaded_set = CVar::SetValueFromString("Render.Raster.Bloom.Enabled", "true");
        });
        producer.join();
        Expect(threaded_set.Succeeded(), "cross-thread cvar set was rejected");
        Expect(
            !editor_config.raster_config.bloom_enabled && control.TickGameThread(editor_config, 0) == 0 &&
                editor_config.raster_config.bloom_enabled,
            "cross-thread callback mutated EditorConfig outside the Game Thread handoff"
        );

        const CVar::CVarSetResult out_of_range =
            CVar::SetValueFromString("Render.Raster.Tonemapping.ExposureEV", "20");
        Expect(
            out_of_range.status == CVar::ESetStatus::OutOfRange &&
                control.TickGameThread(editor_config, 0) == 0 &&
                std::abs(editor_config.raster_config.tonemapping_exposure_ev + 4.5f) < 0.0001f,
            "range rejection changed the live owner"
        );

        for (int index = 0; index < 65; ++index) {
            Expect(
                retained_endpoint->SubmitText("/cvar.list No.Such.Prefix") ==
                    Command::ESubmitStatus::Accepted,
                "drain budget fixture overflowed the pending queue"
            );
        }
        Expect(
            control.TickGameThread(editor_config) == 64 && control.TickGameThread(editor_config) == 1,
            "default Game Thread drain budget is not 64 commands"
        );

        control.UnbindEditorConfig();
        Expect(
            !CVar::Find("Render.Raster.Bloom.Enabled").has_value() &&
                !CVar::Find("Render.Raster.Tonemapping.ExposureEV").has_value() &&
                !CVar::Find("Render.Raytracing.Tonemapping.ExposureBiasEV").has_value(),
            "live registrations outlived their EditorConfig binding"
        );
    }

    const auto final_submission = CVar::Find("RHI.Submission.BatchWindow.PolicyClamped");
    Expect(!final_submission.has_value(), "startup registrations outlived EngineConsoleControl");
    Expect(
        retained_endpoint && !retained_endpoint->IsAccepting() &&
            retained_endpoint->SubmitText("/help") == Command::ESubmitStatus::Closed,
        "retained async endpoint did not close admission safely at Engine teardown"
    );
    retained_endpoint.reset();
    std::cout << "EngineConsoleControlContract passed\n";
    return 0;
}
