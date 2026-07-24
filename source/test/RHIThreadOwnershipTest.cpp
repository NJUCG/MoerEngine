#include "rhi/RHIThreadOwnership.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {

using namespace Moer::Render;
using namespace std::chrono_literals;

bool Expect(bool _condition, const char* _message) {
    if (_condition) {
        return true;
    }
    std::cerr << "RHIThreadOwnershipContract: " << _message << '\n';
    return false;
}

bool RoleScopesNestAndRestore() {
    bool okay = Expect(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Unknown,
        "new threads must begin without an RHI owner role"
    );
    okay &= Expect(
        IsRHIBlockingCallAllowedOnCurrentThread(),
        "ordinary callers must be allowed to use blocking RHI lifecycle calls"
    );

    {
        RHIThreadRoleScope executor_scope(ERHIThreadRole::Executor);
        okay &= Expect(
            GetCurrentRHIThreadRole() == ERHIThreadRole::Executor,
            "executor scope did not publish its role"
        );
        okay &= Expect(
            !IsRHIBlockingCallAllowedOnCurrentThread(),
            "executor owner was allowed to self-join the RHI pipeline"
        );
        {
            RHIThreadRoleScope submission_scope(ERHIThreadRole::Submission);
            okay &= Expect(
                GetCurrentRHIThreadRole() == ERHIThreadRole::Submission,
                "nested submission scope did not override the role"
            );
        }
        okay &= Expect(
            GetCurrentRHIThreadRole() == ERHIThreadRole::Executor,
            "nested scope did not restore the previous role"
        );
    }

    return okay &&
           Expect(
               GetCurrentRHIThreadRole() == ERHIThreadRole::Unknown,
               "outer scope did not restore the caller role"
           );
}

bool EveryPipelineOwnerRejectsBlockingCalls() {
    constexpr ERHIThreadRole owner_roles[] = {
        ERHIThreadRole::Executor,
        ERHIThreadRole::Translate,
        ERHIThreadRole::Submission,
        ERHIThreadRole::Completion,
        ERHIThreadRole::RecordWorker,
    };
    for (ERHIThreadRole role : owner_roles) {
        RHIThreadRoleScope role_scope(role);
        if (!Expect(
                !IsRHIBlockingCallAllowedOnCurrentThread(),
                "an RHI stage owner was allowed to make a blocking lifecycle call"
            )) {
            return false;
        }
    }
    return true;
}

bool LeaseTransfersAcrossThreadsAndRemainsExclusive() {
    static_assert(
        !std::is_copy_constructible_v<RHITransferableOwnershipGate::Lease> &&
        !std::is_copy_assignable_v<RHITransferableOwnershipGate::Lease>
    );
    static_assert(
        std::is_nothrow_move_constructible_v<RHITransferableOwnershipGate::Lease> &&
        std::is_nothrow_move_assignable_v<RHITransferableOwnershipGate::Lease>
    );

    RHITransferableOwnershipGate gate{};
    auto                         first_lease = gate.Acquire();

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    release_first{false};
    std::atomic<bool>       contender_acquired{false};

    std::jthread release_thread(
        [lease = std::move(first_lease), &mutex, &cv, &release_first]() mutable {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&release_first] { return release_first; });
            lock.unlock();
            lease.Release();
        }
    );
    std::jthread contender([&] {
        auto lease = gate.Acquire();
        contender_acquired.store(true, std::memory_order_release);
        cv.notify_all();
    });

    std::this_thread::sleep_for(30ms);
    bool okay = Expect(
        !contender_acquired.load(std::memory_order_acquire),
        "a second owner entered before the transferred lease was released"
    );
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    cv.notify_all();

    {
        std::unique_lock lock(mutex);
        okay &= Expect(
            cv.wait_for(lock, 2s, [&] {
                return contender_acquired.load(std::memory_order_acquire);
            }),
            "cross-thread lease release did not wake the next owner"
        );
    }
    return okay;
}

bool DedicatedOwnerCanWaitForTaskGraphCompletion() {
    TaskGraph::Init();
    GraphEventRef completed = GraphEvent::CreateGraphEvent();
    completed->TryUnlockSubsequents();

    std::atomic<bool> wait_completed{false};
    std::exception_ptr wait_exception{};
    std::jthread waiter([&] {
        RHIThreadRoleScope executor_scope(ERHIThreadRole::Executor);
        try {
            completed->Wait(EThread::UNKNOWN_THREAD);
            wait_completed.store(true, std::memory_order_release);
        } catch (...) {
            wait_exception = std::current_exception();
        }
    });
    waiter.join();
    TaskGraph::Shutdown();

    return Expect(
               wait_exception == nullptr,
               "an unregistered dedicated owner could not wait for a GraphEvent"
           ) &&
           Expect(
               wait_completed.load(std::memory_order_acquire),
               "external GraphEvent wait did not observe completion"
           );
}

} // namespace

int main() {
    if (!RoleScopesNestAndRestore() || !EveryPipelineOwnerRejectsBlockingCalls() ||
        !LeaseTransfersAcrossThreadsAndRemainsExclusive() ||
        !DedicatedOwnerCanWaitForTaskGraphCompletion()) {
        return EXIT_FAILURE;
    }
    std::cout << "RHI thread ownership contract passed\n";
    return EXIT_SUCCESS;
}
