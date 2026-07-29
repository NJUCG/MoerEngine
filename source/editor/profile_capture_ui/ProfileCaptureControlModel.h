#pragma once

#include "ProfileCaptureController.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace Moer {

enum class EProfileCaptureControlActionResult : std::uint8_t {
    None = 0,
    Submitted,
    Completed,
    PendingRequest,
    InvalidOutputPath,
    InvalidControllerState,
    CallbackUnavailable,
    CallbackFailed,
    BackendRejected,
    InvalidSubmission,
};

struct ProfileCaptureControlCallbacks {
    std::function<Render::ProfileCaptureRequestSubmission(
        Render::ProfileCaptureStartOptions
    )>
        request_start;
    std::function<Render::ProfileCaptureRequestSubmission(std::uint64_t)>
        request_stop;
    std::function<Render::ProfileCaptureControllerSnapshot()> get_snapshot;
};

// Editor-side intent model. It owns only a single UI request ticket and never
// advances capture lifecycle state itself; Engine remains the Game Thread
// owner and the sole caller of ProfileCaptureController::TickOwner().
class ProfileCaptureControlModel final {
public:
    explicit ProfileCaptureControlModel(
        ProfileCaptureControlCallbacks callbacks
    ) noexcept;

    void Refresh() noexcept;

    [[nodiscard]] EProfileCaptureControlActionResult RequestStart(
        Render::ProfileCaptureStartOptions options
    ) noexcept;
    [[nodiscard]] EProfileCaptureControlActionResult RequestStop() noexcept;

    [[nodiscard]] bool CanRequestStart() const noexcept;
    [[nodiscard]] bool CanRequestStop() const noexcept;
    [[nodiscard]] bool HasPendingRequest() const noexcept;
    [[nodiscard]] Render::ProfileCaptureRequestKind
    PendingRequestKind() const noexcept;
    [[nodiscard]] std::uint64_t
    PendingExpectedGeneration() const noexcept;

    [[nodiscard]] const Render::ProfileCaptureControllerSnapshot&
    Snapshot() const noexcept;
    [[nodiscard]] const std::optional<Render::ProfileCaptureRequestCompletion>&
    LastCompletion() const noexcept;
    [[nodiscard]] EProfileCaptureControlActionResult
    LastActionResult() const noexcept;
    [[nodiscard]] Render::ProfileCaptureSubmitResult
    LastSubmitResult() const noexcept;

private:
    [[nodiscard]] EProfileCaptureControlActionResult AcceptSubmission(
        Render::ProfileCaptureRequestSubmission submission,
        Render::ProfileCaptureRequestKind       kind,
        std::uint64_t                           expected_generation
    ) noexcept;

    enum class SnapshotStatus : std::uint8_t {
        Available = 0,
        CallbackUnavailable,
        CallbackFailed,
    };

    ProfileCaptureControlCallbacks callbacks_{};
    Render::ProfileCaptureControllerSnapshot snapshot_{};
    Render::ProfileCaptureRequestTicket       pending_ticket_{};
    Render::ProfileCaptureRequestKind pending_kind_{
        Render::ProfileCaptureRequestKind::Start
    };
    std::uint64_t pending_expected_generation_{0};
    bool has_pending_request_{false};
    SnapshotStatus snapshot_status_{SnapshotStatus::CallbackUnavailable};

    std::optional<Render::ProfileCaptureRequestCompletion> last_completion_{};
    EProfileCaptureControlActionResult last_action_result_{
        EProfileCaptureControlActionResult::None
    };
    Render::ProfileCaptureSubmitResult last_submit_result_{
        Render::ProfileCaptureSubmitResult::ResourceExhausted
    };
};

} // namespace Moer
