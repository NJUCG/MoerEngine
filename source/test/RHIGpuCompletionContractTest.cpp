#include "rhi/RHICommand.h"
#include "rhi/RHICompletion.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIThreadOwnership.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace Moer::Render;
using namespace std::chrono_literals;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

template<typename TException, typename TCallback>
void ExpectThrows(TCallback&& _callback, std::string_view _message) {
    bool threw_expected = false;
    try {
        _callback();
    } catch (const TException&) {
        threw_expected = true;
    }
    Expect(threw_expected, _message);
}

class CompletionFenceProbe final : public Fence {
public:
    uint64_t GetValue() const override {
        return 0;
    }

    void Wait(uint64_t) override {}

    void MarkSubmitted(uint64_t _value) override {
        submitted_value.store(_value, std::memory_order_release);
    }

    bool WaitSubmitted(
        uint64_t,
        const std::atomic_bool* = nullptr,
        EQueueType              = EQueueType::Ignore,
        Moer::uint32            = 0
    ) override {
        return false;
    }

    bool IsRejected(uint64_t _value) const override {
        return rejected_value.load(std::memory_order_acquire) == _value;
    }

    void Reject(uint64_t _value) noexcept override {
        rejected_value.store(_value, std::memory_order_release);
    }

private:
    std::atomic<uint64_t> submitted_value{0};
    std::atomic<uint64_t> rejected_value{0};
};

void InvalidFutureIsBoundedAndExplicit() {
    GpuCompletionFuture invalid{};
    Expect(!invalid.Valid(), "default completion future was valid");
    Expect(!invalid.IsReady(), "invalid completion future reported Ready");
    Expect(
        invalid.Status() == GpuCompletionStatus::Error,
        "invalid completion future did not fail closed"
    );
    invalid.Wait();
    Expect(
        !invalid.WaitFor(1ms),
        "invalid completion future unexpectedly satisfied WaitFor"
    );
    const auto try_result = invalid.TryGet();
    Expect(
        try_result.has_value() &&
            try_result->status == GpuCompletionStatus::Error,
        "invalid completion TryGet did not return an Error"
    );
    Expect(
        invalid.Get().status == GpuCompletionStatus::Error,
        "invalid completion Get did not return an Error"
    );
}

void FutureIsThreadSafeOneShotAndContainsCallbackExceptions() {
    CommandList list(EQueueType::Graphics);
    const GpuCompletionFuture future =
        list.TrackGpuCompletion("ConcurrentCompletion");
    CmdSubmit submit = list.Submit();
    Expect(
        submit.gpu_completion_tokens.size() == 1,
        "Submit lost its completion token"
    );
    const GpuCompletionToken token =
        submit.gpu_completion_tokens.front();

    Expect(future.Valid(), "recorded completion returned an invalid future");
    Expect(!future.IsReady(), "new completion future was already terminal");
    Expect(
        future.Status() == GpuCompletionStatus::Pending,
        "new completion future was not Pending"
    );
    Expect(
        !future.WaitFor(1ms),
        "pending completion unexpectedly satisfied WaitFor"
    );
    Expect(
        !future.TryGet().has_value(),
        "pending completion unexpectedly had a result"
    );

    constexpr int callback_thread_count = 32;
    std::atomic<int>  callback_count{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads{};
    threads.reserve(callback_thread_count);
    for (int index = 0; index < callback_thread_count; ++index) {
        threads.emplace_back([&, future] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            future.Then([&](const GpuCompletionResult& _result) {
                Expect(
                    _result.status == GpuCompletionStatus::Ready,
                    "concurrent callback observed a non-Ready result"
                );
                callback_count.fetch_add(1, std::memory_order_relaxed);
            });
        });
    }
    future.Then([](const GpuCompletionResult&) {
        throw std::runtime_error("client callback failure");
    });
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    Expect(
        GpuCompletionBackendAccess::ResolveReady(token),
        "first completion did not win the one-shot transition"
    );
    future.Wait();
    Expect(future.WaitFor(1ms), "terminal future did not satisfy WaitFor");
    Expect(
        callback_count.load(std::memory_order_relaxed) ==
            callback_thread_count,
        "pre-terminal callbacks were not invoked exactly once"
    );

    future.Then([&](const GpuCompletionResult& _result) {
        Expect(
            _result.status == GpuCompletionStatus::Ready,
            "post-terminal callback observed the wrong status"
        );
        callback_count.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("post-terminal callback failure");
    });
    Expect(
        callback_count.load(std::memory_order_relaxed) ==
            callback_thread_count + 1,
        "post-terminal callback was not invoked synchronously once"
    );
    Expect(
        !GpuCompletionBackendAccess::ResolveErrorIfPending(
            token, "late rejection"
        ),
        "a second terminal transition overwrote Ready"
    );

    const GpuCompletionResult result = future.Get();
    Expect(
        result.status == GpuCompletionStatus::Ready,
        "Get lost Ready status"
    );
    Expect(
        result.completion_id == token.Id(),
        "Get lost completion identity"
    );
    Expect(
        result.name == "ConcurrentCompletion",
        "Get lost completion name"
    );
    Expect(
        result.error_reason.empty(),
        "Ready result retained an error reason"
    );
    submit.gpu_completion_tokens.clear();
}

void ReadyAndErrorRaceHasExactlyOneWinner() {
    constexpr int race_iterations = 128;
    for (int iteration = 0; iteration < race_iterations; ++iteration) {
        CommandList list(EQueueType::Graphics);
        const GpuCompletionFuture future =
            list.TrackGpuCompletion("ResolveRace");
        CmdSubmit submit = list.Submit();
        const GpuCompletionToken token =
            submit.gpu_completion_tokens.front();

        std::atomic<int>  callback_count{0};
        std::atomic<bool> start{false};
        bool              ready_won = false;
        bool              error_won = false;
        future.Then([&](const GpuCompletionResult& _result) {
            Expect(
                _result.status == GpuCompletionStatus::Ready ||
                    _result.status == GpuCompletionStatus::Error,
                "resolve race callback observed Pending"
            );
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });

        std::thread ready_thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            ready_won =
                GpuCompletionBackendAccess::ResolveReady(token);
        });
        std::thread error_thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            error_won =
                GpuCompletionBackendAccess::ResolveErrorIfPending(
                    token, "injected completion failure"
                );
        });
        start.store(true, std::memory_order_release);
        ready_thread.join();
        error_thread.join();

        Expect(
            ready_won != error_won,
            "Ready/Error race did not have exactly one winner"
        );
        Expect(future.IsReady(), "race left the future Pending");
        Expect(
            callback_count.load(std::memory_order_relaxed) == 1,
            "race invoked its callback more or less than once"
        );
        Expect(
            future.Get().status ==
                (ready_won ? GpuCompletionStatus::Ready :
                             GpuCompletionStatus::Error),
            "race result disagrees with its winner"
        );
        submit.gpu_completion_tokens.clear();
    }
}

void BatchPublishesEveryPeerBeforeNotification() {
    CommandList list(EQueueType::Graphics);
    const GpuCompletionFuture first =
        list.TrackGpuCompletion("BatchFirst");
    const GpuCompletionFuture second =
        list.TrackGpuCompletion("BatchSecond");
    CmdSubmit submit = list.Submit();

    std::atomic<int>  callback_count{0};
    std::atomic_bool  first_observed_second_ready{false};
    first.Then([&](const GpuCompletionResult& _result) {
        const auto peer = second.TryGet();
        first_observed_second_ready.store(
            _result.status == GpuCompletionStatus::Ready &&
                peer.has_value() &&
                peer->status == GpuCompletionStatus::Ready,
            std::memory_order_release
        );
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });
    second.Then([&](const GpuCompletionResult& _result) {
        if (_result.status == GpuCompletionStatus::Ready) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const GpuCompletionPublishBatch batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    GpuCompletionBackendAccess::PublishReady(
        submit.gpu_completion_tokens, batch
    );
    Expect(
        first.IsReady() && second.IsReady(),
        "batch publication did not make every peer terminal"
    );
    Expect(
        callback_count.load(std::memory_order_relaxed) == 0,
        "batch publication released callbacks before notification"
    );
    GpuCompletionBackendAccess::NotifyTerminals(
        submit.gpu_completion_tokens, batch
    );
    Expect(
        first_observed_second_ready.load(std::memory_order_acquire) &&
            callback_count.load(std::memory_order_relaxed) == 2,
        "batch notification exposed a partially published peer set"
    );
    submit.gpu_completion_tokens.clear();
}

void LosingBatchCannotStealNotificationOwnership() {
    CommandList list(EQueueType::Graphics);
    const GpuCompletionFuture future =
        list.TrackGpuCompletion("WinningTicket");
    CmdSubmit submit = list.Submit();
    const GpuCompletionToken token =
        submit.gpu_completion_tokens.front();
    std::atomic<int> callback_count{0};
    future.Then([&](const GpuCompletionResult&) {
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    const GpuCompletionPublishBatch winner =
        GpuCompletionBackendAccess::BeginPublishBatch();
    const GpuCompletionPublishBatch loser =
        GpuCompletionBackendAccess::BeginPublishBatch();
    Expect(
        GpuCompletionBackendAccess::PublishReady(token, winner),
        "winning batch could not publish Ready"
    );
    Expect(
        !GpuCompletionBackendAccess::PublishErrorIfPending(
            token, "losing batch", loser
        ),
        "losing batch overwrote Ready"
    );
    GpuCompletionBackendAccess::NotifyTerminal(token, loser);
    Expect(
        callback_count.load(std::memory_order_relaxed) == 0,
        "losing batch stole notification ownership"
    );
    GpuCompletionBackendAccess::NotifyTerminal(token, winner);
    Expect(
        callback_count.load(std::memory_order_relaxed) == 1,
        "winning batch did not release its callback"
    );
    submit.gpu_completion_tokens.clear();
}

void PublicationWakesWaitersBeforeDeferredCallbacks() {
    CommandList list(EQueueType::Graphics);
    const GpuCompletionFuture future =
        list.TrackGpuCompletion("DeferredNotification");
    CmdSubmit submit = list.Submit();
    const GpuCompletionToken token =
        submit.gpu_completion_tokens.front();

    std::atomic_bool callback_called{false};
    std::atomic_bool waiter_started{false};
    std::atomic_bool waiter_finished{false};
    std::atomic_bool waiter_satisfied{false};
    future.Then([&](const GpuCompletionResult&) {
        callback_called.store(true, std::memory_order_release);
    });
    std::thread waiter([&] {
        waiter_started.store(true, std::memory_order_release);
        waiter_satisfied.store(
            future.WaitFor(5s), std::memory_order_release
        );
        waiter_finished.store(true, std::memory_order_release);
    });

    const auto start_deadline =
        std::chrono::steady_clock::now() + 500ms;
    while (!waiter_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::yield();
    }
    // Give the waiter an opportunity to enter the condition-variable wait;
    // its own timeout keeps the later join bounded even if wake-up regresses.
    std::this_thread::sleep_for(10ms);
    const GpuCompletionPublishBatch batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    const bool published =
        GpuCompletionBackendAccess::PublishReady(token, batch);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!waiter_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool waiter_woke_before_notification =
        waiter_finished.load(std::memory_order_acquire);
    const bool callback_ran_before_notification =
        callback_called.load(std::memory_order_acquire);
    GpuCompletionBackendAccess::NotifyTerminal(token, batch);
    waiter.join();
    Expect(
        waiter_started.load(std::memory_order_acquire),
        "completion waiter thread did not start"
    );
    Expect(published, "deferred publication could not publish Ready");
    Expect(
        waiter_woke_before_notification &&
            waiter_satisfied.load(std::memory_order_acquire),
        "publication did not wake a waiter"
    );
    Expect(
        !callback_ran_before_notification,
        "publication released callbacks before explicit notification"
    );
    Expect(
        callback_called.load(std::memory_order_acquire),
        "explicit notification did not release the callback"
    );
    submit.gpu_completion_tokens.clear();
}

void OwnerThreadsCannotBlockOnPendingCompletion() {
    CommandList list(EQueueType::Graphics);
    const GpuCompletionFuture future =
        list.TrackGpuCompletion("OwnerThreadWaitGuard");
    CmdSubmit submit = list.Submit();
    const GpuCompletionToken token =
        submit.gpu_completion_tokens.front();

    {
        RHIThreadRoleScope completion_owner(
            ERHIThreadRole::Completion
        );
        ExpectThrows<std::logic_error>(
            [&] { future.Wait(); },
            "Completion owner could block in Wait on Pending"
        );
        ExpectThrows<std::logic_error>(
            [&] { (void)future.WaitFor(1ms); },
            "Completion owner could block in WaitFor on Pending"
        );
        ExpectThrows<std::logic_error>(
            [&] { (void)future.Get(); },
            "Completion owner could block in Get on Pending"
        );
        Expect(
            !future.TryGet().has_value(),
            "owner-thread TryGet changed a Pending completion"
        );
    }

    Expect(
        GpuCompletionBackendAccess::ResolveReady(token),
        "owner-thread guard token could not resolve"
    );
    {
        RHIThreadRoleScope submission_owner(
            ERHIThreadRole::Submission
        );
        future.Wait();
        Expect(
            future.WaitFor(1ms) &&
                future.Get().status ==
                    GpuCompletionStatus::Ready,
            "owner thread could not inspect a terminal completion"
        );
    }
    submit.gpu_completion_tokens.clear();
}

void DestructionAndReplacementAreTerminalAndOrdered() {
    GpuCompletionFuture list_destruction{};
    {
        CommandList list(EQueueType::Graphics);
        list_destruction =
            list.TrackGpuCompletion("ListDestruction");
    }
    Expect(
        list_destruction.Get().status == GpuCompletionStatus::Error,
        "CommandList destruction left a completion Pending"
    );

    GpuCompletionFuture submit_destruction{};
    {
        CommandList list(EQueueType::Graphics);
        submit_destruction =
            list.TrackGpuCompletion("SubmitDestruction");
        [[maybe_unused]] CmdSubmit abandoned = list.Submit();
    }
    Expect(
        submit_destruction.Get().status == GpuCompletionStatus::Error,
        "CmdSubmit destruction left a completion Pending"
    );

    GpuCompletionFuture replaced{};
    CmdSubmit destination =
        CommandList(EQueueType::Graphics).Submit();
    {
        CommandList list(EQueueType::Graphics);
        replaced = list.TrackGpuCompletion("ReplacedSubmit");
        destination = list.Submit();
    }
    destination = CommandList(EQueueType::Graphics).Submit();
    Expect(
        replaced.Get().status == GpuCompletionStatus::Error,
        "CmdSubmit replacement left a completion Pending"
    );
}

void SignalAndQueryPublishBeforeCompletionCallbacks() {
    CompletionFenceProbe fence{};
    std::atomic_bool callback_observed_signal{false};
    std::atomic_bool callback_observed_query{false};
    GpuCompletionFuture completion{};
    QueryFuture query{};
    {
        CommandList list(EQueueType::Graphics);
        list.Signal(&fence, 41);
        QueryToken query_token =
            list.BeginTimestampQuery("CompletionPeerQuery");
        list.EndTimestampQuery(query_token);
        query = query_token.GetFuture();
        completion =
            list.TrackGpuCompletion("DestructionOrdering");
        completion.Then([&](const GpuCompletionResult& _result) {
            callback_observed_signal.store(
                _result.status == GpuCompletionStatus::Error &&
                    fence.IsRejected(41),
                std::memory_order_release
            );
            const auto query_result = query.TryGet();
            callback_observed_query.store(
                query_result.has_value() &&
                    query_result->status == QueryStatus::Error,
                std::memory_order_release
            );
        });
        [[maybe_unused]] CmdSubmit submit = list.Submit();
    }
    Expect(
        completion.Get().status == GpuCompletionStatus::Error &&
            callback_observed_signal.load(std::memory_order_acquire) &&
            callback_observed_query.load(std::memory_order_acquire),
        "completion callback observed a partial terminal transaction"
    );
}

void CompletionOnlySubmitIsExecutableWork() {
    CommandList list(EQueueType::Graphics);
    const GpuCompletionFuture future =
        list.TrackGpuCompletion("EmptyBoundary");
    Expect(
        !list.IsEmpty(),
        "completion-only CommandList was classified empty"
    );
    CmdSubmit submit = list.Submit();
    Expect(
        submit.cmds.empty() &&
            submit.gpu_completion_tokens.size() == 1 &&
            submit.segments.size() == 1,
        "completion-only submit lost its executable boundary"
    );
    Expect(
        submit.segments.front().begin == 0 &&
            submit.segments.front().end == 0,
        "completion-only submit emitted a non-empty command range"
    );
    submit.RejectPendingGpuCompletions("test cleanup");
    Expect(
        future.Get().status == GpuCompletionStatus::Error,
        "completion-only cleanup did not terminalize its future"
    );
}

void CancellationIsDeferredAndGenerationScoped() {
    CommandList sealed_list(EQueueType::Graphics);
    const GpuCompletionCancellationView sealed_view =
        sealed_list.GetGpuCompletionCancellationView();
    const GpuCompletionFuture sealed_future =
        sealed_list.TrackGpuCompletion("SealedGeneration");
    CmdSubmit sealed_submit = sealed_list.Submit();
    Expect(
        !sealed_view.Cancel("late stale-view cancellation"),
        "a stale cancellation view mutated a submitted generation"
    );
    Expect(
        sealed_future.Status() == GpuCompletionStatus::Pending,
        "stale cancellation changed a submitted completion"
    );
    Expect(
        GpuCompletionBackendAccess::ResolveReady(
            sealed_submit.gpu_completion_tokens.front()
        ),
        "submitted generation could not resolve after stale cancellation"
    );
    sealed_submit.gpu_completion_tokens.clear();

    CommandList list(EQueueType::Graphics);
    const GpuCompletionCancellationView first_generation =
        list.GetGpuCompletionCancellationView();
    const GpuCompletionFuture existing =
        list.TrackGpuCompletion("BeforeCancel");
    std::atomic<int> callback_count{0};
    existing.Then([&](const GpuCompletionResult&) {
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    Expect(
        first_generation.Cancel("recording source shutdown"),
        "first cancellation did not win"
    );
    Expect(
        !first_generation.Cancel("duplicate shutdown"),
        "cancellation domain was not one-shot"
    );
    Expect(
        existing.Get().status == GpuCompletionStatus::Error,
        "cancellation did not publish Error immediately"
    );
    Expect(
        callback_count.load(std::memory_order_relaxed) == 0,
        "opaque cancellation released a callback on the producer"
    );

    const GpuCompletionFuture late =
        list.TrackGpuCompletion("AfterCancel");
    Expect(
        late.Status() == GpuCompletionStatus::Error,
        "registration after cancellation did not fail immediately"
    );
    CmdSubmit cancelled_submit = list.Submit();
    Expect(
        callback_count.load(std::memory_order_relaxed) == 0,
        "Submit released a deferred callback before Completion ownership"
    );
    cancelled_submit.RejectPendingGpuCompletions("cancelled cleanup");
    Expect(
        callback_count.load(std::memory_order_relaxed) == 1,
        "CmdSubmit did not release the deferred cancellation callback"
    );

    const GpuCompletionCancellationView second_generation =
        list.GetGpuCompletionCancellationView();
    Expect(
        second_generation.Valid() &&
            !second_generation.IsCancelled() &&
            first_generation.IsCancelled(),
        "Submit did not rotate cancellation generations"
    );
    const GpuCompletionFuture next =
        list.TrackGpuCompletion("NextGeneration");
    Expect(
        next.Status() == GpuCompletionStatus::Pending,
        "old cancellation leaked into the next generation"
    );
    Expect(
        second_generation.Cancel("next shutdown"),
        "next generation could not cancel"
    );
    Expect(
        next.Get().status == GpuCompletionStatus::Error,
        "next cancellation left its future Pending"
    );
}

void CallbacksMayReleaseTheirLastHandles() {
    CommandList list(EQueueType::Graphics);
    GpuCompletionFuture future =
        list.TrackGpuCompletion("SelfRelease");
    CmdSubmit submit = list.Submit();
    std::atomic_bool callback_called{false};
    future.Then([&](const GpuCompletionResult& _result) {
        Expect(
            _result.status == GpuCompletionStatus::Ready,
            "self-release callback observed the wrong status"
        );
        future = {};
        callback_called.store(true, std::memory_order_release);
    });
    Expect(
        GpuCompletionBackendAccess::ResolveReady(
            submit.gpu_completion_tokens.front()
        ),
        "self-release token could not resolve"
    );
    Expect(
        callback_called.load(std::memory_order_acquire) &&
            !future.Valid(),
        "callback could not release its last public handle"
    );
    submit.gpu_completion_tokens.clear();
}

} // namespace

int main() {
    static_assert(std::is_copy_constructible_v<GpuCompletionFuture>);
    static_assert(std::is_copy_constructible_v<GpuCompletionToken>);
    static_assert(
        std::is_nothrow_copy_constructible_v<GpuCompletionToken>
    );
    static_assert(
        !std::is_copy_constructible_v<GpuCompletionCancellationDomain>
    );
    static_assert(
        std::is_copy_constructible_v<GpuCompletionCancellationView>
    );

    try {
        InvalidFutureIsBoundedAndExplicit();
        FutureIsThreadSafeOneShotAndContainsCallbackExceptions();
        ReadyAndErrorRaceHasExactlyOneWinner();
        BatchPublishesEveryPeerBeforeNotification();
        LosingBatchCannotStealNotificationOwnership();
        PublicationWakesWaitersBeforeDeferredCallbacks();
        OwnerThreadsCannotBlockOnPendingCompletion();
        DestructionAndReplacementAreTerminalAndOrdered();
        SignalAndQueryPublishBeforeCompletionCallbacks();
        CompletionOnlySubmitIsExecutableWork();
        CancellationIsDeferredAndGenerationScoped();
        CallbacksMayReleaseTheirLastHandles();
    } catch (const std::exception& exception) {
        std::cerr
            << "RHIGpuCompletionContract: "
            << exception.what() << '\n';
        return 1;
    }

    std::cout << "RHIGpuCompletionContract: all checks passed\n";
    return 0;
}
