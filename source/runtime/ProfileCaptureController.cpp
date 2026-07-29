#include "ProfileCaptureController.h"

#include "profile/ProfileDumpTemplates.h"
#include "profile/ProfileScope.h"

#include <limits>
#include <utility>

namespace Moer::Render {

namespace ProfileCaptureControllerDetail {

struct CompletionState {
    explicit CompletionState(ProfileCaptureRequestKind _kind) noexcept
        : kind(_kind) {}

    // Written once while request_mutex_ owns the submission linearization
    // point, before either the queued request or rejected ticket is published.
    std::uint64_t                   request_id{0};
    const ProfileCaptureRequestKind kind{ProfileCaptureRequestKind::Start};
    std::atomic_uint64_t            generation{0};
    std::atomic_uint32_t            detail{0};
    std::atomic_uint32_t            secondary_detail{0};
    std::atomic_flag                completion_claimed = ATOMIC_FLAG_INIT;
    std::atomic<ProfileCaptureCompletionStatus> status{
        ProfileCaptureCompletionStatus::Pending
    };
};

} // namespace ProfileCaptureControllerDetail

namespace {

template <typename Enum>
[[nodiscard]] constexpr std::uint32_t Raw(Enum _value) noexcept {
    return static_cast<std::uint32_t>(_value);
}

[[nodiscard]] bool RegistrationUsable(
    const ProfileDump::SchemaRegistration& _registration,
    std::uint64_t                           _generation
) noexcept {
    return (_registration.status == ProfileDump::SchemaStatus::Registered ||
            _registration.status == ProfileDump::SchemaStatus::AlreadyRegistered) &&
           static_cast<bool>(_registration.handle) &&
           _registration.handle.generation == _generation;
}

[[nodiscard]] ProfileCaptureCompletionStatus
StopCompletionStatus(RenderProfileSessionFinishResult _result) noexcept {
    switch (_result) {
        case RenderProfileSessionFinishResult::Closed:
            return ProfileCaptureCompletionStatus::Stopped;
        case RenderProfileSessionFinishResult::ClosedWithLoss:
            return ProfileCaptureCompletionStatus::StoppedWithLoss;
        case RenderProfileSessionFinishResult::Aborted:
            return ProfileCaptureCompletionStatus::GpuSessionAborted;
        case RenderProfileSessionFinishResult::Faulted:
        case RenderProfileSessionFinishResult::Inactive:
        case RenderProfileSessionFinishResult::Pending:
            return ProfileCaptureCompletionStatus::GpuSessionFaulted;
    }
    return ProfileCaptureCompletionStatus::GpuSessionFaulted;
}

[[nodiscard]] bool
RuntimeHasDroppedRecords(const ProfileDump::RuntimeStats& _stats) noexcept {
    return _stats.records_dropped_stopped != 0 ||
           _stats.records_dropped_stale_generation != 0 ||
           _stats.records_dropped_oversized != 0 ||
           _stats.records_dropped_queue_full != 0 ||
           _stats.records_dropped_after_fault != 0;
}

[[nodiscard]] ProfileCaptureCompletionStatus AccountRuntimeFinalization(
    ProfileCaptureCompletionStatus  _status,
    ProfileDump::FlushResult         _flush_result,
    ProfileDump::ShutdownResult      _shutdown_result,
    const ProfileDump::RuntimeStats& _pre_shutdown_stats,
    std::uint64_t                    _generation
) noexcept {
    const ProfileDump::RuntimeStats post_shutdown_stats =
        ProfileDump::GetRuntimeStats();
    if (_pre_shutdown_stats.generation != _generation ||
        post_shutdown_stats.generation != _generation ||
        _shutdown_result == ProfileDump::ShutdownResult::AlreadyStopped) {
        return ProfileCaptureCompletionStatus::RuntimeOwnershipLost;
    }
    if (_flush_result == ProfileDump::FlushResult::Faulted ||
        _shutdown_result == ProfileDump::ShutdownResult::Faulted ||
        _pre_shutdown_stats.last_fault != ProfileDump::RuntimeFault::None ||
        post_shutdown_stats.last_fault != ProfileDump::RuntimeFault::None ||
        post_shutdown_stats.state != ProfileDump::RuntimeState::Stopped) {
        return ProfileCaptureCompletionStatus::RuntimeShutdownFaulted;
    }

    const bool runtime_loss =
        _flush_result == ProfileDump::FlushResult::Rejected ||
        RuntimeHasDroppedRecords(post_shutdown_stats);
    if (_status == ProfileCaptureCompletionStatus::Stopped && runtime_loss) {
        return ProfileCaptureCompletionStatus::StoppedWithLoss;
    }
    return _status;
}

} // namespace

ProfileCaptureRequestTicket::ProfileCaptureRequestTicket(
    std::shared_ptr<ProfileCaptureControllerDetail::CompletionState> _state
) noexcept
    : state_(std::move(_state)) {}

bool ProfileCaptureRequestTicket::Valid() const noexcept {
    return static_cast<bool>(state_);
}

bool ProfileCaptureRequestTicket::Ready() const noexcept {
    return state_ &&
           state_->status.load(std::memory_order_acquire) !=
               ProfileCaptureCompletionStatus::Pending;
}

bool ProfileCaptureRequestTicket::TryGet(
    ProfileCaptureRequestCompletion& _completion
) const noexcept {
    if (!state_) {
        return false;
    }

    const ProfileCaptureCompletionStatus status =
        state_->status.load(std::memory_order_acquire);
    if (status == ProfileCaptureCompletionStatus::Pending) {
        return false;
    }

    _completion.request_id       = state_->request_id;
    _completion.generation       = state_->generation.load(std::memory_order_relaxed);
    _completion.kind             = state_->kind;
    _completion.status           = status;
    _completion.detail           = state_->detail.load(std::memory_order_relaxed);
    _completion.secondary_detail =
        state_->secondary_detail.load(std::memory_order_relaxed);
    return true;
}

ProfileCaptureController::ProfileCaptureController(
    RenderProfileCapture* _capture,
    std::size_t           _request_capacity
) noexcept
    : capture_(_capture),
      owner_thread_(std::this_thread::get_id()),
      request_capacity_(_request_capacity) {
    published_.state              = state_;
    published_.accepting_requests = accepting_requests_;
    published_.queue_capacity     = request_capacity_;
}

ProfileCaptureController::~ProfileCaptureController() noexcept {
    {
        std::lock_guard lock(request_mutex_);
        accepting_requests_           = false;
        published_.accepting_requests = false;
    }

    CancelQueued(ProfileCaptureCompletionStatus::ControllerDestroyed);
    if (active_stop_completion_) {
        Complete(
            active_stop_completion_,
            ProfileCaptureCompletionStatus::ControllerDestroyed,
            active_generation_
        );
        active_stop_completion_.reset();
    }

    // Emergency fallback only. The normal Engine path must call the three
    // owner-thread shutdown boundaries in order. If that path was skipped,
    // retire only the exact generation acquired by this controller; never
    // mutate a facade or ProfileDump runtime that has since been rebound.
    if (owns_runtime_ && active_generation_ != 0) {
        const std::uint64_t owned_generation = active_generation_;
        if (gpu_session_active_ && capture_ != nullptr) {
            const RenderProfileCaptureStats stats = capture_->GetStats();
            if (stats.generation == owned_generation) {
                capture_->Abort();
            }
        }

        if (ProfileDump::GetRuntimeGeneration() == owned_generation) {
            if (cpu_producer_active_) {
                ProfileDump::CpuScopeProducer::Deactivate();
            }
            static_cast<void>(ProfileDump::FlushAll());
            static_cast<void>(ProfileDump::Shutdown());
        }
    }

    owns_runtime_        = false;
    cpu_producer_active_ = false;
    gpu_session_active_  = false;
    active_generation_   = 0;
    state_               = ProfileCaptureControllerState::Shutdown;
    PublishOwnerState();
}

ProfileCaptureRequestSubmission ProfileCaptureController::RequestStart(
    ProfileCaptureStartOptions _options
) noexcept {
    Request request{};
    request.kind          = ProfileCaptureRequestKind::Start;
    request.start_options = std::move(_options);
    return Submit(std::move(request));
}

ProfileCaptureRequestSubmission
ProfileCaptureController::RequestStop(std::uint64_t _expected_generation) noexcept {
    Request request{};
    request.kind                = ProfileCaptureRequestKind::Stop;
    request.expected_generation = _expected_generation;
    return Submit(std::move(request));
}

ProfileCaptureRequestSubmission
ProfileCaptureController::Submit(Request&& _request) noexcept {
    ProfileCaptureRequestSubmission submission{};

    std::shared_ptr<ProfileCaptureControllerDetail::CompletionState> completion;
    try {
        completion =
            std::make_shared<ProfileCaptureControllerDetail::CompletionState>(
                _request.kind
            );
    } catch (...) {
        submission.result = ProfileCaptureSubmitResult::ResourceExhausted;
        return submission;
    }

    submission.ticket = ProfileCaptureRequestTicket(completion);
    _request.completion = completion;

    ProfileCaptureCompletionStatus rejected_status =
        ProfileCaptureCompletionStatus::Pending;
    try {
        std::lock_guard lock(request_mutex_);
        ++published_.requests_submitted;

        // Id assignment is part of the same critical section as FIFO
        // admission. A producer that enters this section first receives the
        // lower id, and an accepted request is appended before the lock is
        // released. Complete must remain outside this lock.
        completion->request_id = next_request_id_++;
        if (next_request_id_ == 0) {
            next_request_id_ = 1;
        }

        if (_request.kind == ProfileCaptureRequestKind::Stop &&
            _request.expected_generation == 0) {
            submission.result = ProfileCaptureSubmitResult::InvalidArgument;
            rejected_status =
                ProfileCaptureCompletionStatus::RejectedInvalidArgument;
        } else if (!accepting_requests_) {
            submission.result = ProfileCaptureSubmitResult::AdmissionClosed;
            rejected_status =
                ProfileCaptureCompletionStatus::RejectedAdmissionClosed;
        } else if (requests_.size() >= request_capacity_) {
            submission.result = ProfileCaptureSubmitResult::QueueFull;
            rejected_status = ProfileCaptureCompletionStatus::RejectedQueueFull;
        } else {
            try {
                requests_.emplace_back(std::move(_request));
                submission.result = ProfileCaptureSubmitResult::Queued;
                ++published_.requests_accepted;
                published_.queued_requests = requests_.size();
                return submission;
            } catch (...) {
                submission.result = ProfileCaptureSubmitResult::ResourceExhausted;
                rejected_status =
                    ProfileCaptureCompletionStatus::RejectedResourceExhausted;
            }
        }
        ++published_.requests_rejected;
    } catch (...) {
        submission.result = ProfileCaptureSubmitResult::ResourceExhausted;
        // A lock acquisition failure has no valid linearization point and
        // therefore cannot publish a well-formed non-zero ticket id.
        submission.ticket = {};
        return submission;
    }

    // Never call Complete while request_mutex_ is held: it publishes the
    // controller's completion counters under that same mutex.
    Complete(completion, rejected_status, 0);
    return submission;
}

bool ProfileCaptureController::TryPopRequest(Request& _request) noexcept {
    try {
        std::lock_guard lock(request_mutex_);
        if (requests_.empty()) {
            return false;
        }
        _request = std::move(requests_.front());
        requests_.pop_front();
        published_.queued_requests = requests_.size();
        return true;
    } catch (...) {
        return false;
    }
}

ProfileCaptureTickResult ProfileCaptureController::TickOwner() noexcept {
    if (!OnOwnerThread()) {
        return ProfileCaptureTickResult::WrongThread;
    }

    switch (state_) {
        case ProfileCaptureControllerState::Idle:
        case ProfileCaptureControllerState::Running:
            return ProcessQueuedRequests();
        case ProfileCaptureControllerState::StoppingGpu:
            return PollGpuStop();
        case ProfileCaptureControllerState::Starting:
        case ProfileCaptureControllerState::FinalizingRuntime:
            return ProfileCaptureTickResult::Progressed;
        case ProfileCaptureControllerState::AwaitingRhiDrain:
        case ProfileCaptureControllerState::AwaitingWorkerShutdown:
            return ProfileCaptureTickResult::WaitingForGpu;
        case ProfileCaptureControllerState::Shutdown:
            return ProfileCaptureTickResult::Shutdown;
    }
    return ProfileCaptureTickResult::Idle;
}

ProfileCaptureTickResult ProfileCaptureController::ProcessQueuedRequests() noexcept {
    bool              progressed = false;
    const std::size_t budget     = request_capacity_ == 0 ? 1 : request_capacity_;

    for (std::size_t index = 0; index < budget; ++index) {
        Request request{};
        if (!TryPopRequest(request)) {
            return progressed ? ProfileCaptureTickResult::Progressed :
                                ProfileCaptureTickResult::Idle;
        }
        progressed = true;

        if (state_ == ProfileCaptureControllerState::Idle) {
            if (request.kind == ProfileCaptureRequestKind::Start) {
                ProcessStart(std::move(request));
            } else {
                Complete(
                    request.completion,
                    ProfileCaptureCompletionStatus::RejectedStaleGeneration,
                    0
                );
            }
            continue;
        }

        if (state_ != ProfileCaptureControllerState::Running) {
            // Only an asynchronous GPU stop may block FIFO consumption.
            return ProfileCaptureTickResult::WaitingForGpu;
        }

        if (request.kind == ProfileCaptureRequestKind::Start) {
            Complete(
                request.completion,
                ProfileCaptureCompletionStatus::RejectedAlreadyRunning,
                active_generation_
            );
            continue;
        }

        if (!ProcessStop(std::move(request))) {
            continue;
        }

        const ProfileCaptureTickResult stop_result = PollGpuStop();
        if (stop_result == ProfileCaptureTickResult::WaitingForGpu) {
            return stop_result;
        }
    }

    return progressed ? ProfileCaptureTickResult::Progressed :
                        ProfileCaptureTickResult::Idle;
}

void ProfileCaptureController::ProcessStart(Request&& _request) noexcept {
    state_ = ProfileCaptureControllerState::Starting;
    PublishOwnerState();

    const ProfileDump::StartResult start_result =
        ProfileDump::Start(_request.start_options.runtime);
    if (start_result != ProfileDump::StartResult::Started) {
        state_ = ProfileCaptureControllerState::Idle;
        PublishOwnerState();
        Complete(
            _request.completion,
            start_result == ProfileDump::StartResult::AlreadyRunning ?
                ProfileCaptureCompletionStatus::RejectedExternalRuntime :
                ProfileCaptureCompletionStatus::RuntimeStartFailed,
            0,
            Raw(start_result)
        );
        return;
    }

    owns_runtime_      = true;
    active_generation_ = ProfileDump::GetRuntimeGeneration();
    PublishOwnerState();

    ProfileCaptureCompletionStatus failure_status =
        ProfileCaptureCompletionStatus::CpuSchemaFailed;
    std::uint32_t failure_detail = std::numeric_limits<std::uint32_t>::max();
    const std::uint64_t started_generation = active_generation_;

    try {
        const ProfileDump::SchemaRegistration cpu_scope =
            ProfileDump::RegisterSchema(ProfileDump::Templates::CpuScope());
        if (!RegistrationUsable(cpu_scope, active_generation_)) {
            failure_detail = Raw(cpu_scope.status);
            RollbackOwnedStart();
            Complete(
                _request.completion,
                ProfileCaptureCompletionStatus::CpuSchemaFailed,
                started_generation,
                failure_detail
            );
            return;
        }

        ProfileDump::SchemaRegistration gpu_frame{};
        ProfileDump::SchemaRegistration gpu_scope{};
        if (capture_ != nullptr) {
            failure_status = ProfileCaptureCompletionStatus::GpuFrameSchemaFailed;
            gpu_frame =
                ProfileDump::RegisterSchema(ProfileDump::Templates::GpuFrame());
            if (!RegistrationUsable(gpu_frame, active_generation_)) {
                failure_detail = Raw(gpu_frame.status);
                RollbackOwnedStart();
                Complete(
                    _request.completion,
                    failure_status,
                    started_generation,
                    failure_detail
                );
                return;
            }

            failure_status = ProfileCaptureCompletionStatus::GpuScopeSchemaFailed;
            gpu_scope =
                ProfileDump::RegisterSchema(ProfileDump::Templates::GpuScope());
            if (!RegistrationUsable(gpu_scope, active_generation_)) {
                failure_detail = Raw(gpu_scope.status);
                RollbackOwnedStart();
                Complete(
                    _request.completion,
                    failure_status,
                    started_generation,
                    failure_detail
                );
                return;
            }
        }

        failure_status = ProfileCaptureCompletionStatus::CpuActivationFailed;
        const ProfileDump::CpuScopeActivationResult activation_result =
            ProfileDump::CpuScopeProducer::Activate(cpu_scope.handle);
        if (activation_result != ProfileDump::CpuScopeActivationResult::Activated) {
            failure_detail = Raw(activation_result);
            RollbackOwnedStart();
            Complete(
                _request.completion,
                failure_status,
                started_generation,
                failure_detail
            );
            return;
        }
        cpu_producer_active_ = true;

        if (capture_ != nullptr) {
            // The bridge is the final transactional step. Any failure on a
            // GPU-capable controller rolls back the whole owned generation;
            // a null facade is an intentional CPU-only session instead.
            failure_status = ProfileCaptureCompletionStatus::GpuSessionStartFailed;
            const RenderProfileSessionStartResult gpu_start_result =
                capture_->StartSession(
                    gpu_frame.handle,
                    gpu_scope.handle,
                    _request.start_options.gpu_stream
                );
            if (gpu_start_result != RenderProfileSessionStartResult::Started) {
                failure_detail = Raw(gpu_start_result);
                RollbackOwnedStart();
                Complete(
                    _request.completion,
                    failure_status,
                    started_generation,
                    failure_detail
                );
                return;
            }
            gpu_session_active_ = true;
        }
    } catch (...) {
        RollbackOwnedStart();
        Complete(
            _request.completion,
            failure_status,
            started_generation,
            failure_detail
        );
        return;
    }

    state_ = ProfileCaptureControllerState::Running;
    PublishOwnerState();
    Complete(
        _request.completion,
        capture_ != nullptr ? ProfileCaptureCompletionStatus::Started :
                              ProfileCaptureCompletionStatus::StartedCpuOnly,
        active_generation_
    );
}

bool ProfileCaptureController::ProcessStop(Request&& _request) noexcept {
    if (!owns_runtime_ || _request.expected_generation != active_generation_) {
        Complete(
            _request.completion,
            ProfileCaptureCompletionStatus::RejectedStaleGeneration,
            active_generation_
        );
        return false;
    }

    if (!OwnsPublishedGeneration()) {
        if (capture_ != nullptr) {
            const RenderProfileCaptureStats stats = capture_->GetStats();
            if (stats.generation == active_generation_ && !stats.closed) {
                static_cast<void>(capture_->RequestStop());
            }
        }

        const std::uint64_t lost_generation = active_generation_;
        owns_runtime_                       = false;
        cpu_producer_active_                = false;
        gpu_session_active_                 = false;
        active_generation_                  = 0;
        state_                              = ProfileCaptureControllerState::Idle;
        PublishOwnerState();
        Complete(
            _request.completion,
            ProfileCaptureCompletionStatus::RuntimeOwnershipLost,
            lost_generation
        );
        return false;
    }

    if (cpu_producer_active_) {
        ProfileDump::CpuScopeProducer::Deactivate();
        cpu_producer_active_ = false;
    }

    if (capture_ != nullptr) {
        const RenderProfileCaptureStats stats = capture_->GetStats();
        if (stats.generation == active_generation_) {
            static_cast<void>(capture_->RequestStop());
        } else {
            // The stable facade was rebound outside this controller. Never
            // mutate the newer generation; owner-side runtime finalization
            // below is still guarded independently by its exact generation.
            gpu_session_active_ = false;
        }
    }
    active_stop_completion_ = std::move(_request.completion);
    state_                  = ProfileCaptureControllerState::StoppingGpu;
    PublishOwnerState();
    return true;
}

ProfileCaptureTickResult ProfileCaptureController::PollGpuStop() noexcept {
    RenderProfileSessionFinishResult gpu_result = capture_ == nullptr ?
        RenderProfileSessionFinishResult::Closed :
        RenderProfileSessionFinishResult::Inactive;
    if (capture_ != nullptr && gpu_session_active_) {
        const RenderProfileCaptureStats stats = capture_->GetStats();
        gpu_result = stats.generation == active_generation_ ?
            capture_->TryFinishSession() :
            RenderProfileSessionFinishResult::Faulted;
    }
    if (gpu_result == RenderProfileSessionFinishResult::Pending) {
        return ProfileCaptureTickResult::WaitingForGpu;
    }

    FinalizeDynamicRuntime(gpu_result);
    return ProfileCaptureTickResult::Progressed;
}

void ProfileCaptureController::FinalizeDynamicRuntime(
    RenderProfileSessionFinishResult _gpu_result
) noexcept {
    const std::uint64_t generation = active_generation_;
    state_                         = ProfileCaptureControllerState::FinalizingRuntime;
    gpu_session_active_            = false;
    PublishOwnerState();

    ProfileCaptureCompletionStatus status = StopCompletionStatus(_gpu_result);
    ProfileDump::FlushResult flush_result = ProfileDump::FlushResult::Rejected;
    ProfileDump::ShutdownResult shutdown_result =
        ProfileDump::ShutdownResult::AlreadyStopped;
    ProfileDump::RuntimeStats pre_shutdown_stats{};

    if (!OwnsPublishedGeneration()) {
        status = ProfileCaptureCompletionStatus::RuntimeOwnershipLost;
    } else {
        if (cpu_producer_active_) {
            ProfileDump::CpuScopeProducer::Deactivate();
            cpu_producer_active_ = false;
        }
        flush_result    = ProfileDump::FlushAll();
        pre_shutdown_stats = ProfileDump::GetRuntimeStats();
        shutdown_result = ProfileDump::Shutdown();
        status = AccountRuntimeFinalization(
            status,
            flush_result,
            shutdown_result,
            pre_shutdown_stats,
            generation
        );
    }

    owns_runtime_       = false;
    active_generation_  = 0;
    cpu_producer_active_ = false;
    state_               = ProfileCaptureControllerState::Idle;
    PublishOwnerState();

    Complete(
        active_stop_completion_,
        status,
        generation,
        Raw(_gpu_result),
        Raw(shutdown_result)
    );
    active_stop_completion_.reset();
}

ProfileCaptureLifecycleResult
ProfileCaptureController::BeginEngineShutdown() noexcept {
    if (!OnOwnerThread()) {
        return ProfileCaptureLifecycleResult::WrongThread;
    }
    if (state_ == ProfileCaptureControllerState::Shutdown) {
        return ProfileCaptureLifecycleResult::AlreadyAtBoundary;
    }
    if (state_ == ProfileCaptureControllerState::AwaitingRhiDrain ||
        state_ == ProfileCaptureControllerState::AwaitingWorkerShutdown) {
        return ProfileCaptureLifecycleResult::AlreadyAtBoundary;
    }
    if (state_ == ProfileCaptureControllerState::Starting ||
        state_ == ProfileCaptureControllerState::FinalizingRuntime) {
        return ProfileCaptureLifecycleResult::InvalidState;
    }

    {
        std::lock_guard lock(request_mutex_);
        accepting_requests_           = false;
        published_.accepting_requests = false;
    }
    CancelQueued(ProfileCaptureCompletionStatus::CancelledForShutdown);

    if (OwnsPublishedGeneration()) {
        if (cpu_producer_active_) {
            ProfileDump::CpuScopeProducer::Deactivate();
            cpu_producer_active_ = false;
        }
        if (gpu_session_active_ && capture_ != nullptr) {
            const RenderProfileCaptureStats stats = capture_->GetStats();
            if (stats.generation == active_generation_) {
                static_cast<void>(capture_->RequestStop());
            } else {
                gpu_session_active_ = false;
            }
        }
    }

    state_ = ProfileCaptureControllerState::AwaitingRhiDrain;
    PublishOwnerState();
    return ProfileCaptureLifecycleResult::Advanced;
}

ProfileCaptureLifecycleResult
ProfileCaptureController::FinalizeGpuAfterRhiDrain(bool _rhi_drained) noexcept {
    if (!OnOwnerThread()) {
        return ProfileCaptureLifecycleResult::WrongThread;
    }
    if (state_ == ProfileCaptureControllerState::AwaitingWorkerShutdown) {
        return ProfileCaptureLifecycleResult::AlreadyAtBoundary;
    }
    if (state_ != ProfileCaptureControllerState::AwaitingRhiDrain) {
        return state_ == ProfileCaptureControllerState::Shutdown ?
            ProfileCaptureLifecycleResult::AlreadyAtBoundary :
            ProfileCaptureLifecycleResult::InvalidState;
    }

    shutdown_gpu_result_ = capture_ == nullptr && owns_runtime_ ?
        RenderProfileSessionFinishResult::Closed :
        RenderProfileSessionFinishResult::Inactive;
    if (gpu_session_active_ && capture_ != nullptr) {
        const RenderProfileCaptureStats before = capture_->GetStats();
        if (before.generation == active_generation_) {
            if (_rhi_drained) {
                capture_->ShutdownAfterRhiDrain();
            } else {
                capture_->Abort();
            }
            shutdown_gpu_result_ = capture_->TryFinishSession();
        } else {
            shutdown_gpu_result_ = RenderProfileSessionFinishResult::Faulted;
        }
    }
    gpu_session_active_ = false;

    state_ = ProfileCaptureControllerState::AwaitingWorkerShutdown;
    PublishOwnerState();
    return ProfileCaptureLifecycleResult::Advanced;
}

ProfileCaptureLifecycleResult
ProfileCaptureController::FinalizeRuntimeAfterWorkers() noexcept {
    if (!OnOwnerThread()) {
        return ProfileCaptureLifecycleResult::WrongThread;
    }
    if (state_ == ProfileCaptureControllerState::Shutdown) {
        return ProfileCaptureLifecycleResult::AlreadyAtBoundary;
    }
    if (state_ != ProfileCaptureControllerState::AwaitingWorkerShutdown) {
        return ProfileCaptureLifecycleResult::InvalidState;
    }

    const std::uint64_t generation = active_generation_;
    state_                         = ProfileCaptureControllerState::FinalizingRuntime;
    PublishOwnerState();

    ProfileCaptureCompletionStatus stop_status =
        StopCompletionStatus(shutdown_gpu_result_);
    ProfileDump::FlushResult flush_result = ProfileDump::FlushResult::NothingPending;
    ProfileDump::ShutdownResult shutdown_result =
        ProfileDump::ShutdownResult::AlreadyStopped;
    ProfileDump::RuntimeStats pre_shutdown_stats{};

    if (owns_runtime_) {
        if (!OwnsPublishedGeneration()) {
            stop_status = ProfileCaptureCompletionStatus::RuntimeOwnershipLost;
        } else {
            if (cpu_producer_active_) {
                ProfileDump::CpuScopeProducer::Deactivate();
                cpu_producer_active_ = false;
            }
            flush_result    = ProfileDump::FlushAll();
            pre_shutdown_stats = ProfileDump::GetRuntimeStats();
            shutdown_result = ProfileDump::Shutdown();
            stop_status = AccountRuntimeFinalization(
                stop_status,
                flush_result,
                shutdown_result,
                pre_shutdown_stats,
                generation
            );
        }
    }

    owns_runtime_        = false;
    cpu_producer_active_ = false;
    gpu_session_active_  = false;
    active_generation_   = 0;
    state_               = ProfileCaptureControllerState::Shutdown;
    PublishOwnerState();

    if (active_stop_completion_) {
        Complete(
            active_stop_completion_,
            stop_status,
            generation,
            Raw(shutdown_gpu_result_),
            Raw(shutdown_result)
        );
        active_stop_completion_.reset();
    }
    return ProfileCaptureLifecycleResult::Advanced;
}

ProfileCaptureLifecycleResult
ProfileCaptureController::AbandonRuntimeAfterWorkerShutdownFailure() noexcept {
    if (!OnOwnerThread()) {
        return ProfileCaptureLifecycleResult::WrongThread;
    }
    if (state_ == ProfileCaptureControllerState::Shutdown) {
        return ProfileCaptureLifecycleResult::AlreadyAtBoundary;
    }
    if (state_ != ProfileCaptureControllerState::AwaitingWorkerShutdown) {
        return ProfileCaptureLifecycleResult::InvalidState;
    }

    // Worker quiescence is unknown, so no ProfileDump, producer, or facade
    // operation is safe here. Drop only the controller's bookkeeping and make
    // the exceptional leak explicit. In particular, owns_runtime_ must be
    // cleared so the destructor cannot race a still-running worker by trying
    // the normal generation-guarded emergency finalization.
    const std::uint64_t generation = active_generation_;
    owns_runtime_                  = false;
    cpu_producer_active_           = false;
    gpu_session_active_            = false;
    active_generation_             = 0;
    state_                         = ProfileCaptureControllerState::Shutdown;
    PublishOwnerState();

    if (active_stop_completion_) {
        Complete(
            active_stop_completion_,
            ProfileCaptureCompletionStatus::RuntimeShutdownFaulted,
            generation,
            Raw(shutdown_gpu_result_)
        );
        active_stop_completion_.reset();
    }
    return ProfileCaptureLifecycleResult::Advanced;
}

void ProfileCaptureController::RollbackOwnedStart() noexcept {
    if (OwnsPublishedGeneration()) {
        if (gpu_session_active_ && capture_ != nullptr) {
            const RenderProfileCaptureStats stats = capture_->GetStats();
            if (stats.generation == active_generation_ && !stats.closed) {
                capture_->Abort();
            }
        }
        if (cpu_producer_active_) {
            ProfileDump::CpuScopeProducer::Deactivate();
        }
        static_cast<void>(ProfileDump::FlushAll());
        static_cast<void>(ProfileDump::Shutdown());
    }

    owns_runtime_        = false;
    cpu_producer_active_ = false;
    gpu_session_active_  = false;
    active_generation_   = 0;
    state_               = ProfileCaptureControllerState::Idle;
    PublishOwnerState();
}

bool ProfileCaptureController::OwnsPublishedGeneration() const noexcept {
    return owns_runtime_ && active_generation_ != 0 &&
           ProfileDump::GetRuntimeGeneration() == active_generation_;
}

bool ProfileCaptureController::OnOwnerThread() noexcept {
    if (IsOwnerThread()) {
        return true;
    }
    try {
        std::lock_guard lock(request_mutex_);
        ++published_.wrong_thread_calls;
    } catch (...) {
    }
    return false;
}

bool ProfileCaptureController::IsOwnerThread() const noexcept {
    return std::this_thread::get_id() == owner_thread_;
}

void ProfileCaptureController::PublishOwnerState() noexcept {
    try {
        std::lock_guard lock(request_mutex_);
        published_.state               = state_;
        published_.accepting_requests  = accepting_requests_;
        published_.owns_runtime        = owns_runtime_;
        published_.cpu_producer_active = cpu_producer_active_;
        published_.gpu_session_active  = gpu_session_active_;
        published_.queued_requests     = requests_.size();
        published_.queue_capacity      = request_capacity_;
        published_.active_generation  = active_generation_;
    } catch (...) {
    }
}

void ProfileCaptureController::Complete(
    const std::shared_ptr<ProfileCaptureControllerDetail::CompletionState>& _completion,
    ProfileCaptureCompletionStatus                                           _status,
    std::uint64_t                                                            _generation,
    std::uint32_t                                                            _detail,
    std::uint32_t _secondary_detail
) noexcept {
    if (!_completion ||
        _completion->completion_claimed.test_and_set(std::memory_order_acq_rel)) {
        return;
    }

    _completion->generation.store(_generation, std::memory_order_relaxed);
    _completion->detail.store(_detail, std::memory_order_relaxed);
    _completion->secondary_detail.store(
        _secondary_detail, std::memory_order_relaxed
    );
    try {
        std::lock_guard lock(request_mutex_);
        ++published_.requests_completed;
        published_.last_completed_request_id = _completion->request_id;
        published_.last_completion_status    = _status;
    } catch (...) {
    }

    // Ready is the completion publication point. An acquiring ticket observer
    // can now see both the immutable payload and the already-updated controller
    // completion snapshot.
    _completion->status.store(_status, std::memory_order_release);
}

void ProfileCaptureController::CancelQueued(
    ProfileCaptureCompletionStatus _status
) noexcept {
    for (;;) {
        Request request{};
        if (!TryPopRequest(request)) {
            return;
        }
        Complete(
            request.completion,
            _status,
            request.kind == ProfileCaptureRequestKind::Stop ?
                request.expected_generation :
                active_generation_
        );
    }
}

ProfileCaptureControllerSnapshot
ProfileCaptureController::GetSnapshot() const noexcept {
    try {
        std::lock_guard lock(request_mutex_);
        return published_;
    } catch (...) {
        ProfileCaptureControllerSnapshot snapshot{};
        snapshot.state              = state_;
        snapshot.accepting_requests = false;
        snapshot.queue_capacity     = request_capacity_;
        return snapshot;
    }
}

} // namespace Moer::Render
