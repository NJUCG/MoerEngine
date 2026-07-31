#include "Engine.h"
#include "EngineConsoleControl.h"

// Runtime
#include "config/CVarSystem.h"
#include "config/ConfigManager.h"
#include "misc/Assert.h"
#include "misc/ScopedLogTimer.h"
#include "remote/RemoteConfig.h"
#include "remote/RemoteModule.h"
#include "RenderThread.h"
#include "rhi/RHI.h"
#include "rhi/RHISubmissionPipelinePolicy.h"
#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptHost.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"


// Editor
#include "renderer/common/RuntimeAssets.h"
#include "renderer/raster/RasterRenderer.h"
#include "renderer/raytracing/RaytracingRenderer.h"

// 3rd party (std)
#include <algorithm>
#include <cassert>
#include <chrono>
#include <charconv>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// namespace
using namespace Moer::Render;

namespace Moer {

static bool ContainsNonAscii(const std::filesystem::path& p);

namespace {

using ThreadProfileClock = std::chrono::steady_clock;

constexpr std::uint64_t kProfileCaptureValidationMinGpuFrames = 4;
constexpr std::uint64_t kProfileCaptureValidationMaxGtTicks   = 60000;
constexpr auto kProfileCaptureValidationTimeout = std::chrono::seconds(180);

[[nodiscard]] bool ProfileDumpTerminalIsClean(
    const ProfileDump::RuntimeStats& _stats,
    std::uint64_t                    _generation
) noexcept {
    return _stats.state == ProfileDump::RuntimeState::Stopped &&
           _stats.last_fault == ProfileDump::RuntimeFault::None &&
           _stats.generation == _generation &&
           _stats.records_dropped_stopped == 0 &&
           _stats.records_dropped_stale_generation == 0 &&
           _stats.records_dropped_oversized == 0 &&
           _stats.records_dropped_queue_full == 0 &&
           _stats.records_dropped_after_fault == 0;
}

double ThreadProfileMilliseconds(
    ThreadProfileClock::time_point begin,
    ThreadProfileClock::time_point end
) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

class RenderThreadProfileAccumulator {
public:
    explicit RenderThreadProfileAccumulator(bool enabled) : enabled(enabled) {}

    void RecordPrepare(double milliseconds) {
        if (!enabled) {
            return;
        }
        std::unique_lock<std::mutex> lock(mutex);
        ++prepare_samples;
        prepare_total_ms += milliseconds;
        prepare_max_ms = std::max(prepare_max_ms, milliseconds);
    }

    void RecordRender(double queue_wait_ms, double render_ms) {
        if (!enabled) {
            return;
        }
        std::unique_lock<std::mutex> lock(mutex);
        ++render_samples;
        queue_wait_total_ms += queue_wait_ms;
        queue_wait_max_ms = std::max(queue_wait_max_ms, queue_wait_ms);
        render_total_ms += render_ms;
        render_max_ms = std::max(render_max_ms, render_ms);
    }

    void RecordGameThreadWait(double milliseconds, size_t pending_frames) {
        if (!enabled) {
            return;
        }
        std::unique_lock<std::mutex> lock(mutex);
        ++game_wait_samples;
        game_wait_total_ms += milliseconds;
        game_wait_max_ms = std::max(game_wait_max_ms, milliseconds);
        max_pending_frames = std::max(max_pending_frames, pending_frames);
    }

    void MaybeLog() {
        if (!enabled) {
            return;
        }

        const auto                   now = ThreadProfileClock::now();
        std::unique_lock<std::mutex> lock(mutex);
        const double                 window_ms = ThreadProfileMilliseconds(window_start, now);
        if (window_ms < 1000.0 || render_samples == 0) {
            return;
        }

        LOG_INFO(
            "[ThreadingProfile][RT] window_ms={:.3f} frames={} prepare_samples={} "
            "gt_wait_samples={} prepare_avg_ms={:.3f} prepare_max_ms={:.3f} "
            "queue_wait_avg_ms={:.3f} queue_wait_max_ms={:.3f} render_avg_ms={:.3f} "
            "render_max_ms={:.3f} gt_wait_avg_ms={:.3f} gt_wait_max_ms={:.3f} "
            "max_pending={}",
            window_ms,
            render_samples,
            prepare_samples,
            game_wait_samples,
            prepare_samples == 0 ? 0.0 : prepare_total_ms / double(prepare_samples),
            prepare_max_ms,
            queue_wait_total_ms / double(render_samples),
            queue_wait_max_ms,
            render_total_ms / double(render_samples),
            render_max_ms,
            game_wait_samples == 0 ? 0.0 : game_wait_total_ms / double(game_wait_samples),
            game_wait_max_ms,
            max_pending_frames
        );

        window_start        = now;
        prepare_samples     = 0;
        render_samples      = 0;
        game_wait_samples   = 0;
        prepare_total_ms    = 0.0;
        prepare_max_ms      = 0.0;
        queue_wait_total_ms = 0.0;
        queue_wait_max_ms   = 0.0;
        render_total_ms     = 0.0;
        render_max_ms       = 0.0;
        game_wait_total_ms  = 0.0;
        game_wait_max_ms    = 0.0;
        max_pending_frames  = 0;
    }

private:
    bool enabled = false;

    std::mutex                      mutex;
    ThreadProfileClock::time_point window_start        = ThreadProfileClock::now();
    uint64                          prepare_samples     = 0;
    uint64                          render_samples      = 0;
    uint64                          game_wait_samples   = 0;
    double                          prepare_total_ms    = 0.0;
    double                          prepare_max_ms      = 0.0;
    double                          queue_wait_total_ms = 0.0;
    double                          queue_wait_max_ms   = 0.0;
    double                          render_total_ms     = 0.0;
    double                          render_max_ms       = 0.0;
    double                          game_wait_total_ms  = 0.0;
    double                          game_wait_max_ms    = 0.0;
    size_t                          max_pending_frames  = 0;
};

std::optional<std::filesystem::path> ParseConfigOverride(int argc, const char** argv) {
    constexpr std::string_view config_prefix = "--config=";

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--config") {
            if (index + 1 >= argc || std::string_view(argv[index + 1]).empty()) {
                throw std::invalid_argument("--config requires a TOML file path");
            }
            return std::filesystem::path(argv[index + 1]);
        }
        if (argument.starts_with(config_prefix)) {
            const std::string_view path = argument.substr(config_prefix.size());
            if (path.empty()) {
                throw std::invalid_argument("--config requires a TOML file path");
            }
            return std::filesystem::path(path);
        }
    }

    return std::nullopt;
}

uint64_t ParseVulkanPresentSubmitFaultTrigger(int argc, const char** argv) {
    constexpr std::string_view argument_name = "--vulkan-fault-inject";
    constexpr std::string_view prefix        = "--vulkan-fault-inject=";
    constexpr std::string_view point_prefix  = "present-submit@";

    std::optional<std::string_view> specification;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        std::optional<std::string_view> candidate;
        if (argument == argument_name) {
            if (index + 1 >= argc) {
                throw std::invalid_argument("--vulkan-fault-inject requires <point>@<positive-count>");
            }
            candidate = std::string_view(argv[++index]);
        } else if (argument.starts_with(prefix)) {
            candidate = argument.substr(prefix.size());
        }

        if (!candidate.has_value()) {
            continue;
        }
        if (specification.has_value()) {
            throw std::invalid_argument("--vulkan-fault-inject may be specified only once");
        }
        specification = *candidate;
    }

    if (!specification.has_value()) {
        return 0;
    }
    if (!specification->starts_with(point_prefix)) {
        throw std::invalid_argument(
            "unsupported Vulkan fault point; expected present-submit@<positive-count>"
        );
    }

    const std::string_view count_text = specification->substr(point_prefix.size());
    uint64_t               count      = 0;
    const auto [end, error] = std::from_chars(
        count_text.data(), count_text.data() + count_text.size(), count
    );
    if (count_text.empty() || error != std::errc{} || end != count_text.data() + count_text.size() ||
        count == 0) {
        throw std::invalid_argument(
            "Vulkan fault trigger count must be a positive integer: present-submit@<positive-count>"
        );
    }
    return count;
}

uint64_t ParseParallelRecordWorkerThrowTrigger(int argc, const char** argv) {
    constexpr std::string_view argument_name = "--parallel-record-fault-inject";
    constexpr std::string_view prefix        = "--parallel-record-fault-inject=";
    constexpr std::string_view point_prefix  = "worker-throw@";

    std::optional<std::string_view> specification;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        std::optional<std::string_view> candidate;
        if (argument == argument_name) {
            if (index + 1 >= argc) {
                throw std::invalid_argument(
                    "--parallel-record-fault-inject requires worker-throw@<positive-count>"
                );
            }
            candidate = std::string_view(argv[++index]);
        } else if (argument.starts_with(prefix)) {
            candidate = argument.substr(prefix.size());
        }

        if (!candidate.has_value()) {
            continue;
        }
        if (specification.has_value()) {
            throw std::invalid_argument(
                "--parallel-record-fault-inject may be specified only once"
            );
        }
        specification = *candidate;
    }

    if (!specification.has_value()) {
        return 0;
    }
    if (!specification->starts_with(point_prefix)) {
        throw std::invalid_argument(
            "unsupported parallel-record fault point; expected worker-throw@<positive-count>"
        );
    }

    const std::string_view count_text = specification->substr(point_prefix.size());
    uint64_t               count      = 0;
    const auto [end, error] = std::from_chars(
        count_text.data(), count_text.data() + count_text.size(), count
    );
    if (count_text.empty() || error != std::errc{} || end != count_text.data() + count_text.size() ||
        count == 0) {
        throw std::invalid_argument(
            "parallel-record worker fault trigger must be a positive integer: "
            "worker-throw@<positive-count>"
        );
    }
    return count;
}

bool ParseThreadingRendererSwitchValidation(int argc, const char** argv) {
    constexpr std::string_view argument_name = "--threading-renderer-switch-validation";
    constexpr std::string_view value_prefix  = "--threading-renderer-switch-validation=";

    bool enabled = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument.starts_with(value_prefix)) {
            throw std::invalid_argument(
                "--threading-renderer-switch-validation is a flag and does not accept a value"
            );
        }
        if (argument != argument_name) {
            continue;
        }
        if (enabled) {
            throw std::invalid_argument(
                "--threading-renderer-switch-validation may be specified only once"
            );
        }
        enabled = true;
    }
    return enabled;
}

bool ParseThreadingRasterLifecycleValidation(int argc, const char** argv) {
    constexpr std::string_view argument_name = "--threading-raster-lifecycle-validation";
    constexpr std::string_view value_prefix  = "--threading-raster-lifecycle-validation=";

    bool enabled = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument.starts_with(value_prefix)) {
            throw std::invalid_argument(
                "--threading-raster-lifecycle-validation is a flag and does not accept a value"
            );
        }
        if (argument != argument_name) {
            continue;
        }
        if (enabled) {
            throw std::invalid_argument(
                "--threading-raster-lifecycle-validation may be specified only once"
            );
        }
        enabled = true;
    }
    return enabled;
}

bool ParseProfileCaptureLifecycleValidation(int argc, const char** argv) {
    constexpr std::string_view argument_name =
        "--profile-capture-lifecycle-validation";
    constexpr std::string_view value_prefix =
        "--profile-capture-lifecycle-validation=";

    bool enabled = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument.starts_with(value_prefix)) {
            throw std::invalid_argument(
                "--profile-capture-lifecycle-validation is a flag and does not "
                "accept a value"
            );
        }
        if (argument != argument_name) {
            continue;
        }
        if (enabled) {
            throw std::invalid_argument(
                "--profile-capture-lifecycle-validation may be specified only once"
            );
        }
        enabled = true;
    }
    return enabled;
}

std::optional<std::string>
ParseThreadingRasterFramebufferValidation(int argc, const char** argv) {
    constexpr std::string_view argument_name = "--threading-raster-framebuffer-validation";
    constexpr std::string_view value_prefix  = "--threading-raster-framebuffer-validation=";
    constexpr std::string_view allowed_names[] = {
        "base_color",
        "normal",
        "depth_linear_sampler",
        "tonemapping_output",
    };

    std::optional<std::string_view> selection;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        std::optional<std::string_view> candidate;
        if (argument == argument_name) {
            if (index + 1 >= argc || std::string_view(argv[index + 1]).empty()) {
                throw std::invalid_argument(
                    "--threading-raster-framebuffer-validation requires a framebuffer name"
                );
            }
            candidate = std::string_view(argv[++index]);
        } else if (argument.starts_with(value_prefix)) {
            candidate = argument.substr(value_prefix.size());
        }
        if (!candidate.has_value()) {
            continue;
        }
        if (selection.has_value()) {
            throw std::invalid_argument(
                "--threading-raster-framebuffer-validation may be specified only once"
            );
        }
        if (std::find(std::begin(allowed_names), std::end(allowed_names), *candidate) ==
            std::end(allowed_names)) {
            throw std::invalid_argument(
                "unsupported Raster framebuffer validation target: " + std::string(*candidate)
            );
        }
        selection = *candidate;
    }
    return selection.has_value() ? std::optional<std::string>(std::string(*selection)) : std::nullopt;
}

ERenderMethod ParseDefaultRenderMethod(std::string_view render_method_name) {
    if (render_method_name == "Raster") {
        return ERenderMethod::Raster;
    }
    if (render_method_name == "Raytracing") {
        return ERenderMethod::Raytracing;
    }

    LOG_WARNING("Invalid default render method: {}. Use Raster instead.", render_method_name);
    return ERenderMethod::Raster;
}

template<typename PrepareFunction, typename RenderFunction, typename ApplyFunction, typename StopFunction>
void RunBoundedRenderLoop(
    RenderThreadService& service,
    uint                 max_frame_lag,
    bool                 profile_logging,
    PrepareFunction      prepare_frame,
    RenderFunction       render_frame,
    ApplyFunction        apply_feedback,
    StopFunction         should_stop
) {
    using FramePacket = std::remove_cvref_t<std::invoke_result_t<PrepareFunction&>>;
    using Feedback = std::remove_cvref_t<std::invoke_result_t<RenderFunction&, FramePacket>>;

    BoundedRenderFrameQueue<Feedback> frame_queue(service, max_frame_lag);
    RenderThreadProfileAccumulator    profile(profile_logging);
    bool                              overlap_logged = false;
    auto retire_feedback = [&](Feedback feedback) {
        std::invoke(apply_feedback, std::move(feedback));
    };

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        frame_queue.RetireCompleted(retire_feedback);

        ThreadProfileClock::time_point prepare_started{};
        if (profile_logging) {
            prepare_started = ThreadProfileClock::now();
        }
        FramePacket frame_packet = std::invoke(prepare_frame);
        if (profile_logging) {
            profile.RecordPrepare(
                ThreadProfileMilliseconds(prepare_started, ThreadProfileClock::now())
            );
        }
        const uint64 frame_id     = frame_packet.frame_id;
        ThreadProfileClock::time_point submitted_at{};
        if (profile_logging) {
            submitted_at = ThreadProfileClock::now();
        }
        frame_queue.Submit(
            frame_id,
            [render_frame,
             frame_packet = std::move(frame_packet),
             submitted_at,
             profile_logging,
             &profile]() mutable -> Feedback {
                assert(IsCurrentlyRenderThread());
                if (!profile_logging) {
                    return std::invoke(render_frame, std::move(frame_packet));
                }

                const auto render_started = ThreadProfileClock::now();
                const double queue_wait_ms =
                    ThreadProfileMilliseconds(submitted_at, render_started);
                Feedback feedback = std::invoke(render_frame, std::move(frame_packet));
                profile.RecordRender(
                    queue_wait_ms,
                    ThreadProfileMilliseconds(render_started, ThreadProfileClock::now())
                );
                return feedback;
            }
        );

        if (!overlap_logged && max_frame_lag > 0 && frame_queue.PendingFrameCount() > 1) {
            overlap_logged = true;
            LOG_INFO(
                "[Threading] GT/RT overlap active at frame {}; pending render frames={}; max_frame_lag={}.",
                frame_id,
                frame_queue.PendingFrameCount(),
                max_frame_lag
            );
        }

        const size_t pending_before_limit = frame_queue.PendingFrameCount();
        ThreadProfileClock::time_point game_wait_started{};
        if (profile_logging) {
            game_wait_started = ThreadProfileClock::now();
        }
        frame_queue.EnforceLagLimit(retire_feedback);
        if (profile_logging) {
            profile.RecordGameThreadWait(
                ThreadProfileMilliseconds(game_wait_started, ThreadProfileClock::now()),
                pending_before_limit
            );
            profile.MaybeLog();
        }
        if (std::invoke(should_stop)) {
            break;
        }
    }

    frame_queue.Flush(retire_feedback);
    LOG_INFO("[Threading] Render frame queue drained before renderer shutdown/reload.");
}

} // namespace

Engine::Engine() {}

void Engine::ValidateCommandLine(int argc, const char** argv) {
    (void)ParseVulkanPresentSubmitFaultTrigger(argc, argv);
    (void)ParseParallelRecordWorkerThrowTrigger(argc, argv);
    const bool renderer_switch_validation =
        ParseThreadingRendererSwitchValidation(argc, argv);
    const bool raster_lifecycle_validation =
        ParseThreadingRasterLifecycleValidation(argc, argv);
    const auto raster_framebuffer_validation =
        ParseThreadingRasterFramebufferValidation(argc, argv);
    const bool profile_capture_lifecycle_validation =
        ParseProfileCaptureLifecycleValidation(argc, argv);
    if (renderer_switch_validation && raster_lifecycle_validation) {
        throw std::invalid_argument(
            "renderer-switch and Raster lifecycle validation modes are mutually exclusive"
        );
    }
    if (profile_capture_lifecycle_validation &&
        (renderer_switch_validation || raster_lifecycle_validation ||
         raster_framebuffer_validation.has_value())) {
        throw std::invalid_argument(
            "profile-capture lifecycle validation is mutually exclusive with "
            "threading Raster validation modes"
        );
    }
    if (!renderer_switch_validation && !raster_lifecycle_validation &&
        !raster_framebuffer_validation.has_value() &&
        !profile_capture_lifecycle_validation) {
        return;
    }

    std::filesystem::path workspace_path = argv[0];
    workspace_path = workspace_path.filename().string().find(".exe") != std::string::npos ?
                         workspace_path.parent_path() :
                         workspace_path;
    const std::filesystem::path config_path =
        ParseConfigOverride(argc, argv).value_or(workspace_path / "MoerEngine.toml");
    if (!std::filesystem::is_regular_file(config_path)) {
        throw std::invalid_argument(
            "validation config does not exist: " +
            config_path.generic_string()
        );
    }
    const auto validation_config =
        Config::GlobalConfig::LoadConfigFromTomlFile(config_path.generic_string());
    if ((renderer_switch_validation || raster_lifecycle_validation ||
         raster_framebuffer_validation.has_value()) &&
        validation_config.engine.render.default_render_method != "Raster") {
        throw std::invalid_argument(
            "threading Raster validation requires "
            "engine.render.default_render_method = \"Raster\""
        );
    }
    if (profile_capture_lifecycle_validation &&
        !validation_config.engine.profile_dump.enabled) {
        throw std::invalid_argument(
            "--profile-capture-lifecycle-validation requires "
            "engine.profile_dump.enabled = true"
        );
    }
    if (profile_capture_lifecycle_validation &&
        validation_config.engine.profile_dump.output_path.empty()) {
        throw std::invalid_argument(
            "--profile-capture-lifecycle-validation requires a non-empty "
            "engine.profile_dump.output_path"
        );
    }
}

Engine::~Engine() {
    ShutDown();
}

void Engine::InitializeProfileDump() noexcept {
    const auto& profile_config =
        ConfigManager::GetInstance().GetConfig().engine.profile_dump;
    if (!profile_config.enabled || !m_profile_capture_controller) {
        return;
    }

    try {
        std::filesystem::path output_path(profile_config.output_path);
        if (output_path.empty()) {
            LOG_WARNING("[ProfileDump] Capture is enabled but output_path is empty; capture disabled.");
            return;
        }

        if (output_path.is_relative()) {
            output_path = ConfigManager::GetInstance().GetWorkspacePath() / output_path;
        }

        std::error_code path_error;
        output_path = std::filesystem::weakly_canonical(output_path, path_error);
        if (path_error) {
            LOG_WARNING(
                "[ProfileDump] Failed to resolve output path '{}': {}; capture disabled.",
                profile_config.output_path,
                path_error.message()
            );
            return;
        }

        Render::ProfileCaptureStartOptions options;
        options.runtime.output_path      = output_path;
        options.runtime.replace_existing = profile_config.replace_existing;

        Render::ProfileCaptureRequestSubmission submission =
            m_profile_capture_controller->RequestStart(std::move(options));
        if (submission.result != Render::ProfileCaptureSubmitResult::Queued) {
            LOG_WARNING(
                "[ProfileDump] Bootstrap request rejected (result={}); capture disabled.",
                static_cast<unsigned int>(submission.result)
            );
            return;
        }

        const Render::ProfileCaptureTickResult tick_result =
            m_profile_capture_controller->TickOwner();
        Render::ProfileCaptureRequestCompletion completion;
        if (tick_result == Render::ProfileCaptureTickResult::WrongThread ||
            !submission.ticket.TryGet(completion)) {
            LOG_WARNING(
                "[ProfileDump] Bootstrap request did not complete synchronously "
                "(tick_result={}); capture disabled.",
                static_cast<unsigned int>(tick_result)
            );
            return;
        }

        const bool cpu_only =
            completion.status == Render::ProfileCaptureCompletionStatus::StartedCpuOnly;
        if (completion.status != Render::ProfileCaptureCompletionStatus::Started &&
            !cpu_only) {
            LOG_WARNING(
                "[ProfileDump] Bootstrap start failed "
                "(status={}, detail={}, secondary_detail={}); capture disabled.",
                static_cast<unsigned int>(completion.status),
                completion.detail,
                completion.secondary_detail
            );
            return;
        }

        if (cpu_only) {
            LOG_WARNING(
                "[ProfileDump] CPU-only capture started because the stable GPU facade "
                "is unavailable: generation={}, output='{}', replace_existing={}.",
                completion.generation,
                output_path.generic_string(),
                profile_config.replace_existing
            );
        } else {
            LOG_INFO(
                "[ProfileDump] Capture started: generation={}, output='{}', "
                "replace_existing={}.",
                completion.generation,
                output_path.generic_string(),
                profile_config.replace_existing
            );
        }
    } catch (const std::exception& error) {
        try {
            LOG_WARNING(
                "[ProfileDump] Capture initialization failed: {}; capture disabled.",
                error.what()
            );
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_WARNING("[ProfileDump] Capture initialization failed; capture disabled.");
        } catch (...) {
        }
    }
}

void Engine::InitializeProfileCaptureLifecycleValidation() noexcept {
    if (!m_profile_capture_lifecycle_validation_enabled) {
        m_profile_capture_lifecycle_validation_stage =
            EProfileCaptureLifecycleValidationStage::Disabled;
        return;
    }

    try {
        if (!m_profile_capture_controller || !m_render_profile_capture) {
            FailProfileCaptureLifecycleValidation(
                "Engine did not create both the capture controller and stable GPU facade"
            );
            return;
        }

        const Render::ProfileCaptureControllerSnapshot snapshot =
            m_profile_capture_controller->GetSnapshot();
        const Render::RenderProfileCaptureStats stats =
            m_render_profile_capture->GetStats();
        if (snapshot.state != Render::ProfileCaptureControllerState::Running ||
            !snapshot.owns_runtime || !snapshot.cpu_producer_active ||
            !snapshot.gpu_session_active || snapshot.active_generation == 0 ||
            !m_render_profile_capture->Valid() || !stats.accepting ||
            stats.generation != snapshot.active_generation) {
            FailProfileCaptureLifecycleValidation(
                "bootstrap did not produce a GPU-capable Running generation"
            );
            return;
        }

        const auto& profile_config =
            ConfigManager::GetInstance().GetConfig().engine.profile_dump;
        std::filesystem::path output_path(profile_config.output_path);
        if (output_path.is_relative()) {
            output_path =
                ConfigManager::GetInstance().GetWorkspacePath() / output_path;
        }

        std::error_code path_error;
        output_path = std::filesystem::weakly_canonical(output_path, path_error);
        if (path_error || output_path.empty()) {
            FailProfileCaptureLifecycleValidation(
                "bootstrap output path could not be resolved for restart"
            );
            return;
        }

        const std::string extension = output_path.extension().string();
        std::string       restart_filename;
        if (extension.empty()) {
            restart_filename = output_path.filename().string() + ".restart";
        } else {
            restart_filename =
                output_path.stem().string() + ".restart" + extension;
        }
        std::filesystem::path restart_path =
            output_path.parent_path() / restart_filename;
        if (restart_path == output_path) {
            FailProfileCaptureLifecycleValidation(
                "restart output path aliases the bootstrap output path"
            );
            return;
        }

        m_profile_capture_validation_restart_options = {};
        m_profile_capture_validation_restart_options.runtime.output_path =
            std::move(restart_path);
        // Validation must be repeatable without requiring callers to clean a
        // previous successful restart artifact.
        m_profile_capture_validation_restart_options.runtime.replace_existing =
            true;
        m_profile_capture_validation_first_generation =
            snapshot.active_generation;
        m_profile_capture_validation_second_generation = 0;
        m_profile_capture_validation_gt_ticks          = 0;
        m_profile_capture_validation_ticket            = {};
        m_profile_capture_validation_deadline =
            std::chrono::steady_clock::now() +
            kProfileCaptureValidationTimeout;
        m_profile_capture_lifecycle_validation_stage =
            EProfileCaptureLifecycleValidationStage::
                WaitingForFirstGenerationFrames;

        LOG_INFO(
            "[ProfileCaptureValidation] Enabled on the Game Thread: "
            "generation_a={}, restart_output='{}', required_gpu_frames={}, "
            "timeout_seconds={}, max_gt_ticks={}, stable_facade={}.",
            m_profile_capture_validation_first_generation,
            m_profile_capture_validation_restart_options.runtime.output_path
                .generic_string(),
            kProfileCaptureValidationMinGpuFrames,
            std::chrono::duration_cast<std::chrono::seconds>(
                kProfileCaptureValidationTimeout
            ).count(),
            kProfileCaptureValidationMaxGtTicks,
            static_cast<const void*>(m_render_profile_capture.get())
        );
    } catch (...) {
        FailProfileCaptureLifecycleValidation(
            "unexpected exception while arming validation"
        );
    }
}

void Engine::FailProfileCaptureLifecycleValidation(
    std::string_view reason
) noexcept {
    if (!m_profile_capture_lifecycle_validation_enabled ||
        m_profile_capture_lifecycle_validation_stage ==
            EProfileCaptureLifecycleValidationStage::Complete ||
        m_profile_capture_lifecycle_validation_stage ==
            EProfileCaptureLifecycleValidationStage::Failed) {
        return;
    }

    Render::ProfileCaptureControllerSnapshot snapshot{};
    Render::RenderProfileCaptureStats         stats{};
    if (m_profile_capture_controller) {
        snapshot = m_profile_capture_controller->GetSnapshot();
    }
    if (m_render_profile_capture) {
        stats = m_render_profile_capture->GetStats();
    }

    try {
        LOG_ERROR(
            "[ProfileCaptureValidation][FAIL] reason='{}', stage={}, "
            "gt_ticks={}, controller_state={}, generation={}, owns_runtime={}, "
            "gpu_session_active={}, facade_generation={}, facade_accepting={}, "
            "frames={}, scopes={}.",
            reason,
            static_cast<unsigned int>(
                m_profile_capture_lifecycle_validation_stage
            ),
            m_profile_capture_validation_gt_ticks,
            static_cast<unsigned int>(snapshot.state),
            snapshot.active_generation,
            snapshot.owns_runtime,
            snapshot.gpu_session_active,
            stats.generation,
            stats.accepting,
            stats.frames_emitted,
            stats.scopes_emitted
        );
    } catch (...) {
    }

    m_profile_capture_lifecycle_validation_stage =
        EProfileCaptureLifecycleValidationStage::Failed;
    if (m_window_context_initialized &&
        !m_profile_capture_validation_exit_requested) {
        m_profile_capture_validation_exit_requested = true;
        try {
            RequestExit();
        } catch (...) {
        }
    }
}

void Engine::TickProfileCaptureLifecycleValidation(Render::ProfileCaptureTickResult tick_result) noexcept {
    assert(IsCurrentlyGameThread());
    try {
        if (!m_profile_capture_lifecycle_validation_enabled ||
            m_profile_capture_lifecycle_validation_stage ==
                EProfileCaptureLifecycleValidationStage::Disabled ||
            m_profile_capture_lifecycle_validation_stage ==
                EProfileCaptureLifecycleValidationStage::Complete) {
            return;
        }
        if (m_profile_capture_lifecycle_validation_stage == EProfileCaptureLifecycleValidationStage::Failed) {
            if (!m_profile_capture_validation_exit_requested) {
                m_profile_capture_validation_exit_requested = true;
                try {
                    RequestExit();
                } catch (...) {
                }
            }
            return;
        }

        ++m_profile_capture_validation_gt_ticks;
        if (tick_result == Render::ProfileCaptureTickResult::WrongThread) {
            FailProfileCaptureLifecycleValidation("controller TickOwner did not run on its Game Thread owner"
            );
            return;
        }
        if (tick_result == Render::ProfileCaptureTickResult::Shutdown) {
            FailProfileCaptureLifecycleValidation("controller shut down before validation completed");
            return;
        }
        if (m_profile_capture_validation_gt_ticks > kProfileCaptureValidationMaxGtTicks ||
            std::chrono::steady_clock::now() > m_profile_capture_validation_deadline) {
            FailProfileCaptureLifecycleValidation("bounded Game Thread validation timeout expired");
            return;
        }
        if (!m_profile_capture_controller || !m_render_profile_capture) {
            FailProfileCaptureLifecycleValidation("controller or stable GPU facade disappeared");
            return;
        }

        const auto live_generation_matches = [this](
                                                 const Render::ProfileCaptureControllerSnapshot& snapshot,
                                                 const Render::RenderProfileCaptureStats&        stats,
                                                 std::uint64_t                                   generation
                                             ) noexcept {
            return generation != 0 && snapshot.state == Render::ProfileCaptureControllerState::Running &&
                   snapshot.owns_runtime && snapshot.cpu_producer_active && snapshot.gpu_session_active &&
                   snapshot.active_generation == generation && m_render_profile_capture->Valid() &&
                   stats.accepting && !stats.closed && stats.generation == generation;
        };
        const auto accept_submission =
            [this](Render::ProfileCaptureRequestSubmission&& submission, std::string_view failure) noexcept {
                if (submission.result != Render::ProfileCaptureSubmitResult::Queued ||
                    !submission.ticket.Valid()) {
                    FailProfileCaptureLifecycleValidation(failure);
                    return false;
                }
                m_profile_capture_validation_ticket = std::move(submission.ticket);
                return true;
            };

        const Render::ProfileCaptureControllerSnapshot snapshot = m_profile_capture_controller->GetSnapshot();
        const Render::RenderProfileCaptureStats        stats    = m_render_profile_capture->GetStats();
        const ProfileDump::RuntimeStats runtime_stats =
            ProfileDump::GetRuntimeStats();
        Render::ProfileCaptureRequestCompletion        completion{};

        switch (m_profile_capture_lifecycle_validation_stage) {
            case EProfileCaptureLifecycleValidationStage::WaitingForFirstGenerationFrames: {
                if (!live_generation_matches(
                        snapshot, stats, m_profile_capture_validation_first_generation
                    )) {
                    FailProfileCaptureLifecycleValidation("generation A left GPU-capable Running state");
                    return;
                }
                if (!m_first_main_present_notified ||
                    stats.frames_emitted < kProfileCaptureValidationMinGpuFrames ||
                    stats.scopes_emitted == 0) {
                    return;
                }

                if (!accept_submission(
                        RequestProfileCaptureStop(m_profile_capture_validation_first_generation),
                        "Stop(A) request was not admitted"
                    )) {
                    return;
                }
                m_profile_capture_lifecycle_validation_stage =
                    EProfileCaptureLifecycleValidationStage::WaitingForFirstStop;
                LOG_INFO(
                    "[ProfileCaptureValidation] generation A ran on GPU "
                    "(generation={}, frames={}, scopes={}); Stop(A) queued after "
                    "the controller tick and must complete on a later GT tick.",
                    m_profile_capture_validation_first_generation,
                    stats.frames_emitted,
                    stats.scopes_emitted
                );
                return;
            }

            case EProfileCaptureLifecycleValidationStage::WaitingForFirstStop: {
                if (!m_profile_capture_validation_ticket.TryGet(completion)) {
                    return;
                }
                if (completion.status != Render::ProfileCaptureCompletionStatus::Stopped ||
                    completion.generation != m_profile_capture_validation_first_generation ||
                    snapshot.state != Render::ProfileCaptureControllerState::Idle || snapshot.owns_runtime ||
                    snapshot.gpu_session_active || !stats.closed ||
                    stats.generation != m_profile_capture_validation_first_generation ||
                    !ProfileDumpTerminalIsClean(
                        runtime_stats,
                        m_profile_capture_validation_first_generation
                    )) {
                    FailProfileCaptureLifecycleValidation(
                        "Stop(A) did not publish a clean terminal generation"
                    );
                    return;
                }

                LOG_INFO(
                    "[ProfileCaptureValidation] Stop(A) terminal on a later GT "
                    "tick: generation={}, status={}, frames={}, scopes={}.",
                    completion.generation,
                    static_cast<unsigned int>(completion.status),
                    stats.frames_emitted,
                    stats.scopes_emitted
                );
                if (!accept_submission(
                        RequestProfileCaptureStart(m_profile_capture_validation_restart_options),
                        "Start(B) request was not admitted"
                    )) {
                    return;
                }
                m_profile_capture_lifecycle_validation_stage =
                    EProfileCaptureLifecycleValidationStage::WaitingForRestart;
                LOG_INFO(
                    "[ProfileCaptureValidation] Start(B) queued after the "
                    "controller tick: output='{}'.",
                    m_profile_capture_validation_restart_options.runtime.output_path.generic_string()
                );
                return;
            }

            case EProfileCaptureLifecycleValidationStage::WaitingForRestart: {
                if (!m_profile_capture_validation_ticket.TryGet(completion)) {
                    return;
                }
                if (completion.status != Render::ProfileCaptureCompletionStatus::Started ||
                    completion.generation <= m_profile_capture_validation_first_generation ||
                    !live_generation_matches(snapshot, stats, completion.generation)) {
                    FailProfileCaptureLifecycleValidation(
                        "Start(B) did not bind a newer GPU-capable generation"
                    );
                    return;
                }

                m_profile_capture_validation_second_generation = completion.generation;
                LOG_INFO(
                    "[ProfileCaptureValidation] Start(B) completed on a later GT "
                    "tick: generation_a={}, generation_b={}, status={}, "
                    "stable_facade={}.",
                    m_profile_capture_validation_first_generation,
                    m_profile_capture_validation_second_generation,
                    static_cast<unsigned int>(completion.status),
                    static_cast<const void*>(m_render_profile_capture.get())
                );
                if (!accept_submission(
                        RequestProfileCaptureStop(m_profile_capture_validation_first_generation),
                        "stale Stop(A) request was not admitted"
                    )) {
                    return;
                }
                m_profile_capture_lifecycle_validation_stage =
                    EProfileCaptureLifecycleValidationStage::WaitingForStaleStop;
                LOG_INFO("[ProfileCaptureValidation] stale Stop(A) queued against "
                         "running generation B; it must be rejected on a later GT tick.");
                return;
            }

            case EProfileCaptureLifecycleValidationStage::WaitingForStaleStop: {
                if (!m_profile_capture_validation_ticket.TryGet(completion)) {
                    return;
                }
                if (completion.status != Render::ProfileCaptureCompletionStatus::RejectedStaleGeneration ||
                    completion.generation != m_profile_capture_validation_second_generation ||
                    !live_generation_matches(
                        snapshot, stats, m_profile_capture_validation_second_generation
                    )) {
                    FailProfileCaptureLifecycleValidation(
                        "stale Stop(A) mutated or failed to protect generation B"
                    );
                    return;
                }

                LOG_INFO(
                    "[ProfileCaptureValidation] stale Stop(A) rejected on a later "
                    "GT tick: protected_generation={}, status={}; generation B "
                    "remains GPU-capable Running.",
                    completion.generation,
                    static_cast<unsigned int>(completion.status)
                );
                m_profile_capture_lifecycle_validation_stage =
                    EProfileCaptureLifecycleValidationStage::WaitingForSecondGenerationFrames;
                return;
            }

            case EProfileCaptureLifecycleValidationStage::WaitingForSecondGenerationFrames: {
                if (!live_generation_matches(
                        snapshot, stats, m_profile_capture_validation_second_generation
                    )) {
                    FailProfileCaptureLifecycleValidation("generation B left GPU-capable Running state");
                    return;
                }
                if (stats.frames_emitted < kProfileCaptureValidationMinGpuFrames ||
                    stats.scopes_emitted == 0) {
                    return;
                }

                if (!accept_submission(
                        RequestProfileCaptureStop(m_profile_capture_validation_second_generation),
                        "Stop(B) request was not admitted"
                    )) {
                    return;
                }
                m_profile_capture_lifecycle_validation_stage =
                    EProfileCaptureLifecycleValidationStage::WaitingForSecondStop;
                LOG_INFO(
                    "[ProfileCaptureValidation] generation B ran on GPU "
                    "(generation={}, frames={}, scopes={}); Stop(B) queued after "
                    "the controller tick.",
                    m_profile_capture_validation_second_generation,
                    stats.frames_emitted,
                    stats.scopes_emitted
                );
                return;
            }

            case EProfileCaptureLifecycleValidationStage::WaitingForSecondStop: {
                if (!m_profile_capture_validation_ticket.TryGet(completion)) {
                    return;
                }
                if (completion.status != Render::ProfileCaptureCompletionStatus::Stopped ||
                    completion.generation != m_profile_capture_validation_second_generation ||
                    snapshot.state != Render::ProfileCaptureControllerState::Idle || snapshot.owns_runtime ||
                    snapshot.gpu_session_active || !stats.closed ||
                    stats.generation != m_profile_capture_validation_second_generation ||
                    stats.frames_emitted < kProfileCaptureValidationMinGpuFrames ||
                    stats.scopes_emitted == 0 ||
                    !ProfileDumpTerminalIsClean(
                        runtime_stats,
                        m_profile_capture_validation_second_generation
                    )) {
                    FailProfileCaptureLifecycleValidation(
                        "Stop(B) did not publish a clean terminal generation"
                    );
                    return;
                }

                m_profile_capture_lifecycle_validation_stage =
                    EProfileCaptureLifecycleValidationStage::Complete;
                LOG_INFO(
                    "[ProfileCaptureValidation][PASS] dynamic GPU capture "
                    "lifecycle completed on the Game Thread: generation_a={}, "
                    "generation_b={}, generation_b_frames={}, "
                    "generation_b_scopes={}, gt_ticks={}, stable_facade={}.",
                    m_profile_capture_validation_first_generation,
                    m_profile_capture_validation_second_generation,
                    stats.frames_emitted,
                    stats.scopes_emitted,
                    m_profile_capture_validation_gt_ticks,
                    static_cast<const void*>(m_render_profile_capture.get())
                );
                if (!m_profile_capture_validation_exit_requested) {
                    m_profile_capture_validation_exit_requested = true;
                    try {
                        RequestExit();
                    } catch (...) {
                    }
                }
                return;
            }

            case EProfileCaptureLifecycleValidationStage::Disabled:
            case EProfileCaptureLifecycleValidationStage::Complete:
            case EProfileCaptureLifecycleValidationStage::Failed:
                return;
        }
    } catch (...) {
        FailProfileCaptureLifecycleValidation("unexpected exception while advancing validation");
    }
}

void Engine::Init(
    int                            argc,
    const char**                   argv,
    bool                           main_window_visible,
    const StartupProgressCallback& on_startup_progress
) {
    static_cast<void>(Platform::InitializeCrashDiagnostics());
    ScopedLogTimer startup_timer("[Startup][Engine] Engine::Init total");

    bool startup_logging_ready = false;
    const auto report_startup = [&](std::string_view title, std::string_view detail) {
        try {
            if (on_startup_progress) {
                on_startup_progress(title, detail);
            }
        } catch (...) {
            // Startup reporting is optional and must never prevent the editor
            // from reaching its normal error handling path.
        }
        if (startup_logging_ready) {
            LOG_INFO("[Startup][Progress] {} - {}", title, detail);
        }
    };

    report_startup("Starting engine core", "Initializing logging and configuration");

    // Init LogSystem
    LogSystem::Init(); // for LOG_DEBUG & LOG_TRACE when debug mode
    startup_logging_ready = true;

    // Init ConfigManager
    std::filesystem::path path = argv[0];
    path = path.filename().string().find(".exe") != std::string::npos ? path.parent_path() : path;

    report_startup("Reading configuration", path.string());
    LOG_INFO("Workspace Path : {}", path.string());

    if (ContainsNonAscii(path)) {
        LOG_ERROR(
            "Workspace Path contains non-ASCII characters (e.g., Chinese characters)! This may cause "
            "unexpected issues. Current path: {}",
            path.string()
        );
    }

    if (const auto config_override = ParseConfigOverride(argc, argv)) {
        ConfigManager::GetInstance().Init(path, *config_override);
    } else {
        ConfigManager::GetInstance().Init(path);
    }
    const auto& config = ConfigManager::GetInstance().GetConfig();
    const uint64_t vulkan_present_submit_fault_trigger =
        ParseVulkanPresentSubmitFaultTrigger(argc, argv);
    const uint64_t parallel_record_worker_throw_trigger =
        ParseParallelRecordWorkerThrowTrigger(argc, argv);
    const bool renderer_switch_validation_enabled =
        ParseThreadingRendererSwitchValidation(argc, argv);
    const bool raster_lifecycle_validation_enabled =
        ParseThreadingRasterLifecycleValidation(argc, argv);
    const auto raster_framebuffer_validation =
        ParseThreadingRasterFramebufferValidation(argc, argv);
    m_profile_capture_lifecycle_validation_enabled =
        ParseProfileCaptureLifecycleValidation(argc, argv);
    const uint submission_batch_window =
        RHISubmissionPipelinePolicy::ClampBatchWindow(
            config.engine.threading.submission_batch_window
        );
    if (submission_batch_window != config.engine.threading.submission_batch_window) {
        LOG_WARNING(
            "[Threading] submission_batch_window={} is outside the supported range; "
            "clamping to {}.",
            config.engine.threading.submission_batch_window,
            submission_batch_window
        );
    }
    if (renderer_switch_validation_enabled && raster_lifecycle_validation_enabled) {
        throw std::invalid_argument(
            "renderer-switch and Raster lifecycle validation modes are mutually exclusive"
        );
    }
    if (m_profile_capture_lifecycle_validation_enabled &&
        (renderer_switch_validation_enabled ||
         raster_lifecycle_validation_enabled ||
         raster_framebuffer_validation.has_value())) {
        throw std::invalid_argument(
            "profile-capture lifecycle validation is mutually exclusive with "
            "threading Raster validation modes"
        );
    }
    if ((renderer_switch_validation_enabled || raster_lifecycle_validation_enabled ||
         raster_framebuffer_validation.has_value()) &&
        config.engine.render.default_render_method != "Raster") {
        throw std::invalid_argument(
            "threading Raster validation requires "
            "engine.render.default_render_method = \"Raster\""
        );
    }
    if (m_profile_capture_lifecycle_validation_enabled &&
        !config.engine.profile_dump.enabled) {
        throw std::invalid_argument(
            "--profile-capture-lifecycle-validation requires "
            "engine.profile_dump.enabled = true"
        );
    }

    m_console_control = MakeUnique<EngineConsoleControl>(
        EngineConsoleStartupConfig{
            .render_thread = config.engine.threading.render_thread,
            .rhi_thread = config.engine.threading.rhi_thread,
            .rhi_bypass = config.engine.threading.rhi_bypass,
            .profile_logging = config.engine.threading.profile_logging,
            .parallel_recording =
                config.engine.threading.parallel_recording,
            .parallel_record_workers =
                config.engine.threading.parallel_record_workers,
            .parallel_record_verify =
                config.engine.threading.parallel_record_verify,
            .parallel_record_profile =
                config.engine.threading.parallel_record_profile,
            .parallel_record_min_work_units_per_job =
                config.engine.threading.parallel_record_min_work_units_per_job,
            .configured_submission_batch_window =
                config.engine.threading.submission_batch_window,
            .rhi_heartbeat_enabled =
                config.engine.threading.rhi_heartbeat_enabled,
            .rhi_heartbeat_stall_timeout_ms =
                config.engine.threading.rhi_heartbeat_stall_timeout_ms,
            .rhi_heartbeat_poll_interval_ms =
                config.engine.threading.rhi_heartbeat_poll_interval_ms,
            .max_frame_lag = config.engine.threading.max_frame_lag,
            .max_frames_in_flight = config.engine.rhi.max_frame_in_flight,
            .raster_rdg_enabled =
                config.engine.render.raster.render_graph,
            .raster_rdg_debug_dump =
                config.engine.render.raster.render_graph_debug_dump,
            .raster_rdg_parallel_recording =
                config.engine.render.raster.render_graph_parallel_recording,
            .raytracing_rdg_enabled =
                config.engine.render.raytracing.render_graph,
            .raytracing_rdg_debug_dump =
                config.engine.render.raytracing.render_graph_debug_dump,
            .raytracing_rdg_parallel_recording =
                config.engine.render.raytracing.render_graph_parallel_recording,
        },
        submission_batch_window
    );
    CVar::SealStartupOnlyCVars();

    try {
        // Renderers retain this facade pointer for their whole lifetime.
        // Individual ProfileDump generations bind and unbind session state
        // inside the facade; the pointer itself never changes.
        m_render_profile_capture = MakeUnique<Render::RenderProfileCapture>();
    } catch (...) {
        m_render_profile_capture.reset();
        try {
            LOG_WARNING("[ProfileDump] Failed to allocate the stable GPU capture "
                        "facade; GPU capture will remain disabled.");
        } catch (...) {
        }
    }
    try {
        m_profile_capture_controller =
            MakeUnique<Render::ProfileCaptureController>(m_render_profile_capture.get());
    } catch (...) {
        m_profile_capture_controller.reset();
        try {
            LOG_WARNING(
                "[ProfileDump] Failed to allocate the Engine-owned capture controller; "
                "dynamic capture will remain disabled."
            );
        } catch (...) {
        }
    }
    InitializeProfileDump();
    InitializeProfileCaptureLifecycleValidation();

    // Init TaskSystem
    report_startup("Starting worker services", "Creating task-graph worker threads");
    TaskSystem::Init();
    m_task_system_initialized = true;

    LOG_INFO("[Threading] GameThread id = {}", GetGameThreadId());
    LOG_INFO(
        "[Threading] render_thread={}, rhi_thread={}, rhi_bypass={}, max_frame_lag={}, "
        "profile_logging={}, parallel_recording={}, parallel_record_workers={}, "
        "parallel_record_verify={}, parallel_record_profile={}, "
        "parallel_record_min_work_units_per_job={}, submission_batch_window={}, "
        "rhi_heartbeat_enabled={}, rhi_heartbeat_stall_timeout_ms={}, "
        "rhi_heartbeat_poll_interval_ms={}",
        config.engine.threading.render_thread,
        config.engine.threading.rhi_thread,
        config.engine.threading.rhi_bypass,
        config.engine.threading.max_frame_lag,
        config.engine.threading.profile_logging,
        config.engine.threading.parallel_recording,
        config.engine.threading.parallel_record_workers,
        config.engine.threading.parallel_record_verify,
        config.engine.threading.parallel_record_profile,
        config.engine.threading.parallel_record_min_work_units_per_job,
        submission_batch_window,
        config.engine.threading.rhi_heartbeat_enabled,
        config.engine.threading.rhi_heartbeat_stall_timeout_ms,
        config.engine.threading.rhi_heartbeat_poll_interval_ms
    );

    if (config.engine.threading.render_thread) {
        report_startup("Starting render thread", "Creating the configured render-thread service");
        m_max_frame_lag = std::min(config.engine.threading.max_frame_lag, uint{1});
        if (config.engine.threading.max_frame_lag > 1) {
            LOG_WARNING(
                "[Threading] max_frame_lag={} exceeds the validated range; clamping to 1.",
                config.engine.threading.max_frame_lag
            );
        }

        m_render_thread_service = MakeUnique<RenderThreadService>();
        m_render_thread_service->Start();
        LOG_INFO("[Threading] Effective Render Thread max_frame_lag={}", m_max_frame_lag);
    } else if (config.engine.threading.max_frame_lag != 0) {
        LOG_WARNING("[Threading] max_frame_lag is ignored while render_thread=false.");
    }

    // Init RenderDevice
    std::string rhi_type_str = config.engine.rhi.type;
    std::transform(rhi_type_str.begin(), rhi_type_str.end(), rhi_type_str.begin(), ::tolower);

    ERHIType rhi_type = [&]() {
        if (rhi_type_str == "vulkan") {
            LOG_INFO("Using Vulkan as RHI backend");
            return ERHIType::Vulkan;
        }
        if (rhi_type_str == "d3d12") {
            LOG_INFO("Using D3D12 as RHI backend");
            return ERHIType::D3D12;
        }

        LOG_WARNING("Unknown RHI type '{}', fallback to Vulkan", config.engine.rhi.type);
        return ERHIType::Vulkan;
    }();

    report_startup(
        "Initializing graphics device",
        rhi_type == ERHIType::Vulkan ? "Creating the Vulkan device, queues, and descriptors" :
                                       "Creating the D3D12 device, queues, and descriptors"
    );
    // Dispose is safe when initialization leaves a partially constructed
    // implementation behind, so arm cleanup before entering the backend.
    m_render_device_initialized = true;
    RenderDevice::Init(
        std::move(
            DeviceInitInfo{
                .rhi_type        = rhi_type,
                .name            = "MoerEngine",
                .rhi_api_version = config.engine.rhi.api_version,
                .rhi_thread              = config.engine.threading.rhi_thread,
                .rhi_bypass              = config.engine.threading.rhi_bypass,
                .thread_profile_logging  = config.engine.threading.profile_logging,
                .parallel_recording      = config.engine.threading.parallel_recording,
                .parallel_record_workers = config.engine.threading.parallel_record_workers,
                .parallel_record_verify  = config.engine.threading.parallel_record_verify,
                .parallel_record_profile = config.engine.threading.parallel_record_profile,
                .parallel_record_min_work_units_per_job =
                    config.engine.threading.parallel_record_min_work_units_per_job,
                .submission_batch_window = submission_batch_window,
                .rhi_heartbeat_enabled =
                    config.engine.threading.rhi_heartbeat_enabled,
                .rhi_heartbeat_stall_timeout_ms =
                    config.engine.threading.rhi_heartbeat_stall_timeout_ms,
                .rhi_heartbeat_poll_interval_ms =
                    config.engine.threading.rhi_heartbeat_poll_interval_ms,
                .parallel_record_worker_throw_trigger =
                    parallel_record_worker_throw_trigger,
                .vulkan_present_submit_fault_trigger = vulkan_present_submit_fault_trigger,
            }
        )
    );

    report_startup("Loading shader cache", "Restoring compiled shaders and pipeline metadata");
    ShaderManager::Get(); // Explicit Init ShaderManager
    m_shader_manager_initialized = true;

    m_editor_config = MakeShared<EditorConfig>();
    m_editor_config->selected_render_method =
        ParseDefaultRenderMethod(config.engine.render.default_render_method);
    m_editor_config->scene_path = config.engine.scene.scene_path;
    m_console_control->BindEditorConfig(*m_editor_config);
    if (raster_framebuffer_validation.has_value()) {
        m_editor_config->validation_selected_frame_buffer_name =
            *raster_framebuffer_validation;
    }
    if (renderer_switch_validation_enabled) {
        m_renderer_switch_validation_stage = ERendererSwitchValidationStage::InitialRaster;
        LOG_INFO(
            "[ThreadingValidation][RendererSwitch] Enabled; sequence=Raster reload, "
            "Raster->Raytracing->Raster; "
            "stable_ready_gt_frames=12."
        );
    }
    if (raster_lifecycle_validation_enabled) {
        m_raster_lifecycle_validation_enabled = true;
        LOG_INFO(
            "[ThreadingValidation][RasterLifecycle] Enabled; stable_ready_gt_frames=12."
        );
    }

    // Init WindowContext
    m_editor_config->SetResolution(config.editor.width, config.editor.height);
    bool b_fullscreen = config.editor.fullscreen;
    LOG_INFO(
        "Editor Window Resolution : {}x{}; Fullscreen : {}",
        m_editor_config->GetResolution().x,
        m_editor_config->GetResolution().y,
        b_fullscreen
    );

    report_startup(
        "Creating editor window",
        main_window_visible ? "Preparing the main window" : "Keeping the main window hidden until its first frame"
    );
    WindowContext::Init(SurfaceInitInfo(
        m_editor_config->GetResolution().x,
        m_editor_config->GetResolution().y,
        "MoerEditor",
        b_fullscreen,
        main_window_visible
    ));
    m_window_context_initialized = true;
    m_main_window_surface        = WindowContext::CreateSwapchainSurfaceInfo(*WindowContext::GetMainWindow());
    if (!m_main_window_surface.IsValid()) {
        throw std::runtime_error("Failed to capture the main window surface source");
    }

    report_startup("Loading editor resources", "Uploading editor textures and environment assets");
    m_runtime_assets =
        MakeUnique<RuntimeAssets>(ConfigManager::GetInstance().GetEditorResourcePath(), RenderDevice::Get());
    m_runtime_assets->WaitUntilReady();
    LOG_INFO("[Threading] RuntimeAssets are immutable-ready before renderer/UI frame overlap begins.");

    report_startup("Starting editor services", "Initializing Python scripting and remote control");
    m_script_host = MakeUnique<scripting::ScriptHost>(scripting::PythonRuntimeConfig::Default());
    m_script_host->Start();

    // 初始化 RemoteModule
    auto remote_config = remote::MakeRemoteConfigFromGlobalConfig(config);
    const bool remote_enabled_by_config = remote_config.enable;
    auto       submit_fn                = [this](scripting::ScriptExecutionRequest request) {
        return SubmitScriptExecution(std::move(request));
    };

    m_remote_module = MakeUnique<remote::RemoteModule>(std::move(remote_config), std::move(submit_fn));
    if (remote_enabled_by_config && !m_remote_module->SetEnabled(true)) {
        LOG_WARNING("Remote module failed to start. Continue running without remote access.");
    }
    report_startup("Engine core ready", "Preparing the editor interface");
}

void Engine::Run(const EngineHooks& hooks) {
    EngineHooks runtime_hooks = hooks;
    auto        on_first_main_present = std::move(runtime_hooks.on_first_main_present);
    runtime_hooks.on_first_main_present =
        [this, callback = std::move(on_first_main_present)]() mutable {
            assert(IsCurrentlyGameThread());
            if (m_first_main_present_notified) {
                return;
            }
            m_first_main_present_notified = true;
            LOG_INFO("[Startup][Engine] First main-window present submitted on the Game Thread.");
            if (callback) {
                try {
                    callback();
                } catch (...) {
                    LOG_WARNING("[Startup][Engine] Ignoring an exception from the first-present hook.");
                }
            }
        };
    const bool thread_profile_logging =
        ConfigManager::GetInstance().GetConfig().engine.threading.profile_logging;
    auto on_tick_engine_control = std::move(runtime_hooks.on_tick_engine_control);
    runtime_hooks.on_tick_engine_control =
        [this, callback = std::move(on_tick_engine_control)]() {
            Render::ProfileCaptureTickResult capture_tick_result =
                Render::ProfileCaptureTickResult::Shutdown;
            if (m_profile_capture_controller) {
                capture_tick_result =
                    m_profile_capture_controller->TickOwner();
            }
            if (m_profile_capture_lifecycle_validation_enabled) {
                TickProfileCaptureLifecycleValidation(capture_tick_result);
            }
            if (callback) {
                callback();
            }
        };
    auto on_tick_ui = std::move(runtime_hooks.on_tick_ui);
    runtime_hooks.on_tick_ui =
        [this, callback = std::move(on_tick_ui)](Scene& scene) {
            if (callback) {
                callback(scene);
            }
            if (m_console_control && m_editor_config) {
                static_cast<void>(
                    m_console_control->TickGameThread(*m_editor_config)
                );
            }
        };
    runtime_hooks.on_tick_scripting = [this](Scene& scene) {
        if (m_script_host) {
            m_script_host->ProcessMainThreadCommands(scene);
        }
    };
    if (m_renderer_switch_validation_stage != ERendererSwitchValidationStage::Disabled) {
        auto on_tick_test = std::move(runtime_hooks.on_tick_test);
        runtime_hooks.on_tick_test =
            [this, on_tick_test = std::move(on_tick_test)](Scene& scene) {
                if (on_tick_test) {
                    on_tick_test(scene);
                }
                TickRendererSwitchValidation(scene);
            };

        auto base_should_reload = std::move(runtime_hooks.should_reload);
        runtime_hooks.should_reload =
            [this, base_should_reload = std::move(base_should_reload)]() {
                const bool hook_requested = base_should_reload && base_should_reload();
                const bool validation_requested =
                    ConsumeRendererSwitchValidationReloadRequest();
                return hook_requested || validation_requested;
            };
    } else if (m_raster_lifecycle_validation_enabled) {
        auto on_tick_test = std::move(runtime_hooks.on_tick_test);
        runtime_hooks.on_tick_test =
            [this, on_tick_test = std::move(on_tick_test)](Scene& scene) {
                if (on_tick_test) {
                    on_tick_test(scene);
                }
                TickRasterLifecycleValidation(scene);
            };
    }

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        const ERenderMethod selected_render_method = m_editor_config->selected_render_method;

        LOG_INFO(
            "Selecting Render Method : {}",
            k_render_method_names[static_cast<uint>(selected_render_method)]
        );

        auto create_renderer = [this, runtime_hooks, selected_render_method]() {
            if (selected_render_method == ERenderMethod::Raster) {
                m_renderer = MakeUnique<Raster::RasterRenderer>(
                    m_editor_config->GetResolution(),
                    m_editor_config,
                    m_main_window_surface,
                    m_render_profile_capture.get()
                );

            } else if (selected_render_method == ERenderMethod::Raytracing) {
                m_renderer = MakeUnique<Raytracing::RaytracingRenderer>(
                    m_editor_config->GetResolution(),
                    m_editor_config,
                    m_main_window_surface,
                    *m_runtime_assets,
                    m_render_profile_capture.get()
                );

            } else {
                MOER_ASSERT(
                    false,
                    "Unknown render method during renderer creation: {}",
                    static_cast<std::uint32_t>(
                        m_editor_config->selected_render_method
                    )
                );
            }
        };

        const bool use_render_thread = m_render_thread_service != nullptr;

        if (runtime_hooks.on_startup_progress && !m_first_main_present_notified) {
            runtime_hooks.on_startup_progress(
                "Loading scene and renderer",
                selected_render_method == ERenderMethod::Raster ?
                    "Loading the scene and creating Raster pipelines" :
                    "Loading the scene and creating Raytracing pipelines"
            );
        }

        if (use_render_thread) {
            m_render_thread_service->RunAndWait(std::move(create_renderer));
            MOER_ASSERT(
                m_renderer &&
                    m_renderer->SupportsSynchronizedRenderThread(),
                "Render-thread mode requires a synchronized renderer"
            );

            if (runtime_hooks.on_startup_progress && !m_first_main_present_notified) {
                runtime_hooks.on_startup_progress(
                    "Preparing first frame", "Uploading scene data and warming the renderer"
                );
            }

            if (runtime_hooks.on_show_config_sub_ui) {
                runtime_hooks.on_show_config_sub_ui();
            }

            if (selected_render_method == ERenderMethod::Raster) {
                auto* raster_renderer = static_cast<Raster::RasterRenderer*>(m_renderer.get());
                RunBoundedRenderLoop(
                    *m_render_thread_service,
                    m_max_frame_lag,
                    thread_profile_logging,
                    [this, raster_renderer, &runtime_hooks]() {
                        return raster_renderer->PrepareFrame(m_editor_config, runtime_hooks);
                    },
                    [raster_renderer](Raster::RasterFramePacket frame_packet) {
                        return raster_renderer->RenderFrame(std::move(frame_packet));
                    },
                    [this, raster_renderer, &runtime_hooks](Raster::RasterFrameFeedback feedback) {
                        raster_renderer->ApplyFrameFeedback(
                            std::move(feedback), m_editor_config->raster_config, runtime_hooks
                        );
                    },
                    [&runtime_hooks]() {
                        return runtime_hooks.should_reload && runtime_hooks.should_reload();
                    }
                );

            } else if (selected_render_method == ERenderMethod::Raytracing) {
                auto* raytracing_renderer = static_cast<Raytracing::RaytracingRenderer*>(m_renderer.get());
                RunBoundedRenderLoop(
                    *m_render_thread_service,
                    m_max_frame_lag,
                    thread_profile_logging,
                    [this, raytracing_renderer, &runtime_hooks]() {
                        return raytracing_renderer->PrepareFrame(m_editor_config, runtime_hooks);
                    },
                    [raytracing_renderer](Raytracing::RaytracingFramePacket frame_packet) {
                        return raytracing_renderer->RenderFrame(std::move(frame_packet));
                    },
                    [this, raytracing_renderer, &runtime_hooks](
                        Raytracing::RaytracingFrameFeedback feedback
                    ) {
                        raytracing_renderer->ApplyFrameFeedback(
                            std::move(feedback),
                            m_editor_config->raytracing_config,
                            runtime_hooks
                        );
                    },
                    [&runtime_hooks]() {
                        return runtime_hooks.should_reload && runtime_hooks.should_reload();
                    }
                );
                raytracing_renderer->Shutdown(runtime_hooks);

            } else {
                MOER_ASSERT(
                    false,
                    "Unknown render method during renderer shutdown: {}",
                    static_cast<std::uint32_t>(
                        m_editor_config->selected_render_method
                    )
                );
            }
        } else {
            create_renderer();
            if (runtime_hooks.on_startup_progress && !m_first_main_present_notified) {
                runtime_hooks.on_startup_progress(
                    "Preparing first frame", "Uploading scene data and warming the renderer"
                );
            }
            if (runtime_hooks.on_show_config_sub_ui) {
                runtime_hooks.on_show_config_sub_ui();
            }
            if (selected_render_method == ERenderMethod::Raytracing) {
                auto* raytracing_renderer =
                    static_cast<Raytracing::RaytracingRenderer*>(m_renderer.get());
                while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
                    auto frame_packet =
                        raytracing_renderer->PrepareFrame(m_editor_config, runtime_hooks);
                    raytracing_renderer->ApplyFrameFeedback(
                        raytracing_renderer->RenderFrame(std::move(frame_packet)),
                        m_editor_config->raytracing_config,
                        runtime_hooks
                    );

                    if (runtime_hooks.should_reload && runtime_hooks.should_reload()) {
                        break;
                    }
                }
                raytracing_renderer->Shutdown(runtime_hooks);
            } else {
                m_renderer->Run(m_editor_config, runtime_hooks);
            }
        }

        if (m_script_host) {
            m_script_host->CancelPendingSceneCommands(
                "Scene became unavailable during renderer switch or shutdown."
            );
        }

        // Switch Renderer
        if (use_render_thread) {
            m_render_thread_service->RunAndWait([this]() {
                LOG_INFO("[Threading] Destroying renderer on Render Thread.");
                m_renderer.reset();
                LOG_INFO("[Threading] Renderer destroyed on Render Thread.");
            });
        } else {
            m_renderer.reset();
        }
    }
}

void Engine::TickRendererSwitchValidation(Scene& scene) {
    assert(IsCurrentlyGameThread());

    constexpr uint k_stable_ready_gt_frames = 12;

    ERenderMethod expected_render_method = ERenderMethod::Raster;
    switch (m_renderer_switch_validation_stage) {
        case ERendererSwitchValidationStage::InitialRaster:
        case ERendererSwitchValidationStage::ReloadedRaster:
        case ERendererSwitchValidationStage::FinalRaster:
            expected_render_method = ERenderMethod::Raster;
            break;
        case ERendererSwitchValidationStage::Raytracing:
            expected_render_method = ERenderMethod::Raytracing;
            break;
        case ERendererSwitchValidationStage::Disabled:
        case ERendererSwitchValidationStage::Complete:
        case ERendererSwitchValidationStage::Failed:
            return;
    }

    if (m_editor_config->selected_render_method != expected_render_method) {
        LOG_ERROR(
            "[ThreadingValidation][RendererSwitch][Error] Expected renderer {}, observed {}.",
            k_render_method_names[static_cast<uint>(expected_render_method)],
            k_render_method_names[static_cast<uint>(m_editor_config->selected_render_method)]
        );
        m_renderer_switch_validation_stage = ERendererSwitchValidationStage::Failed;
        m_renderer_switch_validation_reload_requested = false;
        WindowContext::RequestClose(WindowContext::GetMainWindow());
        return;
    }

    if (!scene.IsReady()) {
        m_renderer_switch_validation_ready_frames = 0;
        return;
    }

    ++m_renderer_switch_validation_ready_frames;
    if (m_renderer_switch_validation_ready_frames < k_stable_ready_gt_frames) {
        return;
    }
    m_renderer_switch_validation_ready_frames = 0;

    switch (m_renderer_switch_validation_stage) {
        case ERendererSwitchValidationStage::InitialRaster:
            LOG_INFO(
                "[ThreadingValidation][RendererSwitch] Request Raster reload after {} ready GT "
                "frames.",
                k_stable_ready_gt_frames
            );
            m_renderer_switch_validation_stage = ERendererSwitchValidationStage::ReloadedRaster;
            m_renderer_switch_validation_reload_requested = true;
            break;
        case ERendererSwitchValidationStage::ReloadedRaster:
            LOG_INFO(
                "[ThreadingValidation][RendererSwitch] Request Raster->Raytracing reload after {} "
                "ready GT frames.",
                k_stable_ready_gt_frames
            );
            m_editor_config->selected_render_method = ERenderMethod::Raytracing;
            m_renderer_switch_validation_stage = ERendererSwitchValidationStage::Raytracing;
            m_renderer_switch_validation_reload_requested = true;
            break;
        case ERendererSwitchValidationStage::Raytracing:
            LOG_INFO(
                "[ThreadingValidation][RendererSwitch] Request Raytracing->Raster reload after {} "
                "ready GT frames.",
                k_stable_ready_gt_frames
            );
            m_editor_config->selected_render_method = ERenderMethod::Raster;
            m_renderer_switch_validation_stage = ERendererSwitchValidationStage::FinalRaster;
            m_renderer_switch_validation_reload_requested = true;
            break;
        case ERendererSwitchValidationStage::FinalRaster:
            m_renderer_switch_validation_stage = ERendererSwitchValidationStage::Complete;
            LOG_INFO(
                "[ThreadingValidation][RendererSwitch] Complete: final Raster stable for {} ready "
                "GT frames.",
                k_stable_ready_gt_frames
            );
            WindowContext::RequestClose(WindowContext::GetMainWindow());
            break;
        case ERendererSwitchValidationStage::Disabled:
        case ERendererSwitchValidationStage::Complete:
        case ERendererSwitchValidationStage::Failed:
            break;
    }
}

void Engine::TickRasterLifecycleValidation(Scene& scene) {
    assert(IsCurrentlyGameThread());
    constexpr uint k_stable_ready_gt_frames = 12;

    if (!m_raster_lifecycle_validation_enabled) {
        return;
    }
    if (!scene.IsReady()) {
        m_raster_lifecycle_validation_ready_frames = 0;
        return;
    }
    ++m_raster_lifecycle_validation_ready_frames;
    if (m_raster_lifecycle_validation_ready_frames < k_stable_ready_gt_frames) {
        return;
    }

    m_raster_lifecycle_validation_enabled = false;
    LOG_INFO(
        "[ThreadingValidation][RasterLifecycle] Complete: Raster stable for {} ready GT frames.",
        k_stable_ready_gt_frames
    );
    WindowContext::RequestClose(WindowContext::GetMainWindow());
}

bool Engine::ConsumeRendererSwitchValidationReloadRequest() {
    assert(IsCurrentlyGameThread());
    const bool requested = m_renderer_switch_validation_reload_requested;
    m_renderer_switch_validation_reload_requested = false;
    return requested;
}

void Engine::RequestExit() {
    WindowContext::RequestClose(WindowContext::GetMainWindow());
}

remote::RemoteModuleController Engine::GetRemoteModuleController() const {
    if (!m_remote_module) {
        return remote::RemoteModuleController();
    }

    return m_remote_module->GetController();
}

scripting::ScriptExecutionFuture Engine::SubmitScriptExecution(scripting::ScriptExecutionRequest request) {
    if (m_script_host) {
        return m_script_host->Submit(std::move(request));
    }

    std::promise<scripting::ScriptExecutionResult> promise;
    scripting::ScriptExecutionResult               result;
    result.exception_text = "ScriptHost is not available.";
    promise.set_value(std::move(result));
    return scripting::ScriptExecutionFuture(promise.get_future());
}

Render::ProfileCaptureRequestSubmission
Engine::RequestProfileCaptureStart(Render::ProfileCaptureStartOptions options) noexcept {
    if (!m_profile_capture_controller) {
        return {
            .result = Render::ProfileCaptureSubmitResult::AdmissionClosed,
            .ticket = {},
        };
    }
    return m_profile_capture_controller->RequestStart(std::move(options));
}

Render::ProfileCaptureRequestSubmission
Engine::RequestProfileCaptureStop(std::uint64_t expected_generation) noexcept {
    if (!m_profile_capture_controller) {
        return {
            .result = Render::ProfileCaptureSubmitResult::AdmissionClosed,
            .ticket = {},
        };
    }
    return m_profile_capture_controller->RequestStop(expected_generation);
}

Render::ProfileCaptureControllerSnapshot Engine::GetProfileCaptureSnapshot() const noexcept {
    if (m_profile_capture_controller) {
        return m_profile_capture_controller->GetSnapshot();
    }
    Render::ProfileCaptureControllerSnapshot snapshot;
    snapshot.state              = Render::ProfileCaptureControllerState::Shutdown;
    snapshot.accepting_requests = false;
    return snapshot;
}

SharedPtr<EngineCommandEndpoint> Engine::GetCommandEndpoint() const {
    if (!m_console_control) {
        throw std::logic_error(
            "Engine command processor is unavailable before Engine::Init or after shutdown."
        );
    }
    return m_console_control->GetCommandEndpoint();
}

void Engine::ShutDown() noexcept {
    if (m_has_shutdown) {
        return;
    }
    if (m_profile_capture_lifecycle_validation_enabled &&
        m_profile_capture_lifecycle_validation_stage !=
            EProfileCaptureLifecycleValidationStage::Complete &&
        m_profile_capture_lifecycle_validation_stage !=
            EProfileCaptureLifecycleValidationStage::Failed &&
        m_profile_capture_lifecycle_validation_stage !=
            EProfileCaptureLifecycleValidationStage::Disabled) {
        FailProfileCaptureLifecycleValidation(
            "Engine shutdown began before validation reached PASS"
        );
    }
    m_has_shutdown = true;

    const auto cleanup = []<typename Action>(Action&& action) noexcept {
        try {
            std::forward<Action>(action)();
        } catch (...) {
            // Shutdown is also the rollback path for partial startup. Continue
            // releasing later subsystems even if one cleanup operation fails.
        }
    };
    bool capture_shutdown_trace_enabled =
        m_profile_capture_lifecycle_validation_enabled;
    if (m_profile_capture_controller) {
        const Render::ProfileCaptureControllerSnapshot snapshot =
            m_profile_capture_controller->GetSnapshot();
        capture_shutdown_trace_enabled =
            capture_shutdown_trace_enabled || snapshot.owns_runtime ||
            snapshot.active_generation != 0;
    }
    const auto report_capture_boundary =
        [this, capture_shutdown_trace_enabled](
            std::string_view                     boundary,
            Render::ProfileCaptureLifecycleResult result
        ) noexcept {
            if (result == Render::ProfileCaptureLifecycleResult::Advanced ||
                result == Render::ProfileCaptureLifecycleResult::AlreadyAtBoundary) {
                if (capture_shutdown_trace_enabled &&
                    m_profile_capture_controller) {
                    const Render::ProfileCaptureControllerSnapshot snapshot =
                        m_profile_capture_controller->GetSnapshot();
                    try {
                        LOG_INFO(
                            "[ProfileCapture][Shutdown] boundary='{}' complete "
                            "(result={}, controller_state={}, generation={}, "
                            "owns_runtime={}, gpu_session_active={}).",
                            boundary,
                            static_cast<unsigned int>(result),
                            static_cast<unsigned int>(snapshot.state),
                            snapshot.active_generation,
                            snapshot.owns_runtime,
                            snapshot.gpu_session_active
                        );
                    } catch (...) {
                    }
                }
                return;
            }
            try {
                LOG_WARNING(
                    "[ProfileCapture][Shutdown] Capture controller failed the "
                    "'{}' boundary "
                    "(result={}); its generation-guarded destructor fallback will run.",
                    boundary,
                    static_cast<unsigned int>(result)
                );
            } catch (...) {
            }
        };

    if (m_profile_capture_controller) {
        report_capture_boundary(
            "request-admission-before-renderer",
            m_profile_capture_controller->BeginEngineShutdown()
        );
    }

    if (m_remote_module) {
        cleanup([this]() { m_remote_module->Stop(); });
        m_remote_module.reset();
    }

    if (m_script_host) {
        cleanup([this]() {
            m_script_host->CancelPendingSceneCommands(
                "Scene became unavailable during engine shutdown."
            );
            m_script_host->Stop();
        });
        m_script_host.reset();
    }

    if (m_renderer && m_render_thread_service) {
        cleanup([this]() {
            m_render_thread_service->RunAndWait([this]() { m_renderer.reset(); });
        });
    }
    // RunAndWait can itself fail while unwinding a render-thread exception.
    // Destruction on the Game Thread is the final shutdown fallback.
    m_renderer.reset();

    if (m_render_thread_service) {
        cleanup([this]() {
            LOG_INFO("[Threading] Stopping Render Thread service.");
            m_render_thread_service->Stop();
            LOG_INFO("[Threading] Render Thread service stopped.");
        });
        m_render_thread_service.reset();
    }

    m_runtime_assets.reset(); // 释放RuntimeAssets资源
    m_main_window_surface = {};

    if (m_window_context_initialized) {
        m_window_context_initialized = false;
        cleanup([]() { WindowContext::ShutDown(); });
    }
    if (m_shader_manager_initialized) {
        m_shader_manager_initialized = false;
        cleanup([]() { ShaderManager::ShutDown(); });
    }
    bool rhi_drained = !m_render_device_initialized;
    if (m_render_device_initialized) {
        m_render_device_initialized = false;
        try {
            RenderDevice::Dispose();
            rhi_drained = true;
        } catch (...) {
            rhi_drained = false;
        }
    }
    if (m_profile_capture_controller) {
        report_capture_boundary(
            rhi_drained ? "gpu-finalization-after-rhi-drain" :
                          "gpu-abort-after-rhi-drain-failure",
            m_profile_capture_controller->FinalizeGpuAfterRhiDrain(rhi_drained)
        );
    }
    bool task_workers_joined = !m_task_system_initialized;
    if (m_task_system_initialized) {
        m_task_system_initialized = false;
        if (capture_shutdown_trace_enabled) {
            try {
                LOG_INFO(
                    "[ProfileCapture][Shutdown] TaskSystem worker shutdown "
                    "begins after the GPU post-RHI boundary."
                );
            } catch (...) {
            }
        }
        try {
            TaskSystem::ShutDown();
            task_workers_joined = true;
        } catch (...) {
            task_workers_joined = false;
        }
    }
    if (capture_shutdown_trace_enabled) {
        try {
            if (task_workers_joined) {
                LOG_INFO(
                    "[ProfileCapture][Shutdown] TaskSystem workers joined; "
                    "ProfileDump runtime finalization may begin."
                );
            } else {
                LOG_WARNING(
                    "[ProfileCapture][Shutdown] TaskSystem worker shutdown "
                    "failed before ProfileDump runtime finalization."
                );
            }
        } catch (...) {
        }
    }
    if (m_profile_capture_controller) {
        if (task_workers_joined) {
            report_capture_boundary(
                "profiledump-finalization-after-workers",
                m_profile_capture_controller->FinalizeRuntimeAfterWorkers()
            );
        } else {
            report_capture_boundary(
                "profiledump-abandoned-after-worker-failure",
                m_profile_capture_controller
                    ->AbandonRuntimeAfterWorkerShutdownFailure()
            );
        }
    }
    m_console_control.reset();
    m_editor_config.reset();
}

// 检测路径中是否包含非ASCII字符（包括中文）
bool ContainsNonAscii(const std::filesystem::path& p) {
    // std::filesystem::path 内部存储可能是 wchar_t (Windows) 或 char (其他平台)
    // 转换为 std::string (UTF-8) 或 std::wstring 进行检测更通用

    // 在Windows上，std::filesystem::path::string() 会根据当前 locale 转换为 narrow string
    // 但为了可靠检测非ASCII字符，最好是转换为宽字符串再检查，或者确保转换为UTF-8

    // 方法1：转换为 UTF-8 string 并检查 (更通用，但依赖std::codecvt_utf8_utf16)
    // std::string utf8_path_str = p.u8string(); // C++17，直接获取UTF-8编码
    // for (unsigned char c : utf8_path_str) {
    //     if (c > 127) { // 检查是否为非ASCII字符
    //         return true;
    //     }
    // }
    // return false;

    // 方法2：直接检查宽字符串 (更适合Windows，因为内部存储可能是宽字符)
    // 假设 std::filesystem::path 内部是 wchar_t 或可以转换为 wchar_t
    std::wstring wide_path_str = p.generic_wstring(); // 获取宽字符串表示

    for (wchar_t wc : wide_path_str) {
        // ASCII字符的 wchar_t 值范围是 0-127
        if (wc > 127) {
            return true; // 发现非ASCII字符
        }
    }
    return false;
}

} // namespace Moer
