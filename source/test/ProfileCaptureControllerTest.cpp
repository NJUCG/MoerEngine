#include "ProfileCaptureController.h"

#include "profile/ProfileDump.h"
#include "profile/ProfileScope.h"
#include "profile/RenderProfileCapture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace Moer;
using namespace Moer::ProfileDump;
using namespace Moer::Render;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

class ScopedCaptureOutput final {
public:
    explicit ScopedCaptureOutput(std::string_view _stem) {
        static std::atomic_uint64_t next_id{0};
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto now = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            directory_ = std::filesystem::temp_directory_path() /
                         (std::string(_stem) + "-" + std::to_string(now) + "-" +
                          std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory(directory_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error(
            "ProfileCaptureController test could not reserve a temp directory"
        );
    }

    ~ScopedCaptureOutput() {
        CpuScopeProducer::Deactivate();
        static_cast<void>(Shutdown());
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] std::filesystem::path Path(std::string_view _filename) const {
        return directory_ / std::string(_filename);
    }

private:
    std::filesystem::path directory_{};
};

ProfileCaptureStartOptions StartOptions(
    const ScopedCaptureOutput& _output,
    std::string_view           _filename
) {
    ProfileCaptureStartOptions options{};
    options.runtime.output_path      = _output.Path(_filename);
    options.runtime.replace_existing = true;
    return options;
}

ProfileCaptureStartOptions LossyStartOptions(
    const ScopedCaptureOutput& _output,
    std::string_view           _filename
) {
    ProfileCaptureStartOptions options = StartOptions(_output, _filename);
    options.runtime.max_record_bytes   = 64;
    return options;
}

void EmitOversizedRuntimeRecord(std::uint64_t _generation) {
    const SchemaDescriptor schema = {
        .name           = "ControllerLossProbe",
        .event_type     = "contract.controller_loss_probe",
        .kind           = EventKind::Instant,
        .channel        = Channel::CpuThread,
        .schema_version = 1,
        .fields =
            {
                {"payload", FieldType::String},
            },
    };
    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(
        registration.status == SchemaStatus::Registered &&
            registration.handle.generation == _generation,
        "loss probe schema did not bind the controller generation"
    );

    const std::string oversized_payload(256, 'x');
    const std::array<FieldValueView, 1> values = {
        FieldValueView{std::string_view(oversized_payload)},
    };
    Expect(
        Emit(registration.handle, values) == EmitStatus::RecordTooLarge,
        "loss probe did not deterministically reject an oversized record"
    );
    const RuntimeStats stats = GetRuntimeStats();
    Expect(
        stats.generation == _generation &&
            stats.records_dropped_oversized == 1,
        "loss probe rejection was not attributed to the controller generation"
    );
}

ProfileCaptureRequestCompletion CompletionOf(
    const ProfileCaptureRequestTicket& _ticket,
    std::string_view                    _message
) {
    ProfileCaptureRequestCompletion completion{};
    Expect(_ticket.Valid(), "request did not return a valid completion ticket");
    Expect(_ticket.Ready(), _message);
    Expect(_ticket.TryGet(completion), "ready request ticket did not publish its completion");
    Expect(
        completion.request_id != 0,
        "request completion did not preserve a non-zero request id"
    );
    return completion;
}

void TickUntilReady(
    ProfileCaptureController&          _controller,
    const ProfileCaptureRequestTicket& _ticket,
    std::string_view                    _message,
    std::size_t                         _max_ticks = 64
) {
    for (std::size_t tick = 0; tick < _max_ticks && !_ticket.Ready(); ++tick) {
        const ProfileCaptureTickResult result = _controller.TickOwner();
        Expect(
            result != ProfileCaptureTickResult::WrongThread,
            "owner-thread tick was rejected as a foreign-thread call"
        );
        Expect(
            result != ProfileCaptureTickResult::Shutdown,
            "controller reached shutdown before a request completed"
        );
    }
    Expect(_ticket.Ready(), _message);
}

std::uint64_t StartCapture(
    ProfileCaptureController& _controller,
    ProfileCaptureStartOptions _options
) {
    ProfileCaptureRequestSubmission submission =
        _controller.RequestStart(std::move(_options));
    Expect(
        submission.result == ProfileCaptureSubmitResult::Queued,
        "valid capture start request was not queued"
    );
    TickUntilReady(
        _controller,
        submission.ticket,
        "capture start request did not complete"
    );
    const ProfileCaptureRequestCompletion completion =
        CompletionOf(submission.ticket, "capture start completion was not ready");
    Expect(
        completion.kind == ProfileCaptureRequestKind::Start &&
            completion.status == ProfileCaptureCompletionStatus::Started &&
            completion.generation != 0,
        "capture start request did not publish a live generation"
    );
    return completion.generation;
}

std::uint64_t StartCpuOnlyCapture(
    ProfileCaptureController& _controller,
    ProfileCaptureStartOptions _options
) {
    ProfileCaptureRequestSubmission submission =
        _controller.RequestStart(std::move(_options));
    Expect(
        submission.result == ProfileCaptureSubmitResult::Queued,
        "valid CPU-only capture start request was not queued"
    );
    TickUntilReady(
        _controller,
        submission.ticket,
        "CPU-only capture start request did not complete"
    );
    const ProfileCaptureRequestCompletion completion =
        CompletionOf(
            submission.ticket,
            "CPU-only capture start completion was not ready"
        );
    Expect(
        completion.kind == ProfileCaptureRequestKind::Start &&
            completion.status ==
                ProfileCaptureCompletionStatus::StartedCpuOnly &&
            completion.generation != 0,
        "null-facade controller did not publish a CPU-only generation"
    );
    return completion.generation;
}

ProfileCaptureRequestCompletion StopCapture(
    ProfileCaptureController& _controller,
    std::uint64_t             _generation
) {
    ProfileCaptureRequestSubmission submission =
        _controller.RequestStop(_generation);
    Expect(
        submission.result == ProfileCaptureSubmitResult::Queued,
        "valid capture stop request was not queued"
    );
    TickUntilReady(
        _controller,
        submission.ticket,
        "capture stop request did not complete"
    );
    return CompletionOf(submission.ticket, "capture stop completion was not ready");
}

void FinishEngineShutdown(ProfileCaptureController& _controller) {
    Expect(
        _controller.FinalizeGpuAfterRhiDrain(true) ==
            ProfileCaptureLifecycleResult::Advanced,
        "engine shutdown did not cross the post-RHI GPU boundary"
    );
    Expect(
        _controller.FinalizeRuntimeAfterWorkers() ==
            ProfileCaptureLifecycleResult::Advanced,
        "engine shutdown did not cross the post-worker runtime boundary"
    );
    Expect(
        _controller.GetSnapshot().state == ProfileCaptureControllerState::Shutdown,
        "engine shutdown did not publish the terminal controller state"
    );
}

void AdmissionValidationQueueCapacityAndFifo() {
    ScopedCaptureOutput    output("moer-profile-controller-fifo");
    RenderProfileCapture   capture;
    ProfileCaptureController controller(&capture, 2);

    const ProfileCaptureRequestSubmission invalid_stop =
        controller.RequestStop(0);
    Expect(
        invalid_stop.result == ProfileCaptureSubmitResult::InvalidArgument,
        "Stop(0) was treated as a wildcard or queued request"
    );
    const ProfileCaptureRequestCompletion invalid_completion =
        CompletionOf(invalid_stop.ticket, "Stop(0) did not complete synchronously");
    Expect(
        invalid_completion.kind == ProfileCaptureRequestKind::Stop &&
            invalid_completion.status ==
                ProfileCaptureCompletionStatus::RejectedInvalidArgument,
        "Stop(0) completion did not retain its invalid-argument reason"
    );

    ProfileCaptureRequestSubmission first =
        controller.RequestStart(StartOptions(output, "first.mpd"));
    ProfileCaptureRequestSubmission second =
        controller.RequestStart(StartOptions(output, "second.mpd"));
    ProfileCaptureRequestSubmission overflow =
        controller.RequestStart(StartOptions(output, "overflow.mpd"));
    Expect(
        first.result == ProfileCaptureSubmitResult::Queued &&
            second.result == ProfileCaptureSubmitResult::Queued,
        "bounded FIFO rejected work before reaching its capacity"
    );
    Expect(
        overflow.result == ProfileCaptureSubmitResult::QueueFull,
        "bounded FIFO silently exceeded its declared capacity"
    );
    const ProfileCaptureControllerSnapshot full = controller.GetSnapshot();
    Expect(
        full.queued_requests == 2 && full.queue_capacity == 2,
        "bounded FIFO snapshot did not expose its exact resident capacity"
    );
    const ProfileCaptureRequestCompletion overflow_completion =
        CompletionOf(overflow.ticket, "queue-full request did not complete synchronously");
    Expect(
        overflow_completion.status ==
            ProfileCaptureCompletionStatus::RejectedQueueFull,
        "queue-full ticket lost its rejection reason"
    );

    TickUntilReady(
        controller,
        first.ticket,
        "front FIFO start request did not complete"
    );
    TickUntilReady(
        controller,
        second.ticket,
        "second FIFO start request did not complete"
    );
    const ProfileCaptureRequestCompletion first_completion =
        CompletionOf(first.ticket, "front FIFO completion was not ready");
    const ProfileCaptureRequestCompletion second_completion =
        CompletionOf(second.ticket, "second FIFO completion was not ready");
    Expect(
        first_completion.status == ProfileCaptureCompletionStatus::Started,
        "bounded request queue did not process its front start first"
    );
    Expect(
        second_completion.status ==
            ProfileCaptureCompletionStatus::RejectedAlreadyRunning,
        "second FIFO start did not observe the session opened by the first"
    );
    Expect(
        first_completion.request_id < second_completion.request_id &&
            second_completion.request_id < overflow_completion.request_id,
        "single-thread request ids did not preserve admission order"
    );

    const ProfileCaptureRequestCompletion stopped =
        StopCapture(controller, first_completion.generation);
    Expect(
        stopped.status == ProfileCaptureCompletionStatus::Stopped,
        "empty FIFO-owned session did not stop cleanly"
    );
    Expect(
        GetRuntimeState() == RuntimeState::Stopped,
        "dynamic FIFO-owned stop left ProfileDump running"
    );
}

void ConcurrentSubmissionIdsAreUniqueAndQueueIsBounded() {
    constexpr std::size_t thread_count     = 8;
    constexpr std::size_t requests_per_thread = 8;
    constexpr std::size_t request_count    = thread_count * requests_per_thread;
    constexpr std::size_t queue_capacity   = 17;

    ScopedCaptureOutput      output("moer-profile-controller-mpsc");
    RenderProfileCapture     capture;
    ProfileCaptureController controller(&capture, queue_capacity);
    std::barrier              start_gate(static_cast<std::ptrdiff_t>(thread_count + 1));
    std::vector<std::vector<ProfileCaptureRequestSubmission>> submissions(thread_count);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            submissions[thread_index].reserve(requests_per_thread);
            start_gate.arrive_and_wait();
            for (std::size_t request_index = 0;
                 request_index < requests_per_thread;
                 ++request_index) {
                submissions[thread_index].push_back(controller.RequestStart(
                    StartOptions(output, "not-started.mpd")
                ));
            }
        });
    }
    start_gate.arrive_and_wait();
    for (std::thread& worker : workers) {
        worker.join();
    }

    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (const auto& per_thread : submissions) {
        for (const ProfileCaptureRequestSubmission& submission : per_thread) {
            Expect(
                submission.ticket.Valid(),
                "concurrent submission lost its completion ticket"
            );
            if (submission.result == ProfileCaptureSubmitResult::Queued) {
                ++accepted;
            } else {
                Expect(
                    submission.result == ProfileCaptureSubmitResult::QueueFull,
                    "concurrent bounded admission returned an unexpected result"
                );
                ++rejected;
            }
        }
    }
    Expect(
        accepted == queue_capacity && rejected == request_count - queue_capacity,
        "concurrent MPSC admission did not enforce its exact resident bound"
    );
    const ProfileCaptureControllerSnapshot admitted = controller.GetSnapshot();
    Expect(
        admitted.queued_requests == queue_capacity &&
            admitted.requests_submitted == request_count &&
            admitted.requests_accepted == queue_capacity &&
            admitted.requests_rejected == request_count - queue_capacity,
        "concurrent MPSC snapshot accounting is inconsistent"
    );

    Expect(
        controller.BeginEngineShutdown() ==
            ProfileCaptureLifecycleResult::Advanced,
        "shutdown did not close concurrent request admission"
    );
    std::set<std::uint64_t> request_ids;
    std::uint64_t           largest_accepted_request_id = 0;
    for (const auto& per_thread : submissions) {
        for (const ProfileCaptureRequestSubmission& submission : per_thread) {
            const ProfileCaptureRequestCompletion completion =
                CompletionOf(
                    submission.ticket,
                    "shutdown did not complete every concurrent request ticket"
                );
            Expect(
                completion.status ==
                        ProfileCaptureCompletionStatus::CancelledForShutdown ||
                    completion.status ==
                        ProfileCaptureCompletionStatus::RejectedQueueFull,
                "concurrent ticket reached an unexpected terminal status"
            );
            if (submission.result == ProfileCaptureSubmitResult::Queued) {
                Expect(
                    completion.status ==
                        ProfileCaptureCompletionStatus::CancelledForShutdown,
                    "accepted concurrent request was not cancelled from the FIFO"
                );
                largest_accepted_request_id =
                    std::max(largest_accepted_request_id, completion.request_id);
            }
            request_ids.insert(completion.request_id);
        }
    }
    Expect(
        request_ids.size() == request_count,
        "concurrent request ids were duplicated"
    );
    Expect(
        *request_ids.begin() != 0 &&
            *request_ids.rbegin() - *request_ids.begin() + 1 == request_count,
        "concurrent request ids were not allocated from one bounded contiguous interval"
    );
    const ProfileCaptureControllerSnapshot cancelled = controller.GetSnapshot();
    Expect(
        largest_accepted_request_id != 0 &&
            cancelled.last_completed_request_id ==
                largest_accepted_request_id &&
            cancelled.last_completion_status ==
                ProfileCaptureCompletionStatus::CancelledForShutdown,
        "concurrent request ids did not remain monotonic with FIFO cancellation order"
    );
    FinishEngineShutdown(controller);
}

void ReadyTicketPublishesCompletionAccounting() {
    constexpr std::size_t request_count = 512;

    ScopedCaptureOutput      output("moer-profile-controller-ready-publication");
    RenderProfileCapture     capture;
    ProfileCaptureController controller(&capture, request_count);
    std::vector<ProfileCaptureRequestSubmission> submissions;
    submissions.reserve(request_count);
    for (std::size_t index = 0; index < request_count; ++index) {
        submissions.push_back(controller.RequestStart(
            StartOptions(output, "never-started.mpd")
        ));
        Expect(
            submissions.back().result == ProfileCaptureSubmitResult::Queued,
            "ready-publication setup unexpectedly exceeded its queue"
        );
    }

    std::barrier cancellation_gate(2);
    std::atomic_bool publication_violation{false};
    std::atomic_bool stop_observer{false};
    std::thread observer([&] {
        cancellation_gate.arrive_and_wait();
        for (const ProfileCaptureRequestSubmission& submission : submissions) {
            while (!submission.ticket.Ready()) {
                if (stop_observer.load(std::memory_order_relaxed)) {
                    publication_violation.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
            }

            ProfileCaptureRequestCompletion completion{};
            if (!submission.ticket.TryGet(completion)) {
                publication_violation.store(true, std::memory_order_relaxed);
                return;
            }

            // Cancellation is FIFO and ids are assigned at that same
            // linearization point. Once Ready is visible, the snapshot must
            // already include this completion or a later one.
            const ProfileCaptureControllerSnapshot snapshot =
                controller.GetSnapshot();
            if (snapshot.requests_completed < completion.request_id ||
                snapshot.last_completed_request_id < completion.request_id) {
                publication_violation.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });

    cancellation_gate.arrive_and_wait();
    const ProfileCaptureLifecycleResult begin_result =
        controller.BeginEngineShutdown();
    if (begin_result != ProfileCaptureLifecycleResult::Advanced) {
        stop_observer.store(true, std::memory_order_relaxed);
    }
    observer.join();
    Expect(
        begin_result == ProfileCaptureLifecycleResult::Advanced,
        "ready-publication shutdown did not begin"
    );
    Expect(
        !publication_violation.load(std::memory_order_relaxed),
        "ticket Ready became visible before its snapshot completion accounting"
    );
    const ProfileCaptureControllerSnapshot completed = controller.GetSnapshot();
    Expect(
        completed.requests_completed == request_count &&
            completed.last_completed_request_id == request_count &&
            completed.last_completion_status ==
                ProfileCaptureCompletionStatus::CancelledForShutdown,
        "ready-publication final completion accounting is inconsistent"
    );
    FinishEngineShutdown(controller);
}

void ExternalRuntimeIsNeverAdoptedOrShutdown() {
    ScopedCaptureOutput output("moer-profile-controller-external");
    RuntimeConfig       external_config{};
    external_config.output_path      = output.Path("external.mpd");
    external_config.replace_existing = true;
    Expect(
        Start(external_config) == StartResult::Started,
        "external ProfileDump test runtime did not start"
    );
    const std::uint64_t external_generation = GetRuntimeGeneration();

    RenderProfileCapture capture;
    {
        ProfileCaptureController controller(&capture, 4);
        ProfileCaptureRequestSubmission request =
            controller.RequestStart(StartOptions(output, "controller.mpd"));
        Expect(
            request.result == ProfileCaptureSubmitResult::Queued,
            "controller did not queue a start probe against an external runtime"
        );
        TickUntilReady(
            controller,
            request.ticket,
            "external-runtime start probe did not complete"
        );
        const ProfileCaptureRequestCompletion completion =
            CompletionOf(request.ticket, "external-runtime completion was not ready");
        Expect(
            completion.status ==
                ProfileCaptureCompletionStatus::RejectedExternalRuntime,
            "ProfileDump::AlreadyRunning was not exposed as external ownership"
        );
        const ProfileCaptureControllerSnapshot snapshot = controller.GetSnapshot();
        Expect(
            snapshot.state == ProfileCaptureControllerState::Idle &&
                !snapshot.owns_runtime && snapshot.active_generation == 0 &&
                !capture.Valid(),
            "controller adopted state from an externally owned ProfileDump runtime"
        );
        Expect(
            GetRuntimeState() == RuntimeState::Running &&
                GetRuntimeGeneration() == external_generation,
            "external ProfileDump runtime changed while the controller rejected adoption"
        );
    }
    Expect(
        GetRuntimeState() == RuntimeState::Running &&
            GetRuntimeGeneration() == external_generation,
        "controller destructor shut down or replaced the external runtime"
    );
    Expect(
        Shutdown() == ShutdownResult::Completed,
        "external owner could not shut down its untouched runtime"
    );
}

void ControllerDestructorClosesOwnedRunningSession() {
    ScopedCaptureOutput      output("moer-profile-controller-destroy-running");
    RenderProfileCapture     capture;
    std::uint64_t            generation = 0;
    {
        auto controller = std::make_unique<ProfileCaptureController>(&capture, 4);
        generation = StartCapture(
            *controller,
            StartOptions(output, "destroy-running.mpd")
        );
        Expect(
            capture.Valid() && GetRuntimeState() == RuntimeState::Running,
            "running-destructor setup did not own a live capture session"
        );
    }

    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        !capture.Valid() && stats.closed && stats.generation == generation &&
            GetRuntimeState() == RuntimeState::Stopped &&
            !CpuScopeProducer::IsActive(),
        "controller destructor leaked its owned running generation"
    );
}

void ControllerDestructorClosesOwnedStoppingSession() {
    ScopedCaptureOutput      output("moer-profile-controller-destroy-stopping");
    RenderProfileCapture     capture;
    RenderProfileFrameToken frame;
    ProfileCaptureRequestTicket stop_ticket;
    std::uint64_t            generation = 0;
    {
        auto controller = std::make_unique<ProfileCaptureController>(&capture, 4);
        generation = StartCapture(
            *controller,
            StartOptions(output, "destroy-stopping.mpd")
        );
        frame = capture.BeginFrame();
        Expect(
            frame.Valid(),
            "stopping-destructor setup did not admit its pending GPU frame"
        );
        ProfileCaptureRequestSubmission stop =
            controller->RequestStop(generation);
        Expect(
            stop.result == ProfileCaptureSubmitResult::Queued,
            "stopping-destructor stop request was not queued"
        );
        stop_ticket = stop.ticket;
        Expect(
            controller->TickOwner() ==
                    ProfileCaptureTickResult::WaitingForGpu &&
                controller->GetSnapshot().state ==
                    ProfileCaptureControllerState::StoppingGpu &&
                !stop_ticket.Ready(),
            "stopping-destructor setup did not retain its unsealed GPU tail"
        );
    }

    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        !capture.Valid() && stats.closed && stats.generation == generation &&
            stats.shutdown_abandoned_frames == 1 &&
            GetRuntimeState() == RuntimeState::Stopped &&
            !CpuScopeProducer::IsActive(),
        "controller destructor leaked or presented its pending GPU tail as complete"
    );
    const ProfileCaptureRequestCompletion stop_completion =
        CompletionOf(
            stop_ticket,
            "controller destructor left the active stop ticket pending"
        );
    Expect(
        stop_completion.generation == generation &&
            stop_completion.status ==
                ProfileCaptureCompletionStatus::ControllerDestroyed,
        "controller destructor published an invalid pending-stop terminal result"
    );
    frame = {};
}

void NullFacadeRunsAndStopsCpuOnlyCapture() {
    ScopedCaptureOutput      output("moer-profile-controller-cpu-only-stop");
    ProfileCaptureController controller(nullptr, 4);
    const std::uint64_t generation = StartCpuOnlyCapture(
        controller,
        StartOptions(output, "cpu-only-stop.mpd")
    );

    ProfileCaptureControllerSnapshot snapshot = controller.GetSnapshot();
    Expect(
        snapshot.state == ProfileCaptureControllerState::Running &&
            snapshot.owns_runtime && snapshot.cpu_producer_active &&
            !snapshot.gpu_session_active &&
            snapshot.active_generation == generation &&
            GetRuntimeState() == RuntimeState::Running &&
            GetRuntimeGeneration() == generation &&
            CpuScopeProducer::IsActive(),
        "CPU-only start did not publish its runtime and producer ownership"
    );

    const ProfileCaptureRequestCompletion stopped =
        StopCapture(controller, generation);
    snapshot = controller.GetSnapshot();
    Expect(
        stopped.status == ProfileCaptureCompletionStatus::Stopped &&
            stopped.generation == generation &&
            snapshot.state == ProfileCaptureControllerState::Idle &&
            !snapshot.owns_runtime && !snapshot.cpu_producer_active &&
            !snapshot.gpu_session_active &&
            snapshot.active_generation == 0 &&
            GetRuntimeState() == RuntimeState::Stopped &&
            !CpuScopeProducer::IsActive(),
        "exact-generation CPU-only stop leaked its owned runtime"
    );
}

void NullFacadePreservesEngineShutdownSplit() {
    ScopedCaptureOutput      output("moer-profile-controller-cpu-only-shutdown");
    ProfileCaptureController controller(nullptr, 4);
    const std::uint64_t generation = StartCpuOnlyCapture(
        controller,
        StartOptions(output, "cpu-only-shutdown.mpd")
    );

    Expect(
        controller.BeginEngineShutdown() ==
            ProfileCaptureLifecycleResult::Advanced,
        "CPU-only Engine shutdown did not begin"
    );
    ProfileCaptureControllerSnapshot snapshot = controller.GetSnapshot();
    Expect(
        snapshot.state == ProfileCaptureControllerState::AwaitingRhiDrain &&
            snapshot.owns_runtime && !snapshot.cpu_producer_active &&
            !snapshot.gpu_session_active &&
            snapshot.active_generation == generation &&
            GetRuntimeState() == RuntimeState::Running &&
            !CpuScopeProducer::IsActive(),
        "CPU-only BeginEngineShutdown crossed the runtime finalization boundary"
    );

    Expect(
        controller.FinalizeGpuAfterRhiDrain(true) ==
            ProfileCaptureLifecycleResult::Advanced,
        "CPU-only controller did not cross the post-RHI boundary"
    );
    snapshot = controller.GetSnapshot();
    Expect(
        snapshot.state ==
                ProfileCaptureControllerState::AwaitingWorkerShutdown &&
            snapshot.owns_runtime && !snapshot.gpu_session_active &&
            snapshot.active_generation == generation &&
            GetRuntimeState() == RuntimeState::Running,
        "CPU-only post-RHI boundary shut down ProfileDump before workers joined"
    );

    Expect(
        controller.FinalizeRuntimeAfterWorkers() ==
            ProfileCaptureLifecycleResult::Advanced,
        "CPU-only controller did not cross the post-worker boundary"
    );
    snapshot = controller.GetSnapshot();
    Expect(
        snapshot.state == ProfileCaptureControllerState::Shutdown &&
            !snapshot.owns_runtime && !snapshot.cpu_producer_active &&
            !snapshot.gpu_session_active &&
            snapshot.active_generation == 0 &&
            GetRuntimeState() == RuntimeState::Stopped,
        "CPU-only post-worker boundary leaked its ProfileDump runtime"
    );
}

void NullFacadeDestructorClosesOwnedCpuOnlyCapture() {
    ScopedCaptureOutput output("moer-profile-controller-cpu-only-destructor");
    std::uint64_t       generation = 0;
    {
        ProfileCaptureController controller(nullptr, 4);
        generation = StartCpuOnlyCapture(
            controller,
            StartOptions(output, "cpu-only-destructor.mpd")
        );
        Expect(
            GetRuntimeState() == RuntimeState::Running &&
                GetRuntimeGeneration() == generation &&
                CpuScopeProducer::IsActive(),
            "CPU-only destructor setup did not own a live generation"
        );
    }

    Expect(
        generation != 0 && GetRuntimeState() == RuntimeState::Stopped &&
            !CpuScopeProducer::IsActive(),
        "null-facade controller destructor leaked its owned CPU-only generation"
    );
}

void RestartUsesStableFacadeAndRejectsStaleStop() {
    ScopedCaptureOutput      output("moer-profile-controller-restart");
    RenderProfileCapture     capture;
    RenderProfileCapture* const stable_facade = &capture;
    ProfileCaptureController controller(stable_facade, 8);

    const std::uint64_t first_generation =
        StartCapture(controller, StartOptions(output, "first.mpd"));
    Expect(
        &capture == stable_facade && capture.Valid() &&
            capture.GetStats().generation == first_generation,
        "first generation was not published through the stable GPU facade"
    );
    const ProfileCaptureRequestCompletion first_stop =
        StopCapture(controller, first_generation);
    Expect(
        first_stop.status == ProfileCaptureCompletionStatus::Stopped &&
            first_stop.generation == first_generation &&
            GetRuntimeState() == RuntimeState::Stopped,
        "first dynamic generation did not close cleanly"
    );

    const std::uint64_t second_generation =
        StartCapture(controller, StartOptions(output, "second.mpd"));
    Expect(
        second_generation > first_generation && &capture == stable_facade &&
            capture.Valid() &&
            capture.GetStats().generation == second_generation,
        "restart did not rebind the stable GPU facade to a newer generation"
    );

    ProfileCaptureRequestSubmission stale_stop =
        controller.RequestStop(first_generation);
    Expect(
        stale_stop.result == ProfileCaptureSubmitResult::Queued,
        "non-zero stale stop was rejected before owner-side generation validation"
    );
    TickUntilReady(
        controller,
        stale_stop.ticket,
        "stale stop request did not complete"
    );
    const ProfileCaptureRequestCompletion stale_completion =
        CompletionOf(stale_stop.ticket, "stale stop completion was not ready");
    Expect(
        stale_completion.status ==
                ProfileCaptureCompletionStatus::RejectedStaleGeneration &&
            stale_completion.generation == second_generation,
        "old-generation stop did not report the currently protected generation"
    );
    const ProfileCaptureControllerSnapshot after_stale = controller.GetSnapshot();
    Expect(
        after_stale.state == ProfileCaptureControllerState::Running &&
            after_stale.active_generation == second_generation &&
            after_stale.owns_runtime && capture.Valid() &&
            GetRuntimeState() == RuntimeState::Running,
        "stale stop mutated the restarted capture session"
    );

    const ProfileCaptureRequestCompletion second_stop =
        StopCapture(controller, second_generation);
    Expect(
        second_stop.status == ProfileCaptureCompletionStatus::Stopped &&
            GetRuntimeState() == RuntimeState::Stopped,
        "current-generation stop did not close the restarted session"
    );
}

void AdmittedFrameKeepsDynamicStopPendingUntilSealed() {
    ScopedCaptureOutput      output("moer-profile-controller-pending-frame");
    RenderProfileCapture     capture;
    ProfileCaptureController controller(&capture, 4);
    const std::uint64_t generation =
        StartCapture(controller, StartOptions(output, "pending-frame.mpd"));

    RenderProfileFrameToken frame = capture.BeginFrame();
    Expect(frame.Valid(), "running controller did not admit a GPU frame token");
    ProfileCaptureRequestSubmission stop = controller.RequestStop(generation);
    Expect(
        stop.result == ProfileCaptureSubmitResult::Queued,
        "pending-frame stop request was not queued"
    );
    Expect(
        controller.TickOwner() == ProfileCaptureTickResult::WaitingForGpu,
        "owner did not expose the pending admitted GPU frame"
    );
    Expect(
        !stop.ticket.Ready() &&
            controller.GetSnapshot().state ==
                ProfileCaptureControllerState::StoppingGpu &&
            GetRuntimeState() == RuntimeState::Running,
        "dynamic stop finalized while an admitted GPU frame remained unsealed"
    );
    Expect(
        controller.TickOwner() == ProfileCaptureTickResult::WaitingForGpu &&
            !stop.ticket.Ready(),
        "repeated owner polling bypassed an admitted GPU frame"
    );

    Expect(capture.Seal(frame), "admitted GPU frame could not be sealed after stop");
    TickUntilReady(
        controller,
        stop.ticket,
        "sealed admitted GPU frame did not let stop finish"
    );
    const ProfileCaptureRequestCompletion completion =
        CompletionOf(stop.ticket, "pending-frame stop completion was not ready");
    Expect(
        completion.status == ProfileCaptureCompletionStatus::Stopped &&
            completion.generation == generation &&
            controller.GetSnapshot().state ==
                ProfileCaptureControllerState::Idle &&
            GetRuntimeState() == RuntimeState::Stopped,
        "sealed admitted GPU frame did not close the owned runtime cleanly"
    );
}

void EngineShutdownPreservesRhiAndWorkerOwnershipBoundaries() {
    ScopedCaptureOutput      output("moer-profile-controller-shutdown-split");
    RenderProfileCapture     capture;
    ProfileCaptureController controller(&capture, 4);
    const std::uint64_t generation =
        StartCapture(controller, StartOptions(output, "shutdown-split.mpd"));

    Expect(
        controller.BeginEngineShutdown() ==
            ProfileCaptureLifecycleResult::Advanced,
        "owned controller did not begin engine shutdown"
    );
    ProfileCaptureControllerSnapshot snapshot = controller.GetSnapshot();
    Expect(
        snapshot.state == ProfileCaptureControllerState::AwaitingRhiDrain &&
            !snapshot.accepting_requests && snapshot.owns_runtime &&
            snapshot.active_generation == generation &&
            GetRuntimeState() == RuntimeState::Running,
        "BeginEngineShutdown crossed the RHI or ProfileDump owner boundary"
    );
    Expect(
        !capture.Valid(),
        "BeginEngineShutdown did not close future GPU frame admission"
    );

    Expect(
        controller.FinalizeGpuAfterRhiDrain(true) ==
            ProfileCaptureLifecycleResult::Advanced,
        "post-RHI GPU finalization did not advance"
    );
    snapshot = controller.GetSnapshot();
    Expect(
        snapshot.state ==
                ProfileCaptureControllerState::AwaitingWorkerShutdown &&
            snapshot.owns_runtime && !snapshot.gpu_session_active &&
            GetRuntimeState() == RuntimeState::Running,
        "GPU finalization shut down ProfileDump before worker producers joined"
    );

    Expect(
        controller.FinalizeRuntimeAfterWorkers() ==
            ProfileCaptureLifecycleResult::Advanced,
        "post-worker ProfileDump finalization did not advance"
    );
    snapshot = controller.GetSnapshot();
    Expect(
        snapshot.state == ProfileCaptureControllerState::Shutdown &&
            !snapshot.owns_runtime && snapshot.active_generation == 0 &&
            GetRuntimeState() == RuntimeState::Stopped,
        "post-worker finalization did not release the owned ProfileDump runtime"
    );
}

void DynamicStopReportsProfileDumpRecordLoss() {
    ScopedCaptureOutput      output("moer-profile-controller-dynamic-loss");
    ProfileCaptureController controller(nullptr, 4);
    const std::uint64_t generation = StartCpuOnlyCapture(
        controller,
        LossyStartOptions(output, "dynamic-loss.mpd")
    );
    EmitOversizedRuntimeRecord(generation);

    const ProfileCaptureRequestCompletion completion =
        StopCapture(controller, generation);
    Expect(
        completion.status ==
                ProfileCaptureCompletionStatus::StoppedWithLoss &&
            completion.generation == generation &&
            GetRuntimeState() == RuntimeState::Stopped,
        "dynamic Stop published a clean terminal status after ProfileDump loss"
    );
}

void EngineShutdownReportsProfileDumpRecordLoss() {
    ScopedCaptureOutput      output("moer-profile-controller-shutdown-loss");
    RenderProfileCapture     capture;
    ProfileCaptureController controller(&capture, 4);
    const std::uint64_t generation = StartCapture(
        controller,
        LossyStartOptions(output, "shutdown-loss.mpd")
    );
    EmitOversizedRuntimeRecord(generation);

    RenderProfileFrameToken frame = capture.BeginFrame();
    Expect(frame.Valid(), "shutdown-loss setup did not admit a GPU frame");
    ProfileCaptureRequestSubmission stop =
        controller.RequestStop(generation);
    Expect(
        stop.result == ProfileCaptureSubmitResult::Queued &&
            controller.TickOwner() == ProfileCaptureTickResult::WaitingForGpu &&
            !stop.ticket.Ready(),
        "shutdown-loss setup did not retain an active Stop ticket"
    );

    Expect(
        controller.BeginEngineShutdown() ==
            ProfileCaptureLifecycleResult::Advanced,
        "shutdown-loss setup did not cross the admission boundary"
    );
    Expect(
        capture.Seal(frame),
        "shutdown-loss setup could not seal its admitted GPU frame"
    );
    Expect(
        controller.FinalizeGpuAfterRhiDrain(true) ==
            ProfileCaptureLifecycleResult::Advanced,
        "shutdown-loss setup did not cross the post-RHI boundary"
    );
    const RenderProfileCaptureStats gpu_stats = capture.GetStats();
    Expect(
        gpu_stats.closed && gpu_stats.frames_emitted == 1 &&
            gpu_stats.frame_records_dropped == 0 &&
            gpu_stats.scope_records_dropped == 0 &&
            gpu_stats.terminal_faults == 0 &&
            gpu_stats.shutdown_abandoned_frames == 0 &&
            !stop.ticket.Ready(),
        "shutdown-loss setup introduced GPU loss instead of isolating ProfileDump loss"
    );
    Expect(
        controller.FinalizeRuntimeAfterWorkers() ==
            ProfileCaptureLifecycleResult::Advanced,
        "shutdown-loss setup did not cross the post-worker boundary"
    );

    const ProfileCaptureRequestCompletion completion =
        CompletionOf(
            stop.ticket,
            "Engine shutdown did not complete the active Stop ticket"
        );
    Expect(
        completion.status ==
                ProfileCaptureCompletionStatus::StoppedWithLoss &&
            completion.generation == generation &&
            GetRuntimeState() == RuntimeState::Stopped,
        "post-worker finalization published a clean status after ProfileDump loss"
    );
}

void WorkerShutdownFailureAbandonsRuntimeWithoutUnsafeFinalization() {
    ScopedCaptureOutput  output("moer-profile-controller-worker-failure");
    RenderProfileCapture capture;
    std::uint64_t        generation = 0;

    {
        ProfileCaptureController controller(&capture, 4);
        generation = StartCapture(
            controller,
            StartOptions(output, "worker-failure.mpd")
        );

        Expect(
            controller.BeginEngineShutdown() ==
                ProfileCaptureLifecycleResult::Advanced,
            "worker-failure setup did not begin engine shutdown"
        );
        Expect(
            controller.FinalizeGpuAfterRhiDrain(true) ==
                ProfileCaptureLifecycleResult::Advanced,
            "worker-failure setup did not cross the post-RHI boundary"
        );
        Expect(
            controller.AbandonRuntimeAfterWorkerShutdownFailure() ==
                ProfileCaptureLifecycleResult::Advanced,
            "worker-failure path did not abandon controller ownership"
        );

        const ProfileCaptureControllerSnapshot snapshot =
            controller.GetSnapshot();
        Expect(
            snapshot.state == ProfileCaptureControllerState::Shutdown &&
                !snapshot.owns_runtime &&
                snapshot.active_generation == 0 &&
                GetRuntimeState() == RuntimeState::Running &&
                GetRuntimeGeneration() == generation,
            "worker-failure path touched ProfileDump or retained destructor ownership"
        );
    }

    Expect(
        GetRuntimeState() == RuntimeState::Running &&
            GetRuntimeGeneration() == generation,
        "controller destructor retried unsafe runtime finalization after worker failure"
    );
}

void ClosingAdmissionCancelsQueuedTickets() {
    ScopedCaptureOutput      output("moer-profile-controller-cancel-queue");
    RenderProfileCapture     capture;
    ProfileCaptureController controller(&capture, 4);

    ProfileCaptureRequestSubmission queued_start =
        controller.RequestStart(StartOptions(output, "never-started.mpd"));
    ProfileCaptureRequestSubmission queued_stop =
        controller.RequestStop(42);
    Expect(
        queued_start.result == ProfileCaptureSubmitResult::Queued &&
            queued_stop.result == ProfileCaptureSubmitResult::Queued &&
            controller.GetSnapshot().queued_requests == 2,
        "shutdown cancellation setup did not retain both queued requests"
    );
    Expect(
        controller.BeginEngineShutdown() ==
            ProfileCaptureLifecycleResult::Advanced,
        "idle shutdown did not close request admission"
    );
    const ProfileCaptureRequestCompletion start_completion =
        CompletionOf(
            queued_start.ticket,
            "shutdown did not cancel a queued start ticket"
        );
    const ProfileCaptureRequestCompletion stop_completion =
        CompletionOf(
            queued_stop.ticket,
            "shutdown did not cancel a queued stop ticket"
        );
    Expect(
        start_completion.status ==
                ProfileCaptureCompletionStatus::CancelledForShutdown &&
            stop_completion.status ==
                ProfileCaptureCompletionStatus::CancelledForShutdown &&
            controller.GetSnapshot().queued_requests == 0 &&
            GetRuntimeState() == RuntimeState::Stopped,
        "closing admission did not cancel the entire queued FIFO"
    );

    ProfileCaptureRequestSubmission rejected =
        controller.RequestStart(StartOptions(output, "after-close.mpd"));
    Expect(
        rejected.result == ProfileCaptureSubmitResult::AdmissionClosed,
        "closed controller admitted a new capture request"
    );
    const ProfileCaptureRequestCompletion rejected_completion =
        CompletionOf(
            rejected.ticket,
            "closed-admission request did not complete synchronously"
        );
    Expect(
        rejected_completion.status ==
            ProfileCaptureCompletionStatus::RejectedAdmissionClosed,
        "closed-admission ticket lost its rejection reason"
    );
    FinishEngineShutdown(controller);
}

} // namespace

int main() {
    try {
        AdmissionValidationQueueCapacityAndFifo();
        ConcurrentSubmissionIdsAreUniqueAndQueueIsBounded();
        ReadyTicketPublishesCompletionAccounting();
        ExternalRuntimeIsNeverAdoptedOrShutdown();
        ControllerDestructorClosesOwnedRunningSession();
        ControllerDestructorClosesOwnedStoppingSession();
        NullFacadeRunsAndStopsCpuOnlyCapture();
        NullFacadePreservesEngineShutdownSplit();
        NullFacadeDestructorClosesOwnedCpuOnlyCapture();
        RestartUsesStableFacadeAndRejectsStaleStop();
        AdmittedFrameKeepsDynamicStopPendingUntilSealed();
        EngineShutdownPreservesRhiAndWorkerOwnershipBoundaries();
        DynamicStopReportsProfileDumpRecordLoss();
        EngineShutdownReportsProfileDumpRecordLoss();
        WorkerShutdownFailureAbandonsRuntimeWithoutUnsafeFinalization();
        ClosingAdmissionCancelsQueuedTickets();
    } catch (const std::exception& error) {
        std::cerr << "ProfileCaptureControllerContract: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ProfileCaptureControllerContract: all checks passed\n";
    return 0;
}
