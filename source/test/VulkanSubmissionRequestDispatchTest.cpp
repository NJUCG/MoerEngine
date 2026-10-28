#include "rhi/backends/vulkan/VulkanSubmissionRequestDispatch.h"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace Moer::Render::VulkanSubmissionDetail;

namespace {

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

void SubmitExceptionRejectsEveryOutstandingObligation() {
    struct OutstandingObligations {
        bool callbacks{true};
        bool signals{true};
        bool present_receipt{true};
    } obligations;

    uint32_t process_count  = 0;
    uint32_t reject_count   = 0;
    uint32_t complete_count = 0;
    std::vector<EWorkerRequestFailurePhase> failures;

    const WorkerRequestDispatchResult result = DispatchWorkerRequestNoexcept(
        EWorkerRequestKind::Submit,
        [&] {
            ++process_count;
            throw std::runtime_error("injected Submit failure");
        },
        [&] {
            ++reject_count;
            obligations.callbacks       = false;
            obligations.signals         = false;
            obligations.present_receipt = false;
        },
        [&](const std::exception_ptr&) {
            ++complete_count;
        },
        [&](EWorkerRequestFailurePhase _phase, const std::exception_ptr&) {
            failures.push_back(_phase);
        }
    );

    Expect(result.failed, "Submit exception was not reported as failed");
    Expect(!result.stop, "Submit exception incorrectly stopped the worker");
    Expect(process_count == 1, "Submit process callback did not run exactly once");
    Expect(reject_count == 1, "Submit rejection callback did not run exactly once");
    Expect(complete_count == 0, "Submit incorrectly ran a Sync completion callback");
    Expect(
        !obligations.callbacks && !obligations.signals && !obligations.present_receipt,
        "Submit rejection left an outstanding request obligation"
    );
    Expect(
        failures.size() == 1 &&
            failures.front() == EWorkerRequestFailurePhase::Process,
        "Submit process failure was not diagnosed"
    );
}

void SubmitExceptionPreservesTerminalizedPrefix() {
    enum class ESourceState : uint8_t {
        Submitted,
        Pending,
        Rejected,
    };

    std::vector<ESourceState> sources{
        ESourceState::Submitted,
        ESourceState::Pending,
        ESourceState::Pending,
    };
    const size_t first_unconsumed_source = 1;

    (void)DispatchWorkerRequestNoexcept(
        EWorkerRequestKind::Submit,
        [] { throw std::runtime_error("injected second-source failure"); },
        [&] {
            for (size_t source_index = first_unconsumed_source;
                 source_index < sources.size();
                 ++source_index) {
                sources[source_index] = ESourceState::Rejected;
            }
        },
        [](const std::exception_ptr&) {},
        [](EWorkerRequestFailurePhase, const std::exception_ptr&) {}
    );

    Expect(
        sources[0] == ESourceState::Submitted,
        "exception cleanup rejected an already terminalized source"
    );
    Expect(
        sources[1] == ESourceState::Rejected &&
            sources[2] == ESourceState::Rejected,
        "exception cleanup did not reject the current and later sources"
    );
}

void SuccessfulSyncCompletesWithoutFailure() {
    uint32_t           complete_count = 0;
    uint32_t           reject_count   = 0;
    std::exception_ptr propagated_failure{};
    const WorkerRequestDispatchResult result = DispatchWorkerRequestNoexcept(
        EWorkerRequestKind::Sync,
        [] {},
        [&] {
            ++reject_count;
        },
        [&](const std::exception_ptr& _failure) {
            ++complete_count;
            propagated_failure = _failure;
        },
        [](EWorkerRequestFailurePhase, const std::exception_ptr&) {}
    );

    Expect(!result.failed, "successful Sync was reported as failed");
    Expect(!result.stop, "successful Sync incorrectly stopped the worker");
    Expect(reject_count == 0, "successful Sync incorrectly rejected a Submit");
    Expect(complete_count == 1, "successful Sync did not release its waiter exactly once");
    Expect(!propagated_failure, "successful Sync propagated a failure");
}

void SyncExceptionReachesCompletionDespiteDiagnosticFailure() {
    uint32_t           complete_count = 0;
    uint32_t           reject_count   = 0;
    std::exception_ptr propagated_failure{};
    std::vector<EWorkerRequestFailurePhase> failures;
    const std::exception_ptr injected_failure =
        std::make_exception_ptr(std::runtime_error("injected Sync failure"));

    const WorkerRequestDispatchResult result = DispatchWorkerRequestNoexcept(
        EWorkerRequestKind::Sync,
        [&] {
            std::rethrow_exception(injected_failure);
        },
        [&] {
            ++reject_count;
        },
        [&](const std::exception_ptr& _failure) {
            ++complete_count;
            propagated_failure = _failure;
        },
        [&](EWorkerRequestFailurePhase _phase, const std::exception_ptr&) {
            failures.push_back(_phase);
            throw std::runtime_error("injected diagnostic failure");
        }
    );

    Expect(result.failed, "Sync exception was not reported as failed");
    Expect(!result.stop, "Sync exception incorrectly stopped the worker");
    Expect(reject_count == 0, "Sync exception incorrectly rejected a Submit");
    Expect(complete_count == 1, "Sync exception did not release its waiter exactly once");
    Expect(propagated_failure == injected_failure, "Sync completion did not receive the original process exception");
    Expect(
        failures.size() == 1 && failures.front() == EWorkerRequestFailurePhase::Process,
        "Sync process failure was not diagnosed exactly once"
    );
    try {
        std::rethrow_exception(propagated_failure);
    } catch (const std::runtime_error& error) {
        Expect(
            std::string_view(error.what()) == "injected Sync failure",
            "Sync completion lost the process exception"
        );
    }
}

void StopProcessExceptionStillCompletesShutdown() {
    uint32_t           complete_count = 0;
    uint32_t           reject_count   = 0;
    std::exception_ptr propagated_failure{};
    const std::exception_ptr injected_failure =
        std::make_exception_ptr(std::runtime_error("injected Stop Sync failure"));

    const WorkerRequestDispatchResult result = DispatchWorkerRequestNoexcept(
        EWorkerRequestKind::Stop,
        [&] {
            std::rethrow_exception(injected_failure);
        },
        [&] {
            ++reject_count;
        },
        [&](const std::exception_ptr& _failure) {
            ++complete_count;
            propagated_failure = _failure;
        },
        [](EWorkerRequestFailurePhase, const std::exception_ptr&) {}
    );

    Expect(result.failed, "Stop process exception was not reported as failed");
    Expect(result.stop, "Stop process exception did not terminate the worker");
    Expect(reject_count == 0, "Stop process exception incorrectly rejected a Submit");
    Expect(complete_count == 1, "Stop process exception did not finish shutdown exactly once");
    Expect(propagated_failure == injected_failure, "Stop completion did not receive the original process exception");
}

void StopExceptionsAlwaysCompleteAndExit() {
    uint32_t complete_count = 0;
    std::vector<EWorkerRequestFailurePhase> failures;
    const WorkerRequestDispatchResult result = DispatchWorkerRequestNoexcept(
        EWorkerRequestKind::Stop,
        [] { throw std::runtime_error("injected Stop Sync failure"); },
        [] { throw std::runtime_error("Stop must not reject a Submit"); },
        [&](const std::exception_ptr&) {
            ++complete_count;
            throw std::runtime_error("injected completion failure");
        },
        [&](EWorkerRequestFailurePhase _phase, const std::exception_ptr&) {
            failures.push_back(_phase);
        }
    );

    Expect(result.failed, "Stop exceptions were not reported as failed");
    Expect(result.stop, "Stop exception did not preserve the terminal decision");
    Expect(complete_count == 1, "Stop did not invoke completion exactly once");
    Expect(
        failures.size() == 2 &&
            failures[0] == EWorkerRequestFailurePhase::Process &&
            failures[1] == EWorkerRequestFailurePhase::Complete,
        "Stop did not diagnose both process and completion failures"
    );
}

void FailureReportingCannotEscapeOrSkipRejection() {
    uint32_t reject_count = 0;
    const WorkerRequestDispatchResult result = DispatchWorkerRequestNoexcept(
        EWorkerRequestKind::Submit,
        [] { throw std::runtime_error("injected Submit failure"); },
        [&] {
            ++reject_count;
            throw std::runtime_error("injected rejection failure");
        },
        [](const std::exception_ptr&) {},
        [](EWorkerRequestFailurePhase, const std::exception_ptr&) {
            throw std::runtime_error("injected diagnostic failure");
        }
    );

    Expect(result.failed, "diagnostic containment lost the request failure");
    Expect(reject_count == 1, "diagnostic failure skipped Submit rejection");
}

} // namespace

int main() {
    try {
        SubmitExceptionRejectsEveryOutstandingObligation();
        SubmitExceptionPreservesTerminalizedPrefix();
        SuccessfulSyncCompletesWithoutFailure();
        SyncExceptionReachesCompletionDespiteDiagnosticFailure();
        StopProcessExceptionStillCompletesShutdown();
        StopExceptionsAlwaysCompleteAndExit();
        FailureReportingCannotEscapeOrSkipRejection();
        std::cout << "Vulkan submission request dispatch tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan submission request dispatch test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
