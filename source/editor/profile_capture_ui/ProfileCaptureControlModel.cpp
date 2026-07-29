#include "profile_capture_ui/ProfileCaptureControlModel.h"

#include <utility>

namespace Moer {
namespace {

[[nodiscard]] bool IsCoherentRejection(
    Render::ProfileCaptureSubmitResult      result,
    Render::ProfileCaptureCompletionStatus status
) noexcept {
    switch (result) {
        case Render::ProfileCaptureSubmitResult::AdmissionClosed:
            return status ==
                   Render::ProfileCaptureCompletionStatus::
                       RejectedAdmissionClosed;
        case Render::ProfileCaptureSubmitResult::QueueFull:
            return status ==
                   Render::ProfileCaptureCompletionStatus::RejectedQueueFull;
        case Render::ProfileCaptureSubmitResult::InvalidArgument:
            return status ==
                   Render::ProfileCaptureCompletionStatus::
                       RejectedInvalidArgument;
        case Render::ProfileCaptureSubmitResult::ResourceExhausted:
            return status ==
                   Render::ProfileCaptureCompletionStatus::
                       RejectedResourceExhausted;
        case Render::ProfileCaptureSubmitResult::Queued:
            return false;
    }
    return false;
}

} // namespace

ProfileCaptureControlModel::ProfileCaptureControlModel(
    ProfileCaptureControlCallbacks callbacks
) noexcept :
    callbacks_(std::move(callbacks)) {
    Refresh();
}

void ProfileCaptureControlModel::Refresh() noexcept {
    if (has_pending_request_) {
        Render::ProfileCaptureRequestCompletion completion;
        if (pending_ticket_.TryGet(completion)) {
            if (completion.kind == pending_kind_) {
                last_completion_ = completion;
                last_action_result_ =
                    EProfileCaptureControlActionResult::Completed;
            } else {
                last_action_result_ =
                    EProfileCaptureControlActionResult::InvalidSubmission;
            }
            pending_ticket_     = {};
            pending_expected_generation_ = 0;
            has_pending_request_ = false;
        }
    }

    if (!callbacks_.get_snapshot) {
        snapshot_ = {};
        snapshot_.state = Render::ProfileCaptureControllerState::Shutdown;
        snapshot_.accepting_requests = false;
        snapshot_status_ = SnapshotStatus::CallbackUnavailable;
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackUnavailable;
        return;
    }

    try {
        snapshot_ = callbacks_.get_snapshot();
        snapshot_status_ = SnapshotStatus::Available;
    } catch (...) {
        snapshot_ = {};
        snapshot_.state = Render::ProfileCaptureControllerState::Shutdown;
        snapshot_.accepting_requests = false;
        snapshot_status_ = SnapshotStatus::CallbackFailed;
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackFailed;
    }
}

EProfileCaptureControlActionResult ProfileCaptureControlModel::RequestStart(
    Render::ProfileCaptureStartOptions options
) noexcept {
    Refresh();
    if (has_pending_request_) {
        last_action_result_ =
            EProfileCaptureControlActionResult::PendingRequest;
        return last_action_result_;
    }
    if (snapshot_status_ == SnapshotStatus::CallbackUnavailable) {
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackUnavailable;
        return last_action_result_;
    }
    if (snapshot_status_ == SnapshotStatus::CallbackFailed) {
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackFailed;
        return last_action_result_;
    }
    if (options.runtime.output_path.empty()) {
        last_action_result_ =
            EProfileCaptureControlActionResult::InvalidOutputPath;
        return last_action_result_;
    }
    if (!CanRequestStart()) {
        last_action_result_ =
            EProfileCaptureControlActionResult::InvalidControllerState;
        return last_action_result_;
    }
    if (!callbacks_.request_start) {
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackUnavailable;
        return last_action_result_;
    }

    try {
        return AcceptSubmission(
            callbacks_.request_start(std::move(options)),
            Render::ProfileCaptureRequestKind::Start,
            0
        );
    } catch (...) {
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackFailed;
        return last_action_result_;
    }
}

EProfileCaptureControlActionResult
ProfileCaptureControlModel::RequestStop() noexcept {
    Refresh();
    if (has_pending_request_) {
        last_action_result_ =
            EProfileCaptureControlActionResult::PendingRequest;
        return last_action_result_;
    }
    if (snapshot_status_ == SnapshotStatus::CallbackUnavailable) {
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackUnavailable;
        return last_action_result_;
    }
    if (snapshot_status_ == SnapshotStatus::CallbackFailed) {
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackFailed;
        return last_action_result_;
    }
    if (!CanRequestStop()) {
        last_action_result_ =
            EProfileCaptureControlActionResult::InvalidControllerState;
        return last_action_result_;
    }
    if (!callbacks_.request_stop) {
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackUnavailable;
        return last_action_result_;
    }

    try {
        const std::uint64_t expected_generation =
            snapshot_.active_generation;
        return AcceptSubmission(
            callbacks_.request_stop(expected_generation),
            Render::ProfileCaptureRequestKind::Stop,
            expected_generation
        );
    } catch (...) {
        last_action_result_ =
            EProfileCaptureControlActionResult::CallbackFailed;
        return last_action_result_;
    }
}

bool ProfileCaptureControlModel::CanRequestStart() const noexcept {
    return snapshot_status_ == SnapshotStatus::Available &&
           !has_pending_request_ && snapshot_.accepting_requests &&
           snapshot_.state == Render::ProfileCaptureControllerState::Idle &&
           !snapshot_.owns_runtime && snapshot_.active_generation == 0;
}

bool ProfileCaptureControlModel::CanRequestStop() const noexcept {
    return snapshot_status_ == SnapshotStatus::Available &&
           !has_pending_request_ && snapshot_.accepting_requests &&
           snapshot_.state == Render::ProfileCaptureControllerState::Running &&
           snapshot_.owns_runtime && snapshot_.active_generation != 0;
}

bool ProfileCaptureControlModel::HasPendingRequest() const noexcept {
    return has_pending_request_;
}

Render::ProfileCaptureRequestKind
ProfileCaptureControlModel::PendingRequestKind() const noexcept {
    return pending_kind_;
}

std::uint64_t
ProfileCaptureControlModel::PendingExpectedGeneration() const noexcept {
    return pending_expected_generation_;
}

const Render::ProfileCaptureControllerSnapshot&
ProfileCaptureControlModel::Snapshot() const noexcept {
    return snapshot_;
}

const std::optional<Render::ProfileCaptureRequestCompletion>&
ProfileCaptureControlModel::LastCompletion() const noexcept {
    return last_completion_;
}

EProfileCaptureControlActionResult
ProfileCaptureControlModel::LastActionResult() const noexcept {
    return last_action_result_;
}

Render::ProfileCaptureSubmitResult
ProfileCaptureControlModel::LastSubmitResult() const noexcept {
    return last_submit_result_;
}

EProfileCaptureControlActionResult
ProfileCaptureControlModel::AcceptSubmission(
    Render::ProfileCaptureRequestSubmission submission,
    Render::ProfileCaptureRequestKind       kind,
    std::uint64_t                           expected_generation
) noexcept {
    last_submit_result_ = submission.result;
    if (submission.result != Render::ProfileCaptureSubmitResult::Queued) {
        last_completion_.reset();
        if (!submission.ticket.Valid()) {
            // Completion-state allocation and lock acquisition can fail
            // before the controller has a request id to publish. That
            // deliberately ticketless ResourceExhausted result is the only
            // well-formed rejection without a terminal ticket.
            if (submission.result ==
                Render::ProfileCaptureSubmitResult::ResourceExhausted) {
                last_action_result_ =
                    EProfileCaptureControlActionResult::BackendRejected;
            } else {
                last_action_result_ =
                    EProfileCaptureControlActionResult::InvalidSubmission;
            }
            return last_action_result_;
        }

        Render::ProfileCaptureRequestCompletion completion;
        if (!submission.ticket.TryGet(completion) ||
            completion.kind != kind ||
            !IsCoherentRejection(submission.result, completion.status)) {
            last_action_result_ =
                EProfileCaptureControlActionResult::InvalidSubmission;
            return last_action_result_;
        }
        last_completion_ = completion;
        last_action_result_ =
            EProfileCaptureControlActionResult::BackendRejected;
        return last_action_result_;
    }
    if (!submission.ticket.Valid()) {
        last_action_result_ =
            EProfileCaptureControlActionResult::InvalidSubmission;
        return last_action_result_;
    }

    pending_ticket_              = std::move(submission.ticket);
    pending_kind_                = kind;
    pending_expected_generation_ = expected_generation;
    has_pending_request_         = true;
    last_action_result_          =
        EProfileCaptureControlActionResult::Submitted;
    return last_action_result_;
}

} // namespace Moer
