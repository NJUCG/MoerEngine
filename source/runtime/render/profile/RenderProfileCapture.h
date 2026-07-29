#ifndef MOER_ENGINE_RENDER_PROFILE_CAPTURE_H
#define MOER_ENGINE_RENDER_PROFILE_CAPTURE_H

#include "RenderAPI.h"
#include "profile/ProfileDump.h"
#include "rhi/RHIGpuScope.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Moer::Render {

class CommandList;

namespace RenderProfileDetail {
struct CaptureState;
}

enum class GpuFrameCaptureStatus : std::uint32_t {
    Complete = 0,
    Incomplete,
    Invalid,
};

enum class RenderProfileBindResult : std::uint8_t {
    Bound = 0,
    Inactive,
    InvalidFrame,
    InvalidQueue,
    SourceRejected,
    CommandListRejected,
};

enum class RenderProfileSessionStartResult : std::uint8_t {
    Started = 0,
    AlreadyActive,
    GenerationAlreadyUsed,
    InvalidSchema,
    RuntimeUnavailable,
    StaleGeneration,
    InvalidConfiguration,
    ResourceExhausted,
};

enum class RenderProfileSessionStopResult : std::uint8_t {
    StopRequested = 0,
    AlreadyStopping,
    Inactive,
};

enum class RenderProfileSessionFinishResult : std::uint8_t {
    Pending = 0,
    Closed,
    // The session drained and closed, but at least one frame, source, scope,
    // or serialized record was rejected or dropped.
    ClosedWithLoss,
    Faulted,
    Aborted,
    Inactive,
};

struct RenderProfileCaptureStats {
    bool          accepting{false};
    bool          closed{false};
    std::uint64_t generation{0};

    std::uint64_t frames_attempted{0};
    std::uint64_t frames_admitted{0};
    std::uint64_t frames_admission_rejected{0};
    std::uint64_t frames_sealed{0};
    std::uint64_t frames_emitted{0};
    std::uint64_t frames_invalid{0};
    std::uint64_t frame_records_dropped{0};

    std::uint64_t sources_bound{0};
    std::uint64_t sources_rejected{0};
    std::uint64_t scopes_emitted{0};
    std::uint64_t scope_records_dropped{0};
    std::uint64_t terminal_faults{0};
    std::uint64_t shutdown_abandoned_frames{0};

    GpuScopeStreamStats stream{};
};

class RENDER_API RenderProfileFrameToken {
public:
    RenderProfileFrameToken() = default;
    ~RenderProfileFrameToken();

    RenderProfileFrameToken(const RenderProfileFrameToken&)            = delete;
    RenderProfileFrameToken& operator=(const RenderProfileFrameToken&) = delete;
    RenderProfileFrameToken(RenderProfileFrameToken&& _other) noexcept;
    RenderProfileFrameToken& operator=(RenderProfileFrameToken&& _other) noexcept;

    [[nodiscard]] bool          Valid() const noexcept;
    [[nodiscard]] std::uint64_t FrameId() const noexcept;

private:
    RenderProfileFrameToken(
        std::shared_ptr<RenderProfileDetail::CaptureState> _state,
        GpuScopeFrameHandle                                _frame,
        std::uint64_t                                      _frame_id
    ) noexcept;

    void SealIfNeeded() noexcept;

    std::shared_ptr<RenderProfileDetail::CaptureState> state_{};
    GpuScopeFrameHandle                                frame_{};
    std::uint64_t                                      frame_id_{0};
    bool                                               sealed_{false};

    friend class RenderProfileCapture;
};

class RENDER_API RenderProfileCapture final {
public:
    // Stable facade. One instance may bind to successive ProfileDump
    // generations, but never to two sessions in the same generation.
    RenderProfileCapture() noexcept = default;
    // Compatibility constructor: invalid stream configuration and allocation
    // failure retain the throwing behavior of the original one-shot facade.
    RenderProfileCapture(
        ProfileDump::SchemaHandle   _gpu_frame_schema,
        ProfileDump::SchemaHandle   _gpu_scope_schema,
        const GpuScopeStreamConfig& _stream_config = {}
    );
    ~RenderProfileCapture();

    RenderProfileCapture(const RenderProfileCapture&)            = delete;
    RenderProfileCapture& operator=(const RenderProfileCapture&) = delete;
    RenderProfileCapture(RenderProfileCapture&&)                 = delete;
    RenderProfileCapture& operator=(RenderProfileCapture&&)      = delete;

    [[nodiscard]] RenderProfileSessionStartResult StartSession(
        ProfileDump::SchemaHandle   _gpu_frame_schema,
        ProfileDump::SchemaHandle   _gpu_scope_schema,
        const GpuScopeStreamConfig& _stream_config = {}
    ) noexcept;

    // Stops only future BeginFrame admission. Tokens admitted before the
    // boundary remain valid and may still bind sources and seal.
    [[nodiscard]] RenderProfileSessionStopResult RequestStop() noexcept;

    // Stops admission, drains ready frames, and closes only after every
    // admitted frame has left the resident stream. Pending callbacks retain
    // the old session state and can never enter a later generation.
    [[nodiscard]] RenderProfileSessionFinishResult TryFinishSession() noexcept;

    [[nodiscard]] bool                    Valid() const noexcept;
    [[nodiscard]] RenderProfileFrameToken BeginFrame() noexcept;
    [[nodiscard]] RenderProfileBindResult BindSource(
        RenderProfileFrameToken& _frame,
        CommandList&             _command_list,
        RHIQueueBinding          _queue_binding,
        std::uint64_t            _source_order
    ) noexcept;
    [[nodiscard]] bool Seal(RenderProfileFrameToken& _frame) noexcept;

    // Consumer-only operation. It materializes ready frames in BeginFrame
    // order and emits ProfileDump records on the calling thread. Completion
    // callbacks never enter this path.
    [[nodiscard]] std::size_t DrainReadyFrames() noexcept;

    // Normal terminal boundary. Call only after RHI Executor/Submission/
    // Completion have stopped and joined.
    void ShutdownAfterRhiDrain() noexcept;

    // Exceptional terminal boundary. It is non-blocking and never presents
    // pending frames as completed.
    void Abort() noexcept;

    [[nodiscard]] RenderProfileCaptureStats GetStats() const noexcept;

private:
    std::atomic<std::shared_ptr<RenderProfileDetail::CaptureState>> state_{};
};

#if defined(MOER_RENDER_PROFILE_CAPTURE_TEST_HOOKS)
namespace RenderProfileTesting {
// Deterministic seams for exceptions that can escape first-use construction
// of the lazy ProfileDump schema descriptors. Production builds do not
// declare or compile these injectors.
RENDER_API void InjectNextStartValidationBadAlloc() noexcept;
RENDER_API void InjectNextStartValidationException() noexcept;
RENDER_API void ClearStartValidationException() noexcept;

RENDER_API void
InstallStartPublishPause(std::atomic_uint32_t& _entered_count, std::atomic_bool& _release) noexcept;
RENDER_API void ClearStartPublishPause() noexcept;
} // namespace RenderProfileTesting
#endif

} // namespace Moer::Render

#endif // MOER_ENGINE_RENDER_PROFILE_CAPTURE_H
