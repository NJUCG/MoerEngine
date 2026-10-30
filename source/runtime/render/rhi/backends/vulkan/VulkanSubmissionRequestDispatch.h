#pragma once

#include "misc/Types.h"
#include <exception>
#include <utility>

namespace Moer::Render::VulkanSubmissionDetail {

enum class EWorkerRequestKind : uint8 {
    Submit,
    Sync,
    Stop,
};

enum class EWorkerRequestFailurePhase : uint8 {
    Process,
    Reject,
    Complete,
};

struct WorkerRequestDispatchResult {
    bool failed{false};
    bool stop{false};
};

// The submission thread is a process-lifetime service. No request-owned
// exception may escape its entry point: Submit must reject the payload still
// owned by the request, while Sync/Stop pass the process exception (empty on
// success) to their completion callback to release waiters. Stop is terminal
// even when its final Sync fails.
template <
    typename ProcessFn,
    typename RejectFn,
    typename CompleteFn,
    typename ReportFailureFn>
[[nodiscard]] WorkerRequestDispatchResult DispatchWorkerRequestNoexcept(
    EWorkerRequestKind _kind,
    ProcessFn&&        _process,
    RejectFn&&         _reject,
    CompleteFn&&       _complete,
    ReportFailureFn&&  _report_failure
) noexcept {
    bool               failed = false;
    std::exception_ptr process_exception{};

    auto report_failure = [&](EWorkerRequestFailurePhase _phase,
                              std::exception_ptr          _exception) noexcept {
        try {
            _report_failure(_phase, _exception);
        } catch (...) {
            // Diagnostics are best-effort and must not punch through the
            // submission-thread exception boundary.
        }
    };

    try {
        _process();
    } catch (...) {
        failed            = true;
        process_exception = std::current_exception();
        report_failure(EWorkerRequestFailurePhase::Process, process_exception);

        if (_kind == EWorkerRequestKind::Submit) {
            try {
                _reject();
            } catch (...) {
                report_failure(
                    EWorkerRequestFailurePhase::Reject, std::current_exception()
                );
            }
        }
    }

    if (_kind != EWorkerRequestKind::Submit) {
        try {
            _complete(process_exception);
        } catch (...) {
            failed = true;
            report_failure(
                EWorkerRequestFailurePhase::Complete, std::current_exception()
            );
        }
    }

    return {
        .failed = failed,
        .stop   = _kind == EWorkerRequestKind::Stop,
    };
}

} // namespace Moer::Render::VulkanSubmissionDetail
