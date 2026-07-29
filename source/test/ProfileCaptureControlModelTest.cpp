#include "profile_capture_ui/ProfileCaptureControlModel.h"

#include "profile/ProfileDump.h"
#include "profile/ProfileScope.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace Moer;
using namespace Moer::ProfileDump;
using namespace Moer::Render;

void Expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class ScopedCaptureOutput final {
public:
    ScopedCaptureOutput() {
        static std::atomic_uint64_t next_id{0};
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto now = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count()
            );
            directory_ =
                std::filesystem::temp_directory_path() /
                ("moer-profile-control-model-" + std::to_string(now) + "-" +
                 std::to_string(
                     next_id.fetch_add(1, std::memory_order_relaxed)
                 ));
            std::error_code error;
            if (std::filesystem::create_directory(directory_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error(
            "ProfileCaptureControlModel test could not reserve a temp directory"
        );
    }

    ~ScopedCaptureOutput() {
        CpuScopeProducer::Deactivate();
        static_cast<void>(Shutdown());
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] std::filesystem::path File() const {
        return directory_ / "capture.mpd";
    }

private:
    std::filesystem::path directory_{};
};

ProfileCaptureControlModel MakeModel(
    ProfileCaptureController& controller
) {
    return ProfileCaptureControlModel(
        ProfileCaptureControlCallbacks{
            .request_start =
                [&controller](ProfileCaptureStartOptions options) {
                    return controller.RequestStart(std::move(options));
                },
            .request_stop =
                [&controller](std::uint64_t generation) {
                    return controller.RequestStop(generation);
                },
            .get_snapshot =
                [&controller]() {
                    return controller.GetSnapshot();
                },
        }
    );
}

void CpuOnlyStartStopUsesEngineOwnedTicketBoundary() {
    ScopedCaptureOutput      output;
    ProfileCaptureController controller(nullptr, 4);
    ProfileCaptureControlModel model = MakeModel(controller);

    Expect(
        model.CanRequestStart() && !model.CanRequestStop(),
        "idle control model did not expose only Start"
    );

    ProfileCaptureStartOptions options;
    options.runtime.output_path      = output.File();
    options.runtime.replace_existing = true;
    Expect(
        model.RequestStart(std::move(options)) ==
                EProfileCaptureControlActionResult::Submitted &&
            model.HasPendingRequest() &&
            model.PendingRequestKind() == ProfileCaptureRequestKind::Start,
        "control model did not retain the queued Start ticket"
    );
    Expect(
        model.RequestStop() ==
            EProfileCaptureControlActionResult::PendingRequest,
        "control model admitted a second UI action while Start was pending"
    );

    model.Refresh();
    Expect(
        model.HasPendingRequest() &&
            model.Snapshot().state == ProfileCaptureControllerState::Idle,
        "control model advanced controller lifecycle from the UI refresh"
    );

    Expect(
        controller.TickOwner() == ProfileCaptureTickResult::Progressed,
        "Engine owner tick did not process the UI Start request"
    );
    model.Refresh();
    Expect(
        !model.HasPendingRequest() && model.CanRequestStop() &&
            model.Snapshot().state ==
                ProfileCaptureControllerState::Running &&
            model.Snapshot().active_generation != 0 &&
            model.LastCompletion().has_value() &&
            model.LastCompletion()->status ==
                ProfileCaptureCompletionStatus::StartedCpuOnly,
        "control model did not observe the CPU-only Start completion"
    );
    const std::uint64_t generation =
        model.Snapshot().active_generation;

    Expect(
        model.RequestStop() ==
                EProfileCaptureControlActionResult::Submitted &&
            model.HasPendingRequest() &&
            model.PendingRequestKind() == ProfileCaptureRequestKind::Stop &&
            model.PendingExpectedGeneration() == generation,
        "control model did not submit exact-generation Stop"
    );
    Expect(
        controller.TickOwner() == ProfileCaptureTickResult::Progressed,
        "Engine owner tick did not process the UI Stop request"
    );
    model.Refresh();
    Expect(
        !model.HasPendingRequest() && model.CanRequestStart() &&
            model.Snapshot().state == ProfileCaptureControllerState::Idle &&
            model.LastActionResult() ==
                EProfileCaptureControlActionResult::Completed &&
            model.LastCompletion().has_value() &&
            model.LastCompletion()->generation == generation &&
            model.LastCompletion()->status ==
                ProfileCaptureCompletionStatus::Stopped &&
            model.PendingExpectedGeneration() == 0 &&
            GetRuntimeState() == RuntimeState::Stopped,
        "control model did not observe the exact-generation Stop completion"
    );
}

void LocalValidationAndMalformedRejectionStayBounded() {
    std::uint64_t start_calls = 0;
    ProfileCaptureControllerSnapshot snapshot;
    snapshot.state              = ProfileCaptureControllerState::Idle;
    snapshot.accepting_requests = true;
    snapshot.queue_capacity     = 4;

    ProfileCaptureControlModel model(
        ProfileCaptureControlCallbacks{
            .request_start =
                [&start_calls](ProfileCaptureStartOptions) {
                    ++start_calls;
                    return ProfileCaptureRequestSubmission{
                        .result = ProfileCaptureSubmitResult::QueueFull,
                        .ticket = {},
                    };
                },
            .request_stop = {},
            .get_snapshot =
                [&snapshot]() {
                    return snapshot;
                },
        }
    );

    Expect(
        model.RequestStart(ProfileCaptureStartOptions{}) ==
                EProfileCaptureControlActionResult::InvalidOutputPath &&
            start_calls == 0,
        "empty output path reached the Engine callback"
    );

    ProfileCaptureStartOptions options;
    options.runtime.output_path = "capture.mpd";
    Expect(
        model.RequestStart(std::move(options)) ==
                EProfileCaptureControlActionResult::InvalidSubmission &&
            model.LastSubmitResult() ==
                ProfileCaptureSubmitResult::QueueFull &&
            start_calls == 1 && !model.HasPendingRequest(),
        "ticketless QueueFull submission was not rejected as malformed"
    );

    snapshot.state              = ProfileCaptureControllerState::Running;
    snapshot.owns_runtime       = true;
    snapshot.active_generation  = 7;
    model.Refresh();
    Expect(
        model.RequestStop() ==
            EProfileCaptureControlActionResult::CallbackUnavailable,
        "missing Stop callback was not reported without mutating state"
    );
}

void SynchronousRejectionRetainsTerminalCompletion() {
    ScopedCaptureOutput      output;
    ProfileCaptureController controller(nullptr, 0);
    ProfileCaptureControlModel model = MakeModel(controller);

    ProfileCaptureStartOptions options;
    options.runtime.output_path = output.File();
    Expect(
        model.RequestStart(std::move(options)) ==
                EProfileCaptureControlActionResult::BackendRejected &&
            model.LastSubmitResult() == ProfileCaptureSubmitResult::QueueFull &&
            model.LastCompletion().has_value() &&
            model.LastCompletion()->kind ==
                ProfileCaptureRequestKind::Start &&
            model.LastCompletion()->status ==
                ProfileCaptureCompletionStatus::RejectedQueueFull &&
            !model.HasPendingRequest(),
        "synchronous rejection lost its terminal ticket payload"
    );
}

void SynchronousRejectionProtocolFailsClosed() {
    ProfileCaptureControllerSnapshot idle_snapshot;
    idle_snapshot.state              = ProfileCaptureControllerState::Idle;
    idle_snapshot.accepting_requests = true;

    ProfileCaptureControlModel allocation_failure(
        ProfileCaptureControlCallbacks{
            .request_start =
                [](ProfileCaptureStartOptions) {
                    return ProfileCaptureRequestSubmission{
                        .result =
                            ProfileCaptureSubmitResult::ResourceExhausted,
                        .ticket = {},
                    };
                },
            .request_stop = {},
            .get_snapshot =
                [&idle_snapshot]() {
                    return idle_snapshot;
                },
        }
    );
    ProfileCaptureStartOptions allocation_options;
    allocation_options.runtime.output_path = "capture.mpd";
    Expect(
        allocation_failure.RequestStart(std::move(allocation_options)) ==
                EProfileCaptureControlActionResult::BackendRejected &&
            allocation_failure.LastSubmitResult() ==
                ProfileCaptureSubmitResult::ResourceExhausted &&
            !allocation_failure.LastCompletion().has_value(),
        "ticketless allocation failure was not retained as the explicit "
        "ResourceExhausted exception"
    );

    ProfileCaptureController pending_controller(nullptr, 4);
    ProfileCaptureControlModel non_terminal_rejection(
        ProfileCaptureControlCallbacks{
            .request_start =
                [&pending_controller](ProfileCaptureStartOptions options) {
                    ProfileCaptureRequestSubmission submission =
                        pending_controller.RequestStart(std::move(options));
                    submission.result = ProfileCaptureSubmitResult::QueueFull;
                    return submission;
                },
            .request_stop = {},
            .get_snapshot =
                [&idle_snapshot]() {
                    return idle_snapshot;
                },
        }
    );
    ProfileCaptureStartOptions pending_options;
    pending_options.runtime.output_path = "capture.mpd";
    Expect(
        non_terminal_rejection.RequestStart(std::move(pending_options)) ==
                EProfileCaptureControlActionResult::InvalidSubmission &&
            !non_terminal_rejection.HasPendingRequest() &&
            !non_terminal_rejection.LastCompletion().has_value(),
        "non-terminal ticket attached to a synchronous rejection was accepted"
    );

    ProfileCaptureController mismatch_controller(nullptr, 4);
    ProfileCaptureControllerSnapshot running_snapshot;
    running_snapshot.state              =
        ProfileCaptureControllerState::Running;
    running_snapshot.accepting_requests = true;
    running_snapshot.owns_runtime       = true;
    running_snapshot.active_generation  = 7;
    ProfileCaptureControlModel mismatched_status(
        ProfileCaptureControlCallbacks{
            .request_start = {},
            .request_stop =
                [&mismatch_controller](std::uint64_t) {
                    ProfileCaptureRequestSubmission submission =
                        mismatch_controller.RequestStop(0);
                    submission.result = ProfileCaptureSubmitResult::QueueFull;
                    return submission;
                },
            .get_snapshot =
                [&running_snapshot]() {
                    return running_snapshot;
                },
        }
    );
    Expect(
        mismatched_status.RequestStop() ==
                EProfileCaptureControlActionResult::InvalidSubmission &&
            !mismatched_status.HasPendingRequest() &&
            !mismatched_status.LastCompletion().has_value(),
        "submit result and terminal completion status mismatch was accepted"
    );
}

void SnapshotFailuresStayDistinctFromControllerState() {
    ProfileCaptureStartOptions options;
    options.runtime.output_path = "capture.mpd";

    ProfileCaptureControlModel missing_snapshot(
        ProfileCaptureControlCallbacks{}
    );
    Expect(
        missing_snapshot.RequestStart(options) ==
            EProfileCaptureControlActionResult::CallbackUnavailable,
        "missing snapshot callback was reported as a controller state error"
    );

    ProfileCaptureControlModel throwing_snapshot(
        ProfileCaptureControlCallbacks{
            .request_start =
                [](ProfileCaptureStartOptions) {
                    return ProfileCaptureRequestSubmission{};
                },
            .request_stop = {},
            .get_snapshot =
                []() -> ProfileCaptureControllerSnapshot {
                    throw std::runtime_error("injected snapshot failure");
                },
        }
    );
    Expect(
        throwing_snapshot.RequestStart(std::move(options)) ==
            EProfileCaptureControlActionResult::CallbackFailed,
        "throwing snapshot callback was reported as a controller state error"
    );

    ProfileCaptureControllerSnapshot idle_snapshot;
    idle_snapshot.state              = ProfileCaptureControllerState::Idle;
    idle_snapshot.accepting_requests = true;
    ProfileCaptureControlModel throwing_request(
        ProfileCaptureControlCallbacks{
            .request_start =
                [](ProfileCaptureStartOptions)
                    -> ProfileCaptureRequestSubmission {
                    throw std::runtime_error("injected request failure");
                },
            .request_stop = {},
            .get_snapshot =
                [&idle_snapshot]() {
                    return idle_snapshot;
                },
        }
    );
    ProfileCaptureStartOptions throwing_options;
    throwing_options.runtime.output_path = "capture.mpd";
    Expect(
        throwing_request.RequestStart(std::move(throwing_options)) ==
            EProfileCaptureControlActionResult::CallbackFailed,
        "throwing request callback escaped the control model"
    );

    ProfileCaptureControlModel missing_ticket(
        ProfileCaptureControlCallbacks{
            .request_start =
                [](ProfileCaptureStartOptions) {
                    return ProfileCaptureRequestSubmission{
                        .result = ProfileCaptureSubmitResult::Queued,
                        .ticket = {},
                    };
                },
            .request_stop = {},
            .get_snapshot =
                [&idle_snapshot]() {
                    return idle_snapshot;
                },
        }
    );
    ProfileCaptureStartOptions missing_ticket_options;
    missing_ticket_options.runtime.output_path = "capture.mpd";
    Expect(
        missing_ticket.RequestStart(std::move(missing_ticket_options)) ==
                EProfileCaptureControlActionResult::InvalidSubmission &&
            !missing_ticket.HasPendingRequest(),
        "queued submission without a ticket was not rejected"
    );
}

void MismatchedTicketKindFailsClosed() {
    ProfileCaptureController controller(nullptr, 4);
    ProfileCaptureControllerSnapshot snapshot;
    snapshot.state              = ProfileCaptureControllerState::Idle;
    snapshot.accepting_requests = true;
    snapshot.queue_capacity     = 4;

    ProfileCaptureControlModel model(
        ProfileCaptureControlCallbacks{
            .request_start =
                [&controller](ProfileCaptureStartOptions) {
                    return controller.RequestStop(1);
                },
            .request_stop = {},
            .get_snapshot =
                [&snapshot]() {
                    return snapshot;
                },
        }
    );

    ProfileCaptureStartOptions options;
    options.runtime.output_path = "capture.mpd";
    Expect(
        model.RequestStart(std::move(options)) ==
                EProfileCaptureControlActionResult::Submitted &&
            model.HasPendingRequest(),
        "mismatched-ticket fixture did not queue its request"
    );
    Expect(
        controller.TickOwner() == ProfileCaptureTickResult::Progressed,
        "mismatched-ticket fixture was not completed"
    );
    model.Refresh();
    Expect(
        !model.HasPendingRequest() &&
            model.LastActionResult() ==
                EProfileCaptureControlActionResult::InvalidSubmission &&
            !model.LastCompletion().has_value(),
        "mismatched ticket kind was accepted as a UI completion"
    );
}

void StaleUiStopProtectsTheNewGeneration() {
    ScopedCaptureOutput      output;
    ProfileCaptureController controller(nullptr, 4);

    ProfileCaptureStartOptions start_options;
    start_options.runtime.output_path      = output.File();
    start_options.runtime.replace_existing = true;
    ProfileCaptureRequestSubmission start_a =
        controller.RequestStart(start_options);
    Expect(
        start_a.result == ProfileCaptureSubmitResult::Queued &&
            controller.TickOwner() == ProfileCaptureTickResult::Progressed,
        "stale UI Stop fixture could not start generation A"
    );
    ProfileCaptureRequestCompletion start_a_completion;
    Expect(
        start_a.ticket.TryGet(start_a_completion) &&
            start_a_completion.status ==
                ProfileCaptureCompletionStatus::StartedCpuOnly,
        "stale UI Stop fixture did not complete generation A Start"
    );
    const std::uint64_t generation_a = start_a_completion.generation;

    ProfileCaptureRequestSubmission stop_a =
        controller.RequestStop(generation_a);
    Expect(
        stop_a.result == ProfileCaptureSubmitResult::Queued &&
            controller.TickOwner() == ProfileCaptureTickResult::Progressed,
        "stale UI Stop fixture could not stop generation A"
    );
    ProfileCaptureRequestCompletion stop_a_completion;
    Expect(
        stop_a.ticket.TryGet(stop_a_completion) &&
            stop_a_completion.status ==
                ProfileCaptureCompletionStatus::Stopped,
        "stale UI Stop fixture did not complete generation A Stop"
    );

    ProfileCaptureRequestSubmission start_b =
        controller.RequestStart(std::move(start_options));
    Expect(
        start_b.result == ProfileCaptureSubmitResult::Queued &&
            controller.TickOwner() == ProfileCaptureTickResult::Progressed,
        "stale UI Stop fixture could not start generation B"
    );
    ProfileCaptureRequestCompletion start_b_completion;
    Expect(
        start_b.ticket.TryGet(start_b_completion) &&
            start_b_completion.status ==
                ProfileCaptureCompletionStatus::StartedCpuOnly &&
            start_b_completion.generation > generation_a,
        "stale UI Stop fixture did not publish a newer generation B"
    );
    const std::uint64_t generation_b = start_b_completion.generation;

    ProfileCaptureControllerSnapshot stale_snapshot =
        controller.GetSnapshot();
    stale_snapshot.active_generation = generation_a;
    ProfileCaptureControlModel model(
        ProfileCaptureControlCallbacks{
            .request_start = {},
            .request_stop =
                [&controller](std::uint64_t generation) {
                    return controller.RequestStop(generation);
                },
            .get_snapshot =
                [&stale_snapshot]() {
                    return stale_snapshot;
                },
        }
    );

    Expect(
        model.RequestStop() ==
                EProfileCaptureControlActionResult::Submitted &&
            model.PendingExpectedGeneration() == generation_a,
        "UI did not retain the stale generation A Stop intent"
    );
    Expect(
        controller.TickOwner() == ProfileCaptureTickResult::Progressed,
        "controller did not complete the stale generation A Stop"
    );
    stale_snapshot = controller.GetSnapshot();
    model.Refresh();
    Expect(
        !model.HasPendingRequest() &&
            model.LastActionResult() ==
                EProfileCaptureControlActionResult::Completed &&
            model.LastCompletion().has_value() &&
            model.LastCompletion()->status ==
                ProfileCaptureCompletionStatus::RejectedStaleGeneration &&
            model.LastCompletion()->generation == generation_b &&
            model.Snapshot().state ==
                ProfileCaptureControllerState::Running &&
            model.Snapshot().active_generation == generation_b &&
            model.CanRequestStop(),
        "stale UI Stop did not preserve and expose running generation B"
    );

    ProfileCaptureRequestSubmission stop_b =
        controller.RequestStop(generation_b);
    Expect(
        stop_b.result == ProfileCaptureSubmitResult::Queued &&
            controller.TickOwner() == ProfileCaptureTickResult::Progressed,
        "stale UI Stop fixture could not cleanly stop generation B"
    );
    ProfileCaptureRequestCompletion stop_b_completion;
    Expect(
        stop_b.ticket.TryGet(stop_b_completion) &&
            stop_b_completion.status ==
                ProfileCaptureCompletionStatus::Stopped,
        "stale UI Stop fixture did not cleanly complete generation B"
    );
}

} // namespace

int main() {
    try {
        CpuOnlyStartStopUsesEngineOwnedTicketBoundary();
        LocalValidationAndMalformedRejectionStayBounded();
        SynchronousRejectionRetainsTerminalCompletion();
        SynchronousRejectionProtocolFailsClosed();
        SnapshotFailuresStayDistinctFromControllerState();
        MismatchedTicketKindFailsClosed();
        StaleUiStopProtectsTheNewGeneration();
    } catch (const std::exception& error) {
        std::cerr << "ProfileCaptureControlModel contract failed: "
                  << error.what() << '\n';
        return 1;
    }

    std::cout << "ProfileCaptureControlModel contract passed\n";
    return 0;
}
