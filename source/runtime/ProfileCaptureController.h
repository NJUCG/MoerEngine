#pragma once

#include "profile/ProfileDump.h"
#include "profile/RenderProfileCapture.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace Moer::Render {

enum class ProfileCaptureControllerState : std::uint8_t {
    Idle = 0,
    Starting,
    Running,
    StoppingGpu,
    FinalizingRuntime,
    AwaitingRhiDrain,
    AwaitingWorkerShutdown,
    Shutdown,
};

enum class ProfileCaptureRequestKind : std::uint8_t {
    Start = 0,
    Stop,
};

enum class ProfileCaptureSubmitResult : std::uint8_t {
    Queued = 0,
    AdmissionClosed,
    QueueFull,
    InvalidArgument,
    ResourceExhausted,
};

enum class ProfileCaptureCompletionStatus : std::uint8_t {
    Pending = 0,
    Started,
    StartedCpuOnly,
    Stopped,
    StoppedWithLoss,
    RejectedAdmissionClosed,
    RejectedQueueFull,
    RejectedResourceExhausted,
    RejectedInvalidArgument,
    RejectedAlreadyRunning,
    RejectedStaleGeneration,
    RejectedExternalRuntime,
    CancelledForShutdown,
    RuntimeStartFailed,
    CpuSchemaFailed,
    GpuFrameSchemaFailed,
    GpuScopeSchemaFailed,
    CpuActivationFailed,
    GpuSessionStartFailed,
    RuntimeOwnershipLost,
    GpuSessionFaulted,
    GpuSessionAborted,
    RuntimeShutdownFaulted,
    ControllerDestroyed,
};

enum class ProfileCaptureTickResult : std::uint8_t {
    Idle = 0,
    Progressed,
    WaitingForGpu,
    WrongThread,
    Shutdown,
};

enum class ProfileCaptureLifecycleResult : std::uint8_t {
    Advanced = 0,
    AlreadyAtBoundary,
    WrongThread,
    InvalidState,
};

// The two detail fields retain the raw subsystem result values for telemetry.
// Their interpretation is selected by status and remains optional to callers.
struct ProfileCaptureRequestCompletion {
    std::uint64_t                  request_id{0};
    std::uint64_t                  generation{0};
    ProfileCaptureRequestKind      kind{ProfileCaptureRequestKind::Start};
    ProfileCaptureCompletionStatus status{ProfileCaptureCompletionStatus::Pending};
    std::uint32_t                  detail{0};
    std::uint32_t                  secondary_detail{0};
};

namespace ProfileCaptureControllerDetail {
struct CompletionState;
}

class ProfileCaptureRequestTicket final {
public:
    ProfileCaptureRequestTicket() noexcept = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] bool Ready() const noexcept;

    // Returns false while the request is pending or when this is an empty
    // ticket. A terminal completion is immutable and may be polled from any
    // thread.
    [[nodiscard]] bool TryGet(ProfileCaptureRequestCompletion& _completion) const noexcept;

private:
    explicit ProfileCaptureRequestTicket(
        std::shared_ptr<ProfileCaptureControllerDetail::CompletionState> _state
    ) noexcept;

    std::shared_ptr<ProfileCaptureControllerDetail::CompletionState> state_{};

    friend class ProfileCaptureController;
};

struct ProfileCaptureRequestSubmission {
    ProfileCaptureSubmitResult result{ProfileCaptureSubmitResult::ResourceExhausted};
    ProfileCaptureRequestTicket ticket{};
};

struct ProfileCaptureStartOptions {
    ProfileDump::RuntimeConfig runtime{};
    GpuScopeStreamConfig       gpu_stream{};
};

struct ProfileCaptureControllerSnapshot {
    ProfileCaptureControllerState state{ProfileCaptureControllerState::Idle};
    bool                          accepting_requests{true};
    bool                          owns_runtime{false};
    bool                          cpu_producer_active{false};
    bool                          gpu_session_active{false};
    std::size_t                   queued_requests{0};
    std::size_t                   queue_capacity{0};
    std::uint64_t                 active_generation{0};
    std::uint64_t                 last_completed_request_id{0};
    ProfileCaptureCompletionStatus last_completion_status{
        ProfileCaptureCompletionStatus::Pending
    };
    std::uint64_t requests_submitted{0};
    std::uint64_t requests_accepted{0};
    std::uint64_t requests_rejected{0};
    std::uint64_t requests_completed{0};
    std::uint64_t wrong_thread_calls{0};
};

// Game Thread owns all capture lifecycle transitions. RequestStart and
// RequestStop are bounded MPSC admission points; tickets hold their own
// completion state, so the controller never grows a result map.
//
// While this controller owns a ProfileDump generation, every ProfileDump and
// CpuScopeProducer lifecycle operation must be serialized through it on the
// Game Thread. External concurrent Shutdown/Deactivate calls violate the
// process-owner contract; ProfileDump Core does not currently expose an owner
// token that could enforce this boundary itself.
class ProfileCaptureController final {
public:
    static constexpr std::size_t kDefaultRequestCapacity = 64;

    explicit ProfileCaptureController(
        RenderProfileCapture* _capture,
        std::size_t           _request_capacity = kDefaultRequestCapacity
    ) noexcept;
    ~ProfileCaptureController() noexcept;

    ProfileCaptureController(const ProfileCaptureController&)            = delete;
    ProfileCaptureController& operator=(const ProfileCaptureController&) = delete;
    ProfileCaptureController(ProfileCaptureController&&)                 = delete;
    ProfileCaptureController& operator=(ProfileCaptureController&&)      = delete;

    [[nodiscard]] ProfileCaptureRequestSubmission
    RequestStart(ProfileCaptureStartOptions _options) noexcept;

    // A zero generation is never a wildcard and is rejected at admission.
    [[nodiscard]] ProfileCaptureRequestSubmission
    RequestStop(std::uint64_t _expected_generation) noexcept;

    [[nodiscard]] ProfileCaptureTickResult TickOwner() noexcept;

    // Engine shutdown is intentionally split around the RHI and worker joins:
    // Begin closes request/CPU/GPU admission only; it does not close the
    // bridge or ProfileDump runtime.
    [[nodiscard]] ProfileCaptureLifecycleResult BeginEngineShutdown() noexcept;
    [[nodiscard]] ProfileCaptureLifecycleResult
    FinalizeGpuAfterRhiDrain(bool _rhi_drained) noexcept;
    [[nodiscard]] ProfileCaptureLifecycleResult FinalizeRuntimeAfterWorkers() noexcept;
    // Last-resort Engine failure path. If worker quiescence cannot be proven,
    // relinquish controller ownership without touching ProfileDump again.
    // This intentionally leaves the process-global runtime alive so an
    // in-flight worker scope cannot race an unsafe FlushAll/Shutdown, and it
    // also prevents the controller destructor from retrying finalization.
    [[nodiscard]] ProfileCaptureLifecycleResult
    AbandonRuntimeAfterWorkerShutdownFailure() noexcept;

    [[nodiscard]] ProfileCaptureControllerSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] bool                             IsOwnerThread() const noexcept;

private:
    struct Request {
        ProfileCaptureRequestKind kind{ProfileCaptureRequestKind::Start};
        ProfileCaptureStartOptions start_options{};
        std::uint64_t expected_generation{0};
        std::shared_ptr<ProfileCaptureControllerDetail::CompletionState> completion{};
    };

    [[nodiscard]] ProfileCaptureRequestSubmission Submit(Request&& _request) noexcept;
    [[nodiscard]] bool                            TryPopRequest(Request& _request) noexcept;
    [[nodiscard]] ProfileCaptureTickResult        ProcessQueuedRequests() noexcept;
    void                                          ProcessStart(Request&& _request) noexcept;
    [[nodiscard]] bool                            ProcessStop(Request&& _request) noexcept;
    [[nodiscard]] ProfileCaptureTickResult        PollGpuStop() noexcept;
    void                                          FinalizeDynamicRuntime(
                                                 RenderProfileSessionFinishResult _gpu_result
                                             ) noexcept;

    void RollbackOwnedStart() noexcept;
    [[nodiscard]] bool OwnsPublishedGeneration() const noexcept;
    [[nodiscard]] bool OnOwnerThread() noexcept;
    void PublishOwnerState() noexcept;
    void Complete(
        const std::shared_ptr<ProfileCaptureControllerDetail::CompletionState>& _completion,
        ProfileCaptureCompletionStatus                                           _status,
        std::uint64_t                                                            _generation,
        std::uint32_t                                                            _detail = 0,
        std::uint32_t _secondary_detail = 0
    ) noexcept;
    void CancelQueued(ProfileCaptureCompletionStatus _status) noexcept;

    RenderProfileCapture* const capture_{nullptr};
    const std::thread::id       owner_thread_;
    const std::size_t           request_capacity_{0};
    mutable std::mutex request_mutex_{};
    // Protected by request_mutex_. Assigning the id and appending an accepted
    // request share one linearization point, so accepted ids strictly follow
    // FIFO order even when producers race.
    std::uint64_t next_request_id_{1};
    std::deque<Request> requests_{};
    ProfileCaptureControllerSnapshot published_{};

    ProfileCaptureControllerState state_{ProfileCaptureControllerState::Idle};
    std::uint64_t                  active_generation_{0};
    bool                           owns_runtime_{false};
    bool                           cpu_producer_active_{false};
    bool                           gpu_session_active_{false};
    bool                           accepting_requests_{true};

    std::shared_ptr<ProfileCaptureControllerDetail::CompletionState>
        active_stop_completion_{};
    RenderProfileSessionFinishResult shutdown_gpu_result_{
        RenderProfileSessionFinishResult::Inactive
    };
};

} // namespace Moer::Render
