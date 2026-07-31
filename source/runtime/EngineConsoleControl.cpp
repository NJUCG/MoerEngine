#include "EngineConsoleControl.h"

#include "command/EngineCommandProcessor.h"
#include "config/CVarSystem.h"
#include "renderer/EditorConfig.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Moer {

namespace {

constexpr CVar::EFlags kStartupReadOnlyFlags = CVar::EFlags::ReadOnly | CVar::EFlags::StartupOnly;

CVar::CVarDescriptor MakeDescriptor(
    std::string           name,
    std::string           helper,
    CVar::EFlags          flags     = CVar::EFlags::None,
    std::optional<double> min_value = std::nullopt,
    std::optional<double> max_value = std::nullopt
) {
    return {
        .name      = std::move(name),
        .helper    = std::move(helper),
        .flags     = flags,
        .min_value = min_value,
        .max_value = max_value,
    };
}

void RequireRegistration(
    std::vector<CVar::Registration>& registrations,
    CVar::RegistrationResult         result,
    std::string_view                 name
) {
    if (!result.Succeeded()) {
        throw std::runtime_error(
            "Failed to register console variable '" + std::string(name) +
            "': " + (result.detail != nullptr ? result.detail : "unknown error")
        );
    }
    registrations.push_back(std::move(result.registration));
}

template<typename Value>
struct LiveValueState {
    Value                applied{};
    std::optional<Value> pending;
};

} // namespace

struct EngineCommandEndpoint::Impl {
    explicit Impl(Command::EngineCommandProcessorLimits limits) : processor(limits) {}

    mutable std::recursive_mutex    mutex;
    bool                            accepting = true;
    Command::EngineCommandProcessor processor;
};

EngineCommandEndpoint::EngineCommandEndpoint(Command::EngineCommandProcessorLimits limits) :
    impl(std::make_unique<Impl>(limits)) {}

EngineCommandEndpoint::~EngineCommandEndpoint() = default;

Command::ESubmitStatus EngineCommandEndpoint::SubmitText(std::string_view text) {
    std::scoped_lock lock(impl->mutex);
    if (!impl->accepting) {
        return Command::ESubmitStatus::Closed;
    }
    return impl->processor.SubmitText(text);
}

Command::CommandOutputBatch
EngineCommandEndpoint::PollOutput(std::uint64_t next_sequence, std::size_t max_count) const {
    std::scoped_lock lock(impl->mutex);
    return impl->processor.PollOutput(next_sequence, max_count);
}

std::vector<Command::CommandCandidate>
EngineCommandEndpoint::GetCandidates(std::string_view input, std::size_t max_count) const {
    std::scoped_lock lock(impl->mutex);
    return impl->processor.GetCandidates(input, max_count);
}

bool EngineCommandEndpoint::IsAccepting() const noexcept {
    std::scoped_lock lock(impl->mutex);
    return impl->accepting;
}

Command::ProcessPendingResult EngineCommandEndpoint::ProcessPending(std::size_t max_commands) {
    std::scoped_lock lock(impl->mutex);
    return impl->processor.ProcessPending(max_commands);
}

void EngineCommandEndpoint::CloseAdmission() noexcept {
    std::scoped_lock lock(impl->mutex);
    impl->accepting = false;
}

struct EngineConsoleControl::Impl {
    explicit Impl(
        const EngineConsoleStartupConfig& config,
        unsigned int                      policy_clamped_submission_batch_window
    ) {
        RegisterStartupVariables(config, policy_clamped_submission_batch_window);
    }

    void RegisterStartupVariables(
        const EngineConsoleStartupConfig& config,
        unsigned int                      policy_clamped_submission_batch_window
    ) {
        const auto register_bool = [this](std::string name, std::string helper, bool value) {
            const std::string registered_name = name;
            RequireRegistration(
                startup_registrations,
                CVar::RegisterBool(
                    MakeDescriptor(std::move(name), std::move(helper), kStartupReadOnlyFlags), value
                ),
                registered_name
            );
        };
        const auto register_int = [this](std::string name, std::string helper, std::int64_t value) {
            const std::string registered_name = name;
            RequireRegistration(
                startup_registrations,
                CVar::RegisterInt(
                    MakeDescriptor(std::move(name), std::move(helper), kStartupReadOnlyFlags), value
                ),
                registered_name
            );
        };

        register_bool(
            "Engine.Threading.RenderThread.Configured",
            "Configured request for a dedicated Render Thread.",
            config.render_thread
        );
        register_bool(
            "Engine.Threading.RHIThread.Configured",
            "Configured request for a dedicated RHI thread; bypass and backend policy may disable it.",
            config.rhi_thread
        );
        register_bool(
            "Engine.Threading.RHIBypass.Configured",
            "Configured request to bypass queued RHI translation.",
            config.rhi_bypass
        );
        register_bool(
            "Engine.Threading.ProfileLogging.Configured",
            "Configured request for periodic threading profile diagnostics.",
            config.profile_logging
        );
        register_int(
            "Engine.Threading.MaxFrameLag.Configured",
            "Configured maximum Render Thread frame lag; runtime policy may clamp it.",
            config.max_frame_lag
        );
        register_bool(
            "RHI.CommandRecording.Parallel.Configured",
            "Configured request for parallel backend command-list translation/recording.",
            config.parallel_recording
        );
        register_int(
            "RHI.CommandRecording.Parallel.Workers.Configured",
            "Configured parallel command-recording worker count; zero selects the runtime default.",
            config.parallel_record_workers
        );
        register_bool(
            "RHI.CommandRecording.Parallel.Verify.Configured",
            "Whether parallel command recording runs verification checks.",
            config.parallel_record_verify
        );
        register_bool(
            "RHI.CommandRecording.Parallel.Profile.Configured",
            "Whether parallel command recording emits profiling diagnostics.",
            config.parallel_record_profile
        );
        register_int(
            "RHI.CommandRecording.Parallel.MinWorkUnitsPerJob.Configured",
            "Minimum estimated work units assigned to each parallel recording job.",
            config.parallel_record_min_work_units_per_job
        );
        register_int(
            "RHI.Submission.BatchWindow.Configured",
            "Configured bounded submission batch window before policy clamping.",
            config.configured_submission_batch_window
        );
        register_int(
            "RHI.Submission.BatchWindow.PolicyClamped",
            "Startup request after the generic [1, 8] policy clamp; native queue aliasing may reduce the "
            "effective window further.",
            policy_clamped_submission_batch_window
        );
        register_bool(
            "RHI.Heartbeat.Enabled.Configured",
            "Configured request for the RHI ownership heartbeat watchdog.",
            config.rhi_heartbeat_enabled
        );
        register_int(
            "RHI.Heartbeat.StallTimeoutMs.Configured",
            "Configured RHI heartbeat stall timeout in milliseconds.",
            config.rhi_heartbeat_stall_timeout_ms
        );
        register_int(
            "RHI.Heartbeat.PollIntervalMs.Configured",
            "Configured RHI heartbeat polling interval in milliseconds.",
            config.rhi_heartbeat_poll_interval_ms
        );
        register_int(
            "RHI.MaxFramesInFlight.Configured",
            "Configured maximum frames in flight.",
            config.max_frames_in_flight
        );

        const auto register_rdg =
            [&register_bool](
                std::string_view prefix, bool enabled, bool debug_dump, bool parallel_recording
            ) {
                register_bool(
                    std::string(prefix) + ".Enabled.Configured",
                    "Configured request for the compiled Render Dependency Graph path.",
                    enabled
                );
                register_bool(
                    std::string(prefix) + ".DebugDump.Configured",
                    "Whether the renderer emits compiled RDG diagnostic dumps.",
                    debug_dump
                );
                register_bool(
                    std::string(prefix) + ".ParallelRecording.Configured",
                    "Configured request to record eligible compiled RDG waves in parallel.",
                    parallel_recording
                );
            };
        register_rdg(
            "Render.Raster.RDG",
            config.raster_rdg_enabled,
            config.raster_rdg_debug_dump,
            config.raster_rdg_parallel_recording
        );
        register_rdg(
            "Render.Raytracing.RDG",
            config.raytracing_rdg_enabled,
            config.raytracing_rdg_debug_dump,
            config.raytracing_rdg_parallel_recording
        );
    }

    void BindEditorConfig(EditorConfig& config) {
        UnbindEditorConfig();

        {
            std::scoped_lock lock(live_mutex);
            bloom.applied               = config.raster_config.bloom_enabled;
            raster_exposure.applied     = config.raster_config.tonemapping_exposure_ev;
            raytracing_exposure.applied = config.raytracing_config.tone_mapping_cfg.exposure_bias;
        }

        auto bloom_result = CVar::RegisterBool(
            MakeDescriptor(
                "Render.Raster.Bloom.Enabled",
                "Enable or disable Raster bloom for subsequent frame snapshots."
            ),
            config.raster_config.bloom_enabled,
            [this](bool, bool new_value) {
                std::scoped_lock lock(live_mutex);
                bloom.pending = new_value;
            }
        );
        RequireLiveRegistration(bloom_registration, std::move(bloom_result), "Render.Raster.Bloom.Enabled");

        auto raster_exposure_result = CVar::RegisterFloat(
            MakeDescriptor(
                "Render.Raster.Tonemapping.ExposureEV",
                "Raster tonemapping exposure in EV stops.",
                CVar::EFlags::None,
                -15.0,
                10.0
            ),
            config.raster_config.tonemapping_exposure_ev,
            [this](double, double new_value) {
                std::scoped_lock lock(live_mutex);
                raster_exposure.pending = new_value;
            }
        );
        RequireLiveRegistration(
            raster_exposure_registration,
            std::move(raster_exposure_result),
            "Render.Raster.Tonemapping.ExposureEV"
        );

        auto raytracing_exposure_result = CVar::RegisterFloat(
            MakeDescriptor(
                "Render.Raytracing.Tonemapping.ExposureBiasEV",
                "Raytracing tonemapping exposure bias in EV stops.",
                CVar::EFlags::None,
                -10.0,
                10.0
            ),
            config.raytracing_config.tone_mapping_cfg.exposure_bias,
            [this](double, double new_value) {
                std::scoped_lock lock(live_mutex);
                raytracing_exposure.pending = new_value;
            }
        );
        RequireLiveRegistration(
            raytracing_exposure_registration,
            std::move(raytracing_exposure_result),
            "Render.Raytracing.Tonemapping.ExposureBiasEV"
        );
    }

    void RequireLiveRegistration(
        CVar::Registration&      destination,
        CVar::RegistrationResult result,
        std::string_view         name
    ) {
        if (!result.Succeeded()) {
            UnbindEditorConfig();
            throw std::runtime_error(
                "Failed to register live console variable '" + std::string(name) +
                "': " + (result.detail != nullptr ? result.detail : "unknown error")
            );
        }
        destination = std::move(result.registration);
    }

    void UnbindEditorConfig() noexcept {
        raytracing_exposure_registration.Reset();
        raster_exposure_registration.Reset();
        bloom_registration.Reset();
        std::scoped_lock lock(live_mutex);
        bloom.pending.reset();
        raster_exposure.pending.reset();
        raytracing_exposure.pending.reset();
    }

    void SynchronizeOwners(EditorConfig& config) {
        std::scoped_lock lock(live_mutex);
        if (!bloom.pending.has_value() && config.raster_config.bloom_enabled != bloom.applied) {
            const auto result = bloom_registration.SynchronizeOwnerValue(config.raster_config.bloom_enabled);
            if (result.Succeeded()) {
                bloom.applied = config.raster_config.bloom_enabled;
            }
        }
        const double current_raster_exposure =
            static_cast<double>(config.raster_config.tonemapping_exposure_ev);
        if (!raster_exposure.pending.has_value() && current_raster_exposure != raster_exposure.applied) {
            const auto result = raster_exposure_registration.SynchronizeOwnerValue(current_raster_exposure);
            if (result.Succeeded()) {
                raster_exposure.applied = current_raster_exposure;
            }
        }
        const double current_raytracing_exposure =
            static_cast<double>(config.raytracing_config.tone_mapping_cfg.exposure_bias);
        if (!raytracing_exposure.pending.has_value() &&
            current_raytracing_exposure != raytracing_exposure.applied) {
            const auto result =
                raytracing_exposure_registration.SynchronizeOwnerValue(current_raytracing_exposure);
            if (result.Succeeded()) {
                raytracing_exposure.applied = current_raytracing_exposure;
            }
        }
    }

    void ApplyPending(EditorConfig& config) {
        std::scoped_lock lock(live_mutex);
        if (bloom.pending.has_value()) {
            config.raster_config.bloom_enabled = *bloom.pending;
            static_cast<void>(bloom_registration.SynchronizeOwnerValue(*bloom.pending));
            bloom.applied = *bloom.pending;
            bloom.pending.reset();
        }
        if (raster_exposure.pending.has_value()) {
            config.raster_config.tonemapping_exposure_ev = static_cast<float>(*raster_exposure.pending);
            const double applied = static_cast<double>(config.raster_config.tonemapping_exposure_ev);
            static_cast<void>(raster_exposure_registration.SynchronizeOwnerValue(applied));
            raster_exposure.applied = applied;
            raster_exposure.pending.reset();
        }
        if (raytracing_exposure.pending.has_value()) {
            config.raytracing_config.tone_mapping_cfg.exposure_bias =
                static_cast<float>(*raytracing_exposure.pending);
            const double applied =
                static_cast<double>(config.raytracing_config.tone_mapping_cfg.exposure_bias);
            static_cast<void>(raytracing_exposure_registration.SynchronizeOwnerValue(applied));
            raytracing_exposure.applied = applied;
            raytracing_exposure.pending.reset();
        }
    }

    std::shared_ptr<EngineCommandEndpoint> command_endpoint = std::make_shared<EngineCommandEndpoint>();
    std::vector<CVar::Registration>        startup_registrations;

    std::mutex             live_mutex;
    LiveValueState<bool>   bloom;
    LiveValueState<double> raster_exposure;
    LiveValueState<double> raytracing_exposure;
    CVar::Registration     bloom_registration;
    CVar::Registration     raster_exposure_registration;
    CVar::Registration     raytracing_exposure_registration;
};

EngineConsoleControl::EngineConsoleControl(
    const EngineConsoleStartupConfig& config,
    unsigned int                      policy_clamped_submission_batch_window
) :
    impl(std::make_unique<Impl>(config, policy_clamped_submission_batch_window)) {}

EngineConsoleControl::~EngineConsoleControl() {
    impl->command_endpoint->CloseAdmission();
    impl->UnbindEditorConfig();
}

void EngineConsoleControl::BindEditorConfig(EditorConfig& config) {
    impl->BindEditorConfig(config);
}

void EngineConsoleControl::UnbindEditorConfig() noexcept {
    impl->UnbindEditorConfig();
}

std::size_t EngineConsoleControl::TickGameThread(EditorConfig& config, std::size_t max_commands) {
    impl->SynchronizeOwners(config);
    const Command::ProcessPendingResult result = impl->command_endpoint->ProcessPending(max_commands);
    impl->ApplyPending(config);
    return result.processed;
}

std::shared_ptr<EngineCommandEndpoint> EngineConsoleControl::GetCommandEndpoint() const noexcept {
    return impl->command_endpoint;
}

} // namespace Moer
