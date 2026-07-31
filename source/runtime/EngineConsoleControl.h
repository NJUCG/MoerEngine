#pragma once

#include "RenderAPI.h"
#include "command/EngineCommandProcessor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace Moer {

struct EditorConfig;

struct EngineConsoleStartupConfig {
    bool          render_thread                          = false;
    bool          rhi_thread                             = false;
    bool          rhi_bypass                             = true;
    bool          profile_logging                        = false;
    bool          parallel_recording                     = false;
    std::uint32_t parallel_record_workers                = 0;
    bool          parallel_record_verify                 = false;
    bool          parallel_record_profile                = false;
    std::uint32_t parallel_record_min_work_units_per_job = 64;
    std::uint32_t configured_submission_batch_window     = 2;
    bool          rhi_heartbeat_enabled                  = false;
    std::uint32_t rhi_heartbeat_stall_timeout_ms         = 5000;
    std::uint32_t rhi_heartbeat_poll_interval_ms         = 1000;
    std::uint32_t max_frame_lag                          = 0;
    std::uint32_t max_frames_in_flight                   = 0;

    bool raster_rdg_enabled                = false;
    bool raster_rdg_debug_dump             = false;
    bool raster_rdg_parallel_recording     = false;
    bool raytracing_rdg_enabled            = false;
    bool raytracing_rdg_debug_dump         = false;
    bool raytracing_rdg_parallel_recording = false;
};

// Stable producer/reader endpoint shared by Engine and Editor. Closing
// admission is synchronized with in-flight calls, while the underlying Core
// processor remains alive until the final endpoint owner releases it.
class RENDER_API EngineCommandEndpoint {
public:
    explicit EngineCommandEndpoint(Command::EngineCommandProcessorLimits limits = {});
    ~EngineCommandEndpoint();

    EngineCommandEndpoint(const EngineCommandEndpoint&)            = delete;
    EngineCommandEndpoint& operator=(const EngineCommandEndpoint&) = delete;
    EngineCommandEndpoint(EngineCommandEndpoint&&)                 = delete;
    EngineCommandEndpoint& operator=(EngineCommandEndpoint&&)      = delete;

    [[nodiscard]] Command::ESubmitStatus SubmitText(std::string_view text);
    [[nodiscard]] Command::CommandOutputBatch
    PollOutput(std::uint64_t next_sequence = 1, std::size_t max_count = 256) const;
    [[nodiscard]] std::vector<Command::CommandCandidate>
                       GetCandidates(std::string_view input, std::size_t max_count = 64) const;
    [[nodiscard]] bool IsAccepting() const noexcept;

private:
    [[nodiscard]] Command::ProcessPendingResult ProcessPending(std::size_t max_commands);
    void                                        CloseAdmission() noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl;

    friend class EngineConsoleControl;
};

// Runtime-owned console endpoint. Command submission may come from any thread,
// but command execution and EditorConfig mutation are serialized by
// TickGameThread.
class RENDER_API EngineConsoleControl {
public:
    EngineConsoleControl(
        const EngineConsoleStartupConfig& config,
        unsigned int                      policy_clamped_submission_batch_window
    );
    ~EngineConsoleControl();

    EngineConsoleControl(const EngineConsoleControl&)            = delete;
    EngineConsoleControl& operator=(const EngineConsoleControl&) = delete;
    EngineConsoleControl(EngineConsoleControl&&)                 = delete;
    EngineConsoleControl& operator=(EngineConsoleControl&&)      = delete;

    void BindEditorConfig(EditorConfig& config);
    void UnbindEditorConfig() noexcept;

    [[nodiscard]] std::size_t TickGameThread(EditorConfig& config, std::size_t max_commands = 64);

    [[nodiscard]] std::shared_ptr<EngineCommandEndpoint> GetCommandEndpoint() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Moer
