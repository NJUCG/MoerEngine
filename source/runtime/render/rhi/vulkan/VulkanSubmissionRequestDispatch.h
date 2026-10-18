#pragma once

#include <cstdint>
#include <exception>
#include <utility>

namespace Moer::Render::VulkanSubmissionDetail {

enum class EWorkerRequestKind : uint8_t {
    Submit,
    Sync,
    Stop,
};

enum class EWorkerRequestFailurePhase : uint8_t {
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
// owned by the request, while Sync/Stop must release their waiters. Stop is
// terminal even when its final Sync fails.
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
    bool failed = false;

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
        failed = true;
        const std::exception_ptr process_exception = std::current_exception();
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
            _complete();
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
