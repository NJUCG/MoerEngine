#include "rhi/RHIExecutor.h"
#include "rhi/RHIThreadOwnership.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace Moer::Render;
using namespace std::chrono_literals;

struct ResolutionLog {
    void Push(int _id, ERHIRecordingHandoffResult _result) {
        {
            std::lock_guard lock(mutex);
            entries.emplace_back(_id, _result);
        }
        cv.notify_all();
    }

    bool WaitForSize(size_t _size, std::chrono::milliseconds _timeout = 2s) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, _timeout, [this, _size] {
            return entries.size() >= _size;
        });
    }

    std::vector<std::pair<int, ERHIRecordingHandoffResult>> Snapshot() const {
        std::lock_guard lock(mutex);
        return entries;
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::pair<int, ERHIRecordingHandoffResult>> entries{};
};

bool Expect(bool _condition, const char* _message) {
    if (_condition) {
        return true;
    }
    std::cerr << "RHIExecutorRecordingHandoffContract: " << _message << '\n';
    return false;
}

class RecordingFenceProbe final : public Fence {
public:
    RecordingFenceProbe() = default;

    explicit RecordingFenceProbe(
        std::shared_ptr<std::atomic_bool> _destroyed,
        std::function<void()>             _on_reject = {}
    ) :
        destroyed(std::move(_destroyed)),
        on_reject(std::move(_on_reject)) {}

    ~RecordingFenceProbe() override {
        if (destroyed) {
            destroyed->store(true, std::memory_order_release);
        }
    }

    uint64_t GetValue() const override {
        return 0;
    }

    void Wait(uint64_t) override {}

    void MarkSubmitted(uint64_t _value) override {
        submitted_value.store(_value, std::memory_order_release);
    }

    bool WaitSubmitted(
        uint64_t               _value,
        const std::atomic_bool* = nullptr,
        EQueueType              = EQueueType::Ignore,
        Moer::uint32            = 0
    ) override {
        return submitted_value.load(std::memory_order_acquire) >= _value &&
               !IsRejected(_value);
    }

    void Reject(uint64_t _value) noexcept override {
        rejected_value.store(_value, std::memory_order_release);
        if (on_reject) {
            try {
                on_reject();
            } catch (...) {
            }
        }
    }

    bool IsRejected(uint64_t _value) const override {
        return rejected_value.load(std::memory_order_acquire) == _value;
    }

    [[nodiscard]] uint64_t RejectedValue() const {
        return rejected_value.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic_bool> destroyed{};
    std::function<void()>             on_reject{};
    std::atomic<uint64_t> submitted_value{0};
    std::atomic<uint64_t> rejected_value{0};
};

bool CommandListSignalTracksNativeAcceptanceAndRejection() {
    RecordingFenceProbe accepted{};
    CommandList         accepted_commands(EQueueType::Graphics);
    accepted_commands.Signal(&accepted, 3);
    if (!Expect(
            !accepted_commands.IsEmpty(),
            "a CommandList signal was not treated as submit work"
        )) {
        return false;
    }
    CmdSubmit accepted_submit = accepted_commands.Submit();
    if (!Expect(
            accepted_submit.signal_events.size() == 1 &&
                accepted_submit.signal_events.front().value == 3,
            "CommandList::Submit dropped its native-acceptance signal"
        )) {
        return false;
    }
    accepted.MarkSubmitted(3);
    if (!Expect(
            accepted.WaitSubmitted(3),
            "accepted signal did not publish native-submission state"
        )) {
        return false;
    }

    auto owned_destroyed = std::make_shared<std::atomic_bool>(false);
    FenceRef owned_fence{
        MoerNew(RecordingFenceProbe)(owned_destroyed)
    };
    auto* owned_probe =
        static_cast<RecordingFenceProbe*>(owned_fence.Get());
    CommandList owned_commands(EQueueType::Graphics);
    owned_commands.Signal(owned_fence, 5);
    owned_fence = {};
    if (!Expect(
            !owned_destroyed->load(std::memory_order_acquire),
            "CommandList::Signal(FenceRef) dropped its owner before sealing"
        )) {
        return false;
    }
    CmdSubmit owned_submit = owned_commands.Submit();
    if (!Expect(
            !owned_destroyed->load(std::memory_order_acquire) &&
                owned_submit.callbacks.size() == 1,
            "sealed signal did not retain its FenceRef through completion"
        )) {
        return false;
    }
    owned_probe->MarkSubmitted(5);
    for (auto& callback : owned_submit.callbacks) {
        callback();
    }
    // Native completion consumes signal ownership before releasing the
    // keepalive callback. Mirror that ordering so CmdSubmit's fail-closed
    // destructor cannot revisit an already-retired raw fence pointer.
    owned_submit.signal_events.clear();
    owned_submit.callbacks.clear();
    if (!Expect(
            owned_destroyed->load(std::memory_order_acquire),
            "signal FenceRef survived after its completion callback retired"
        )) {
        return false;
    }

    RecordingFenceProbe rejected{};
    CommandList         rejected_commands(EQueueType::Graphics);
    rejected_commands.Signal(&rejected, 7);
    (void)rejected_commands.DrainOrdinaryCallbacksForRejection();
    bool okay = Expect(
                    rejected.IsRejected(7),
                    "rejected graph source did not terminalize its signal"
                ) &&
                Expect(
                    !rejected.WaitSubmitted(7),
                    "rejected signal was reported as natively submitted"
                ) &&
                Expect(
                    rejected_commands.IsEmpty(),
                    "rejected graph source retained its signal"
                );

    RecordingFenceProbe frontend_rejected{};
    CommandList         sealed_commands(EQueueType::Graphics);
    sealed_commands.Signal(&frontend_rejected, 9);
    RHIExecutor::Get().Submit(
        EQueueType::Ignore,
        sealed_commands.Submit(),
        ERHIExecSubmitFlags::None
    );
    okay &= Expect(
        frontend_rejected.IsRejected(9),
        "frontend rejection did not terminalize a sealed signal"
    );
    okay &= Expect(
        !frontend_rejected.WaitSubmitted(9),
        "frontend-rejected signal was reported as natively submitted"
    );
    return okay;
}

bool FrontendQueryRejectionTerminalizesBeforeOrdinaryCallbacks() {
    CommandList commands(EQueueType::Graphics);
    QueryFuture future{};

    std::atomic<int>  ordinary_callbacks{0};
    std::atomic<int>  success_callbacks{0};
    std::atomic<bool> ordinary_callback_observed_error{false};

    // Register this before BeginTimestampQuery so it also precedes the query's
    // own rejection fallback in CmdSubmit::callbacks. Frontend rejection must
    // terminalize every query before invoking either callback.
    commands.AddCallback([&] {
        const std::optional<QueryResult> result = future.TryGet();
        ordinary_callback_observed_error.store(
            result.has_value() && result->status == QueryStatus::Error &&
                !result->error_reason.empty(),
            std::memory_order_release
        );
        ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    commands.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_release);
    });

    QueryToken token =
        commands.BeginTimestampQuery("FrontendRejectedTimestamp");
    future = token.GetFuture();
    commands.EndTimestampQuery(token);

    RHIExecutor::Get().Submit(
        EQueueType::Ignore,
        commands.Submit(),
        ERHIExecSubmitFlags::None
    );

    const std::optional<QueryResult> result = future.TryGet();
    return Expect(
               ordinary_callbacks.load(std::memory_order_acquire) == 1,
               "frontend rejection did not invoke the ordinary callback exactly once"
           ) &&
           Expect(
               ordinary_callback_observed_error.load(std::memory_order_acquire),
               "ordinary callback ran before the rejected query became Error"
           ) &&
           Expect(
               success_callbacks.load(std::memory_order_acquire) == 0,
               "frontend rejection invoked a success-only callback"
           ) &&
           Expect(
               result.has_value() && result->status == QueryStatus::Error &&
                   !result->error_reason.empty(),
               "frontend rejection left the timestamp query non-terminal"
            );
}

bool FrontendCompletionRejectionIsOneTerminalTransaction() {
    std::atomic<int>  stage{0};
    std::atomic<int>  completion_callbacks{0};
    std::atomic<int>  query_callbacks{0};
    std::atomic<int>  ordinary_callbacks{0};
    std::atomic<int>  success_callbacks{0};
    std::atomic<bool> signal_ordered{false};
    std::atomic<bool> completion_ordered{false};
    std::atomic<bool> query_ordered{false};
    std::atomic<bool> ordinary_ordered{false};

    RecordingFenceProbe signal(
        {},
        [&] {
            signal_ordered.store(
                stage.load(std::memory_order_acquire) == 0,
                std::memory_order_release
            );
            stage.store(1, std::memory_order_release);
        }
    );
    CommandList commands(EQueueType::Graphics);
    commands.Signal(&signal, 15);
    const GpuCompletionFuture completion =
        commands.TrackGpuCompletion("FrontendRejectedCompletion");
    QueryToken query =
        commands.BeginTimestampQuery("FrontendRejectedPeerQuery");
    commands.EndTimestampQuery(query);
    const QueryFuture query_future = query.GetFuture();

    completion.Then([&](const GpuCompletionResult& _result) {
        const auto peer = query_future.TryGet();
        completion_ordered.store(
            _result.status == GpuCompletionStatus::Error &&
                signal.IsRejected(15) &&
                peer.has_value() &&
                peer->status == QueryStatus::Error &&
                stage.load(std::memory_order_acquire) == 1,
            std::memory_order_release
        );
        stage.store(2, std::memory_order_release);
        completion_callbacks.fetch_add(1, std::memory_order_release);
    });
    query_future.Then([&](const QueryResult& _result) {
        query_ordered.store(
            _result.status == QueryStatus::Error &&
                completion.Status() == GpuCompletionStatus::Error &&
                stage.load(std::memory_order_acquire) == 2,
            std::memory_order_release
        );
        stage.store(3, std::memory_order_release);
        query_callbacks.fetch_add(1, std::memory_order_release);
    });
    commands.AddCallback([&] {
        ordinary_ordered.store(
            stage.load(std::memory_order_acquire) == 3,
            std::memory_order_release
        );
        stage.store(4, std::memory_order_release);
        ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    commands.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_release);
    });

    RHIExecutor::Get().Submit(
        EQueueType::Ignore,
        commands.Submit(),
        ERHIExecSubmitFlags::None
    );

    return Expect(
               signal_ordered.load(std::memory_order_acquire),
               "frontend rejection did not reject Signal first"
           ) &&
           Expect(
               completion_ordered.load(std::memory_order_acquire) &&
                   completion_callbacks.load(
                       std::memory_order_acquire
                   ) == 1,
               "Completion callback observed a partial terminal batch"
           ) &&
           Expect(
               query_ordered.load(std::memory_order_acquire) &&
                   query_callbacks.load(std::memory_order_acquire) == 1,
               "Query callback did not follow Completion notification"
           ) &&
           Expect(
               ordinary_ordered.load(std::memory_order_acquire) &&
                   ordinary_callbacks.load(
                       std::memory_order_acquire
                   ) == 1 &&
                   stage.load(std::memory_order_acquire) == 4,
               "ordinary callback ran before Future notifications"
           ) &&
           Expect(
               success_callbacks.load(std::memory_order_acquire) == 0,
               "frontend rejection invoked a success-only callback"
           );
}

bool FrontendPresentRejectionPublishesBatchBeforeReceipt() {
    PresentReceiptRef receipt = std::make_shared<PresentReceipt>();
    std::atomic<bool> signal_observed_pending_receipt{false};
    std::atomic<bool> query_observed_terminal_batch_and_receipt{false};
    std::atomic<bool> ordinary_observed_query_notification{false};
    std::atomic<int>  query_callbacks{0};
    std::atomic<int>  ordinary_callbacks{0};
    std::atomic<int>  success_callbacks{0};

    RecordingFenceProbe signal(
        std::shared_ptr<std::atomic_bool>{},
        [&] {
            const PresentReceiptResult result =
                receipt->WaitForSubmission(0ms);
            signal_observed_pending_receipt.store(
                !result.resolved,
                std::memory_order_release
            );
        }
    );

    CommandList commands(EQueueType::Graphics);
    commands.Signal(&signal, 13);
    commands.AddCallback([&] {
        const PresentReceiptResult present_result =
            receipt->WaitForSubmission(0ms);
        ordinary_observed_query_notification.store(
            present_result.resolved && !present_result.submitted &&
                signal.IsRejected(13) &&
                query_callbacks.load(std::memory_order_acquire) == 1,
            std::memory_order_release
        );
        ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    commands.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_release);
    });

    QueryToken token =
        commands.BeginTimestampQuery("FrontendRejectedPresentTimestamp");
    QueryFuture future = token.GetFuture();
    commands.EndTimestampQuery(token);
    future.Then([&](const QueryResult& _result) {
        const PresentReceiptResult present_result =
            receipt->WaitForSubmission(0ms);
        query_observed_terminal_batch_and_receipt.store(
            _result.status == QueryStatus::Error && signal.IsRejected(13) &&
                present_result.resolved && !present_result.submitted,
            std::memory_order_release
        );
        query_callbacks.fetch_add(1, std::memory_order_release);
    });

    // The invalid Present request is rejected synchronously, without requiring
    // a RenderDevice. Its receipt must remain Pending while sibling Signals
    // are rejected, then resolve after Query state publication and before any
    // Query or ordinary callback notification.
    RHIPresentRequest present{};
    present.receipt = receipt;
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        commands.Submit(),
        ERHIExecSubmitFlags::None,
        &present
    );

    const PresentReceiptResult present_result =
        receipt->WaitForSubmission(100ms);
    const std::optional<QueryResult> query_result = future.TryGet();
    return Expect(
               signal_observed_pending_receipt.load(
                   std::memory_order_acquire
               ),
               "frontend Present receipt resolved before a sibling signal became terminal"
           ) &&
           Expect(
               present_result.resolved && !present_result.submitted &&
                   receipt->ResolutionAttemptCount() == 1,
               "frontend Present rejection did not resolve its receipt exactly once"
           ) &&
           Expect(
               query_result.has_value() &&
                   query_result->status == QueryStatus::Error,
               "frontend Present rejection left a sibling Query Pending"
           ) &&
           Expect(
               query_callbacks.load(std::memory_order_acquire) == 1 &&
                   query_observed_terminal_batch_and_receipt.load(
                       std::memory_order_acquire
                   ),
               "Query callback ran before the rejected batch and Present receipt were terminal"
           ) &&
           Expect(
               ordinary_callbacks.load(std::memory_order_acquire) == 1 &&
                   ordinary_observed_query_notification.load(
                       std::memory_order_acquire
                   ),
               "ordinary callback ran before Query notification or Present rejection"
           ) &&
           Expect(
               success_callbacks.load(std::memory_order_acquire) == 0,
               "frontend Present rejection invoked a success-only callback"
           );
}

bool DeferredOpaqueCancellationTicketSurvivesSubmit() {
    RecordingFenceProbe signal{};
    CommandList         commands(EQueueType::Graphics);
    std::atomic<int>    query_callbacks{0};
    std::atomic<int>    ordinary_callbacks{0};
    std::atomic<int>    success_callbacks{0};
    std::atomic<bool>   query_observed_signal_rejection{false};
    std::atomic<bool>   ordinary_observed_query_notification{false};

    commands.Signal(&signal, 17);
    commands.AddCallback([&] {
        ordinary_observed_query_notification.store(
            signal.IsRejected(17) &&
                query_callbacks.load(std::memory_order_acquire) == 1,
            std::memory_order_release
        );
        ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    commands.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_release);
    });
    QueryToken token =
        commands.BeginTimestampQuery("DeferredCancellationSubmit");
    commands.EndTimestampQuery(token);
    QueryFuture future = token.GetFuture();

    QueryCancellationView cancellation =
        commands.GetQueryCancellationView();
    const QueryPublishBatch cancellation_batch =
        QueryBackendAccess::BeginPublishBatch();
    const bool cancellation_published =
        cancellation.PublishCancellation(
            "opaque source cancelled during shutdown",
            cancellation_batch
        );
    future.Then([&](const QueryResult& _result) {
        query_observed_signal_rejection.store(
            _result.status == QueryStatus::Error &&
                signal.IsRejected(17) &&
                ordinary_callbacks.load(std::memory_order_acquire) == 0,
            std::memory_order_release
        );
        query_callbacks.fetch_add(1, std::memory_order_release);
    });

    const bool callback_was_deferred =
        query_callbacks.load(std::memory_order_acquire) == 0;
    CmdSubmit submit = commands.Submit();
    const bool ticket_transferred =
        submit.query_publish_batch.Valid() &&
        submit.query_tokens.size() == 1;
    const bool callback_still_deferred =
        query_callbacks.load(std::memory_order_acquire) == 0;

    // Invalid-queue rejection is synchronous and exercises the ordinary
    // frontend terminalization path without constructing a native backend.
    RHIExecutor::Get().Submit(
        EQueueType::Ignore,
        std::move(submit),
        ERHIExecSubmitFlags::None
    );
    cancellation.NotifyPublishedCancellation();

    const std::optional<QueryResult> result = future.TryGet();
    return Expect(
               cancellation_published,
               "opaque cancellation did not publish its Query Error"
           ) &&
           Expect(
               callback_was_deferred && callback_still_deferred,
               "opaque cancellation released its callback before signal ownership transferred"
           ) &&
           Expect(
               ticket_transferred,
               "CommandList::Submit dropped the deferred cancellation ticket or tokens"
           ) &&
           Expect(
               query_callbacks.load(std::memory_order_acquire) == 1,
               "deferred Query callback was not released exactly once"
           ) &&
           Expect(
               query_observed_signal_rejection.load(std::memory_order_acquire),
               "deferred Query callback ran before its submitted signal was rejected"
           ) &&
           Expect(
               ordinary_callbacks.load(std::memory_order_acquire) == 1 &&
                   ordinary_observed_query_notification.load(
                       std::memory_order_acquire
                   ),
               "ordinary callback did not run exactly once after Query notification"
           ) &&
           Expect(
               success_callbacks.load(std::memory_order_acquire) == 0,
               "rejection invoked a success-only callback"
           ) &&
           Expect(
               result.has_value() && result->status == QueryStatus::Error,
               "deferred cancellation result did not remain Error"
           );
}

bool DirectCommandListGroupPreflightRejectsEverySource() {
    std::array<RecordingFenceProbe, 3> signals{};
    Moer::Array<CommandList>           command_lists{};
    command_lists.reserve(3);
    for (size_t index = 0; index < 3; ++index) {
        command_lists.emplace_back(EQueueType::Graphics);
    }

    std::array<QueryFuture, 3> futures{};
    std::array<std::atomic<int>, 3> query_callbacks{};
    std::array<std::atomic<int>, 3> ordinary_callbacks{};
    std::array<std::atomic<int>, 3> success_callbacks{};
    std::array<std::atomic<bool>, 3> query_observed_error{};
    std::array<std::atomic<bool>, 3> query_observed_present_rejection{};
    std::array<std::atomic<bool>, 3> ordinary_observed_own_query{};
    std::atomic<bool> first_query_observed_last_terminal{false};
    PresentReceiptRef receipt = std::make_shared<PresentReceipt>();
    RHIPresentRequest present{};
    present.receipt = receipt;

    for (size_t index = 0; index < command_lists.size(); ++index) {
        command_lists[index].Signal(&signals[index], 40 + index);
        command_lists[index].AddCallback([&, index] {
            ordinary_observed_own_query[index].store(
                query_callbacks[index].load(std::memory_order_acquire) == 1,
                std::memory_order_release
            );
            ordinary_callbacks[index].fetch_add(
                1, std::memory_order_release
            );
        });
        command_lists[index].AddSuccessCallback([&, index] {
            success_callbacks[index].fetch_add(
                1, std::memory_order_release
            );
        });
        QueryToken token = command_lists[index].BeginTimestampQuery(
            "DirectSubmitPreflightTimestamp"
        );
        futures[index] = token.GetFuture();
        if (index != 1) {
            command_lists[index].EndTimestampQuery(token);
        }
        futures[index].Then([&, index](const QueryResult& _result) {
            if (index == 0) {
                first_query_observed_last_terminal.store(
                    futures[2].WaitFor(100ms) &&
                        futures[2].Status() == QueryStatus::Error,
                    std::memory_order_release
                );
            }
            query_observed_error[index].store(
                _result.status == QueryStatus::Error,
                std::memory_order_release
            );
            const PresentReceiptResult present_result =
                receipt->WaitForSubmission(0ms);
            query_observed_present_rejection[index].store(
                present_result.resolved && !present_result.submitted,
                std::memory_order_release
            );
            query_callbacks[index].fetch_add(
                1, std::memory_order_release
            );
        });
    }

    bool threw = false;
    try {
        RHIExecutor::Get().Submit(
            std::move(command_lists),
            ERHIExecSubmitFlags::None,
            &present
        );
    } catch (...) {
        threw = true;
    }

    bool okay = Expect(
        !threw,
        "direct CommandList group preflight propagated an open-query failure"
    );
    for (size_t index = 0; index < command_lists.size(); ++index) {
        okay &= Expect(
            signals[index].IsRejected(40 + index),
            "direct CommandList group preflight left a signal Pending"
        );
        okay &= Expect(
            futures[index].WaitFor(100ms) &&
                futures[index].Status() == QueryStatus::Error,
            "direct CommandList group preflight left a QueryFuture Pending"
        );
        okay &= Expect(
            query_callbacks[index].load(std::memory_order_acquire) == 1 &&
                query_observed_error[index].load(
                    std::memory_order_acquire
                ),
            "direct CommandList group preflight did not notify a Query callback exactly once"
        );
        okay &= Expect(
            query_observed_present_rejection[index].load(
                std::memory_order_acquire
            ),
            "direct CommandList group Query callback ran before Present rejection"
        );
        okay &= Expect(
            ordinary_callbacks[index].load(std::memory_order_acquire) == 1 &&
                ordinary_observed_own_query[index].load(
                    std::memory_order_acquire
                ),
            "direct CommandList group preflight did not run ordinary cleanup after its Query callback"
        );
        okay &= Expect(
            success_callbacks[index].load(std::memory_order_acquire) == 0,
            "direct CommandList group preflight invoked a success-only callback"
        );
        okay &= Expect(
            command_lists[index].IsEmpty(),
            "direct CommandList group preflight left a source undrained"
        );
    }
    okay &= Expect(
        first_query_observed_last_terminal.load(std::memory_order_acquire),
        "direct CommandList group preflight allowed an earlier Query callback to observe a later Future Pending"
    );
    const PresentReceiptResult present_result =
        receipt->WaitForSubmission(100ms);
    okay &= Expect(
        present_result.resolved && !present_result.submitted &&
            receipt->ResolutionAttemptCount() == 1,
        "direct CommandList group preflight did not reject Present exactly once"
    );

    // Keep the regression bounded and release raw fence ownership even when
    // running against an older implementation that propagates the exception.
    for (CommandList& command_list : command_lists) {
        if (!command_list.IsEmpty()) {
            auto callbacks =
                command_list.DrainOrdinaryCallbacksForRejection();
            for (auto& callback : callbacks) {
                if (callback) {
                    callback();
                }
            }
        }
    }
    return okay;
}

bool DirectCommandListGroupMaterializationFailureRejectsPrefixAndSuffix() {
    std::optional<CommandList::ManagedRecordingLease> blocking_lease{};
    RecordingFenceProbe prefix_signal(
        {},
        [&blocking_lease] {
            blocking_lease.reset();
        }
    );
    RecordingFenceProbe middle_signal{};
    RecordingFenceProbe suffix_signal{};
    const std::array<RecordingFenceProbe*, 3> signals{
        &prefix_signal,
        &middle_signal,
        &suffix_signal,
    };

    Moer::Array<CommandList> command_lists{};
    command_lists.reserve(3);
    for (size_t index = 0; index < 3; ++index) {
        command_lists.emplace_back(EQueueType::Graphics);
    }

    std::array<QueryFuture, 3> futures{};
    std::array<std::atomic<int>, 3> query_callbacks{};
    std::array<std::atomic<int>, 3> ordinary_callbacks{};
    std::array<std::atomic<int>, 3> success_callbacks{};
    std::array<std::atomic<bool>, 3> ordinary_observed_own_query{};
    std::atomic<bool> prefix_observed_suffix_terminal{false};
    std::atomic<int>  reentrant_callbacks{0};

    for (size_t index = 0; index < command_lists.size(); ++index) {
        command_lists[index].Signal(signals[index], 50 + index);
        command_lists[index].AddCallback([&, index] {
            ordinary_observed_own_query[index].store(
                query_callbacks[index].load(std::memory_order_acquire) == 1,
                std::memory_order_release
            );
            ordinary_callbacks[index].fetch_add(
                1, std::memory_order_release
            );
        });
        command_lists[index].AddSuccessCallback([&, index] {
            success_callbacks[index].fetch_add(
                1, std::memory_order_release
            );
        });
        QueryToken token = command_lists[index].BeginTimestampQuery(
            "DirectSubmitMaterializationTimestamp"
        );
        command_lists[index].EndTimestampQuery(token);
        futures[index] = token.GetFuture();
        futures[index].Then([&, index](const QueryResult& _result) {
            if (index == 0) {
                prefix_observed_suffix_terminal.store(
                    futures[2].WaitFor(100ms) &&
                        futures[2].Status() == QueryStatus::Error,
                    std::memory_order_release
                );
                // Source zero has already sealed. Re-entrant work belongs to
                // its fresh generation and must survive rejection of the old
                // materialized prefix.
                command_lists[0].AddCallback([&reentrant_callbacks] {
                    reentrant_callbacks.fetch_add(
                        1, std::memory_order_release
                    );
                });
            }
            if (_result.status == QueryStatus::Error) {
                query_callbacks[index].fetch_add(
                    1, std::memory_order_release
                );
            }
        });
    }

    blocking_lease.emplace(
        command_lists[1].AcquireManagedRecordingLease()
    );
    bool threw = false;
    try {
        RHIExecutor::Get().Submit(
            std::move(command_lists),
            ERHIExecSubmitFlags::None,
            nullptr
        );
    } catch (...) {
        threw = true;
    }

    bool okay = Expect(
                    !threw,
                    "direct CommandList group propagated a mid-materialization failure"
                ) &&
                Expect(
                    !blocking_lease.has_value(),
                    "materialization-failure trigger did not release its managed lease"
                );
    for (size_t index = 0; index < command_lists.size(); ++index) {
        okay &= Expect(
            signals[index]->IsRejected(50 + index),
            "direct CommandList materialization failure left a signal Pending"
        );
        okay &= Expect(
            futures[index].WaitFor(100ms) &&
                futures[index].Status() == QueryStatus::Error,
            "direct CommandList materialization failure left a QueryFuture Pending"
        );
        okay &= Expect(
            query_callbacks[index].load(std::memory_order_acquire) == 1,
            "direct CommandList materialization failure did not notify a Query callback exactly once"
        );
        okay &= Expect(
            ordinary_callbacks[index].load(std::memory_order_acquire) == 1 &&
                ordinary_observed_own_query[index].load(
                    std::memory_order_acquire
                ),
            "direct CommandList materialization failure did not run ordinary cleanup exactly once after Query notification"
        );
        okay &= Expect(
            success_callbacks[index].load(std::memory_order_acquire) == 0,
            "direct CommandList materialization failure invoked a success-only callback"
        );
    }
    okay &= Expect(
        prefix_observed_suffix_terminal.load(std::memory_order_acquire),
        "materialized-prefix Query callback observed a suffix Future Pending"
    );
    okay &= Expect(
        reentrant_callbacks.load(std::memory_order_acquire) == 0,
        "group rejection consumed work re-entered into the prefix's fresh generation"
    );
    okay &= Expect(
        command_lists[1].IsEmpty() && command_lists[2].IsEmpty(),
        "direct CommandList materialization failure left an unmaterialized suffix undrained"
    );

    CmdSubmit reentrant_submit = command_lists[0].Submit();
    okay &= Expect(
        reentrant_submit.callbacks.size() == 1,
        "materialized-prefix fresh generation did not retain re-entrant work"
    );
    for (auto& callback : reentrant_submit.callbacks) {
        if (callback) {
            callback();
        }
    }
    reentrant_submit.callbacks.clear();
    okay &= Expect(
        reentrant_callbacks.load(std::memory_order_acquire) == 1,
        "preserved re-entrant callback did not execute exactly once"
    );
    return okay;
}

bool RecordingPreflightFailureTerminalizesEveryQueryGeneration() {
    RHIExecutor::StartUp();

    auto first_list  = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    auto middle_list = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    auto last_list   = Moer::MakeShared<CommandList>(EQueueType::Graphics);

    QueryToken first_query =
        first_list->BeginTimestampQuery("PreflightFirstTimestamp");
    QueryFuture first_future = first_query.GetFuture();
    first_list->EndTimestampQuery(first_query);

    QueryToken middle_query =
        middle_list->BeginTimestampQuery("PreflightUnclosedTimestamp");
    QueryFuture middle_future = middle_query.GetFuture();

    QueryToken last_query =
        last_list->BeginTimestampQuery("PreflightLastTimestamp");
    QueryFuture last_future = last_query.GetFuture();
    last_list->EndTimestampQuery(last_query);

    std::array<std::atomic<int>, 3> query_callbacks{};
    std::array<std::atomic<int>, 3> ordinary_callbacks{};
    std::array<std::atomic<int>, 3> success_callbacks{};
    std::array<std::atomic<bool>, 3> query_callbacks_observed_error{};
    std::atomic<bool> first_query_observed_last_terminal{false};

    first_future.Then([&](const QueryResult& _result) {
        first_query_observed_last_terminal.store(
            last_future.WaitFor(100ms) &&
                last_future.Status() == QueryStatus::Error,
            std::memory_order_release
        );
        query_callbacks_observed_error[0].store(
            _result.status == QueryStatus::Error,
            std::memory_order_release
        );
        query_callbacks[0].fetch_add(1, std::memory_order_release);
    });
    middle_future.Then([&](const QueryResult& _result) {
        query_callbacks_observed_error[1].store(
            _result.status == QueryStatus::Error,
            std::memory_order_release
        );
        query_callbacks[1].fetch_add(1, std::memory_order_release);
    });
    last_future.Then([&](const QueryResult& _result) {
        query_callbacks_observed_error[2].store(
            _result.status == QueryStatus::Error,
            std::memory_order_release
        );
        query_callbacks[2].fetch_add(1, std::memory_order_release);
    });

    const std::array<Moer::SharedPtr<CommandList>, 3> command_lists{
        first_list,
        middle_list,
        last_list,
    };
    for (size_t index = 0; index < command_lists.size(); ++index) {
        command_lists[index]->AddCallback([&, index] {
            ordinary_callbacks[index].fetch_add(1, std::memory_order_release);
        });
        command_lists[index]->AddSuccessCallback([&, index] {
            success_callbacks[index].fetch_add(1, std::memory_order_release);
        });
    }

    Moer::Array<RHIRecordingSource> sources{};
    for (const Moer::SharedPtr<CommandList>& command_list : command_lists) {
        sources.emplace_back(RHIRecordingSource{
            .command_list = command_list,
            .completion   = RHIRecordingGate::Create(true),
        });
    }
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const std::array<QueryFuture, 3> futures{
        first_future,
        middle_future,
        last_future,
    };
    bool okay = true;
    for (size_t index = 0; index < futures.size(); ++index) {
        okay &= Expect(
            futures[index].WaitFor(100ms) &&
                futures[index].Status() == QueryStatus::Error,
            "recording preflight failure left a QueryFuture non-terminal"
        );
        okay &= Expect(
            query_callbacks[index].load(std::memory_order_acquire) == 1 &&
                query_callbacks_observed_error[index].load(
                    std::memory_order_acquire
                ),
            "recording preflight failure did not invoke a query callback exactly once with Error"
        );
        okay &= Expect(
            ordinary_callbacks[index].load(std::memory_order_acquire) == 1,
            "recording preflight failure did not drain an ordinary callback exactly once"
        );
        okay &= Expect(
            success_callbacks[index].load(std::memory_order_acquire) == 0,
            "recording preflight failure invoked a success-only callback"
        );
        okay &= Expect(
            command_lists[index]->IsEmpty(),
            "recording preflight failure left a CommandList generation undrained"
        );
    }
    okay &= Expect(
        first_query_observed_last_terminal.load(std::memory_order_acquire),
        "an earlier query callback observed a later source query still Pending"
    );

    RHIExecutor::ShutDown();
    return okay;
}

bool RecordingPreflightFailureTerminalizesEveryCompletionGeneration() {
    RHIExecutor::StartUp();

    auto first_list =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    auto middle_list =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    auto last_list =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    const std::array<Moer::SharedPtr<CommandList>, 3> command_lists{
        first_list,
        middle_list,
        last_list,
    };

    std::array<GpuCompletionFuture, 3> completion_futures{};
    std::array<std::atomic<int>, 3> completion_callbacks{};
    std::array<std::atomic<int>, 3> ordinary_callbacks{};
    std::array<std::atomic<int>, 3> success_callbacks{};
    for (size_t index = 0; index < command_lists.size(); ++index) {
        completion_futures[index] =
            command_lists[index]->TrackGpuCompletion(
                "RecordingPreflightCompletion"
            );
        completion_futures[index].Then(
            [&, index](const GpuCompletionResult& _result) {
                if (_result.status == GpuCompletionStatus::Error) {
                    completion_callbacks[index].fetch_add(
                        1, std::memory_order_release
                    );
                }
            }
        );
        command_lists[index]->AddCallback([&, index] {
            ordinary_callbacks[index].fetch_add(
                1, std::memory_order_release
            );
        });
        command_lists[index]->AddSuccessCallback([&, index] {
            success_callbacks[index].fetch_add(
                1, std::memory_order_release
            );
        });
    }

    std::atomic<bool> first_observed_last_terminal{false};
    completion_futures[0].Then(
        [&](const GpuCompletionResult& _result) {
            first_observed_last_terminal.store(
                _result.status == GpuCompletionStatus::Error &&
                    completion_futures[2].WaitFor(100ms) &&
                    completion_futures[2].Status() ==
                        GpuCompletionStatus::Error,
                std::memory_order_release
            );
        }
    );

    QueryToken unclosed =
        middle_list->BeginTimestampQuery(
            "CompletionPreflightTrigger"
        );
    const QueryFuture trigger_future = unclosed.GetFuture();

    Moer::Array<RHIRecordingSource> sources{};
    for (const Moer::SharedPtr<CommandList>& command_list :
         command_lists) {
        sources.emplace_back(RHIRecordingSource{
            .command_list = command_list,
            .completion   = RHIRecordingGate::Create(true),
        });
    }
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    bool okay = true;
    for (size_t index = 0;
         index < completion_futures.size();
         ++index) {
        okay &= Expect(
            completion_futures[index].WaitFor(100ms) &&
                completion_futures[index].Status() ==
                    GpuCompletionStatus::Error,
            "recording preflight failure left a completion Pending"
        );
        okay &= Expect(
            completion_callbacks[index].load(
                std::memory_order_acquire
            ) == 1,
            "recording preflight failure did not notify a completion once"
        );
        okay &= Expect(
            ordinary_callbacks[index].load(
                std::memory_order_acquire
            ) == 1,
            "recording preflight failure did not drain ordinary cleanup"
        );
        okay &= Expect(
            success_callbacks[index].load(
                std::memory_order_acquire
            ) == 0,
            "recording preflight failure invoked success cleanup"
        );
        okay &= Expect(
            command_lists[index]->IsEmpty(),
            "recording preflight failure left a completion generation"
        );
    }
    okay &= Expect(
        first_observed_last_terminal.load(std::memory_order_acquire),
        "an earlier completion callback observed a later source Pending"
    );
    okay &= Expect(
        trigger_future.WaitFor(100ms) &&
            trigger_future.Status() == QueryStatus::Error,
        "completion preflight trigger query did not terminalize"
    );

    RHIExecutor::ShutDown();
    return okay;
}

bool SubmitRecordingRefreshesAStaleQueryCancellationView() {
    RHIExecutor::StartUp();

    auto command_list =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    QueryCancellationView stale_cancellation =
        command_list->GetQueryCancellationView();
    {
        // Even an empty Submit rotates the CommandList cancellation domain.
        // The caller intentionally publishes the still-valid prior view below.
        CmdSubmit prior_generation = command_list->Submit();
    }

    QueryToken current_query =
        command_list->BeginTimestampQuery("CurrentGenerationTimestamp");
    QueryFuture current_future = current_query.GetFuture();
    command_list->EndTimestampQuery(current_query);

    std::atomic<int> current_query_callbacks{0};
    std::atomic<bool> current_callback_observed_error{false};
    current_future.Then([&](const QueryResult& _result) {
        current_callback_observed_error.store(
            _result.status == QueryStatus::Error,
            std::memory_order_release
        );
        current_query_callbacks.fetch_add(1, std::memory_order_release);
    });

    auto pending_gate = RHIRecordingGate::Create();
    Moer::Array<RHIRecordingSource> sources{};
    sources.emplace_back(RHIRecordingSource{
        .command_list       = command_list,
        .completion         = pending_gate,
        .query_cancellation = stale_cancellation,
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );

    // The pending producer remains opaque during shutdown. Only the refreshed
    // cancellation view can publish Error for its current query generation.
    RHIExecutor::ShutDown();

    bool okay = Expect(
                    stale_cancellation.Valid() &&
                        !stale_cancellation.IsCancelled(),
                    "SubmitRecording cancelled the caller's stale generation"
                ) &&
                Expect(
                    current_future.WaitFor(100ms) &&
                        current_future.Status() == QueryStatus::Error,
                    "shutdown left the current query generation Pending when a stale view was supplied"
                ) &&
                Expect(
                    current_query_callbacks.load(std::memory_order_acquire) == 0,
                    "opaque shutdown released a current-generation query callback too early"
                );

    pending_gate->Signal();
    (void)command_list->DrainOrdinaryCallbacksForRejection();
    okay &= Expect(
        current_query_callbacks.load(std::memory_order_acquire) == 1 &&
            current_callback_observed_error.load(std::memory_order_acquire),
        "current-generation query callback was not released exactly once after producer ownership became safe"
    );
    okay &= Expect(
        command_list->IsEmpty(),
        "stale cancellation-view shutdown left the current CommandList generation undrained"
    );
    return okay;
}

bool SubmitRecordingRefreshesAStaleCompletionCancellationView() {
    RHIExecutor::StartUp();

    auto command_list =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    GpuCompletionCancellationView stale_cancellation =
        command_list->GetGpuCompletionCancellationView();
    {
        CmdSubmit prior_generation = command_list->Submit();
    }

    const GpuCompletionFuture current_future =
        command_list->TrackGpuCompletion(
            "CurrentCompletionGeneration"
        );
    std::atomic<int> current_callbacks{0};
    std::atomic<bool> callback_observed_error{false};
    current_future.Then([&](const GpuCompletionResult& _result) {
        callback_observed_error.store(
            _result.status == GpuCompletionStatus::Error,
            std::memory_order_release
        );
        current_callbacks.fetch_add(1, std::memory_order_release);
    });

    auto pending_gate = RHIRecordingGate::Create();
    Moer::Array<RHIRecordingSource> sources{};
    sources.emplace_back(RHIRecordingSource{
        .command_list = command_list,
        .completion   = pending_gate,
        .gpu_completion_cancellation = stale_cancellation,
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::ShutDown();

    bool okay = Expect(
                    stale_cancellation.Valid() &&
                        !stale_cancellation.IsCancelled(),
                    "SubmitRecording cancelled a stale completion generation"
                ) &&
                Expect(
                    current_future.WaitFor(100ms) &&
                        current_future.Status() ==
                            GpuCompletionStatus::Error,
                    "shutdown left the current completion generation Pending"
                ) &&
                Expect(
                    current_callbacks.load(
                        std::memory_order_acquire
                    ) == 0,
                    "opaque shutdown released a completion callback early"
                );

    pending_gate->Signal();
    (void)command_list->DrainOrdinaryCallbacksForRejection();
    okay &= Expect(
        current_callbacks.load(std::memory_order_acquire) == 1 &&
            callback_observed_error.load(std::memory_order_acquire),
        "safe producer ownership did not release completion callback once"
    );
    okay &= Expect(
        command_list->IsEmpty(),
        "stale completion-view shutdown left its generation undrained"
    );
    return okay;
}

bool PerSourceGatesPreserveFifo() {
    RHIExecutorRecordingHandoffQueue queue{};
    queue.Start();

    ResolutionLog log{};
    auto first_gate  = RHIRecordingGate::Create();
    auto second_gate = RHIRecordingGate::Create();

    queue.EnqueueRecording(RHIExecutorRecordingHandoffWork{
        .prerequisites = {first_gate, second_gate},
        .resolve = [&log](ERHIRecordingHandoffResult _result) {
            log.Push(1, _result);
        },
    });
    queue.RouteReady(RHIExecutorRecordingHandoffWork{
        .prerequisites = {},
        .resolve = [&log](ERHIRecordingHandoffResult _result) {
            log.Push(2, _result);
        },
    });

    second_gate->Signal();
    std::this_thread::sleep_for(30ms);
    if (!Expect(log.Snapshot().empty(), "a later ready gate overtook the FIFO head")) {
        queue.ShutDown();
        return false;
    }

    first_gate->Signal();
    const bool completed = log.WaitForSize(2);
    const auto entries   = log.Snapshot();
    queue.ShutDown();

    return Expect(completed, "completed gates did not release the handoff") &&
           Expect(entries.size() == 2, "unexpected FIFO result count") &&
           Expect(entries[0].first == 1 && entries[1].first == 2, "FIFO order changed") &&
           Expect(
               entries[0].second == ERHIRecordingHandoffResult::Consume &&
                   entries[1].second == ERHIRecordingHandoffResult::Consume,
               "completed work was not consumed"
           );
}

bool ReadyWorkUsesStableExecutorOwnerAndCannotBeOvertaken() {
    RHIExecutorRecordingHandoffQueue queue{};
    queue.Start();

    ResolutionLog log{};
    std::mutex block_mutex;
    std::condition_variable block_cv;
    bool first_entered = false;
    bool release_first = false;
    const std::thread::id caller_thread = std::this_thread::get_id();
    std::thread::id first_owner{};
    std::thread::id second_owner{};
    ERHIThreadRole first_role{ERHIThreadRole::Unknown};
    ERHIThreadRole second_role{ERHIThreadRole::Unknown};

    queue.RouteReady(RHIExecutorRecordingHandoffWork{
        .prerequisites = {},
        .resolve = [&](ERHIRecordingHandoffResult _result) {
            {
                std::unique_lock lock(block_mutex);
                first_owner = std::this_thread::get_id();
                first_role  = GetCurrentRHIThreadRole();
                first_entered = true;
                block_cv.notify_all();
                block_cv.wait(lock, [&] { return release_first; });
            }
            log.Push(1, _result);
        },
    });

    {
        std::unique_lock lock(block_mutex);
        if (!block_cv.wait_for(lock, 2s, [&] { return first_entered; })) {
            release_first = true;
            block_cv.notify_all();
            queue.ShutDown();
            return Expect(false, "ready work did not start on the Executor worker");
        }
    }

    auto completed_gate = RHIRecordingGate::Create(true);
    queue.EnqueueRecording(RHIExecutorRecordingHandoffWork{
        .prerequisites = {completed_gate},
        .resolve = [&log, &second_owner, &second_role](ERHIRecordingHandoffResult _result) {
            second_owner = std::this_thread::get_id();
            second_role  = GetCurrentRHIThreadRole();
            log.Push(2, _result);
        },
    });
    std::this_thread::sleep_for(30ms);
    if (!Expect(log.Snapshot().empty(), "recording work overtook active ready work")) {
        {
            std::lock_guard lock(block_mutex);
            release_first = true;
        }
        block_cv.notify_all();
        queue.ShutDown();
        return false;
    }

    {
        std::lock_guard lock(block_mutex);
        release_first = true;
    }
    block_cv.notify_all();

    const bool completed = log.WaitForSize(2);
    const auto entries   = log.Snapshot();
    queue.ShutDown();
    return Expect(completed, "queued recording work did not run") &&
           Expect(entries.size() == 2, "unexpected ready-work result count") &&
           Expect(entries[0].first == 1 && entries[1].first == 2, "ready-work order changed") &&
           Expect(
               first_owner != caller_thread && second_owner != caller_thread,
               "accepted ready work ran on the caller thread"
           ) &&
           Expect(first_owner == second_owner, "ready work changed Executor owner thread") &&
           Expect(
               first_role == ERHIThreadRole::Executor &&
                   second_role == ERHIThreadRole::Executor,
               "ready work did not run under the Executor role"
           );
}

bool FailedGateRejectsOnlyItsGroupAndPreservesFifo() {
    RHIExecutorRecordingHandoffQueue queue{};
    queue.Start();

    ResolutionLog log{};
    auto failed_gate = RHIRecordingGate::Create();
    auto later_gate  = RHIRecordingGate::Create();
    queue.EnqueueRecording(RHIExecutorRecordingHandoffWork{
        .prerequisites = {failed_gate, later_gate},
        .resolve = [&log](ERHIRecordingHandoffResult _result) {
            log.Push(1, _result);
        },
    });
    queue.RouteReady(RHIExecutorRecordingHandoffWork{
        .prerequisites = {},
        .resolve = [&log](ERHIRecordingHandoffResult _result) {
            log.Push(2, _result);
        },
    });

    failed_gate->Fail();
    std::this_thread::sleep_for(30ms);
    if (!Expect(
            log.Snapshot().empty(),
            "a failed group resolved before every producer gate became terminal"
        )) {
        later_gate->Signal();
        queue.ShutDown();
        return false;
    }

    later_gate->Signal();
    const bool completed = log.WaitForSize(2);
    const auto entries   = log.Snapshot();

    bool okay = Expect(completed, "failed gate did not resolve the FIFO") &&
                Expect(entries.size() == 2, "unexpected failed-gate result count") &&
                Expect(entries[0].first == 1 && entries[1].first == 2, "failed group reordered FIFO") &&
                Expect(
                    entries[0].second == ERHIRecordingHandoffResult::Reject,
                    "failed recording group was consumed"
                ) &&
                Expect(
                    entries[1].second == ERHIRecordingHandoffResult::Consume,
                    "ready work after failed group was rejected"
                ) &&
                Expect(
                    failed_gate->Status() == ERHIRecordingStatus::Failed,
                    "failed gate lost its terminal status"
                ) &&
                Expect(!failed_gate->Signal(), "failed gate changed terminal status");
    queue.ShutDown();
    return okay;
}

bool FailedRecordingDrainsCleanupExactlyOnce() {
    RHIExecutor::StartUp();

    std::atomic<int> ordinary_callbacks{0};
    std::atomic<int> success_callbacks{0};
    auto first_list  = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    auto second_list = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    first_list->AddCallback([&ordinary_callbacks] { ordinary_callbacks.fetch_add(1); });
    second_list->AddCallback([&ordinary_callbacks] { ordinary_callbacks.fetch_add(1); });
    first_list->AddSuccessCallback([&success_callbacks] { success_callbacks.fetch_add(1); });
    second_list->AddSuccessCallback([&success_callbacks] { success_callbacks.fetch_add(1); });
    // A producer exception may bypass a manual PopScope. The rejection path
    // must drain cleanup without calling Submit(), whose Debug contract
    // intentionally rejects an unmatched scope.
    first_list->PushScope("Unclosed failed producer scope");

    auto failed_gate = RHIRecordingGate::Create();
    auto later_gate  = RHIRecordingGate::Create();
    Moer::Array<RHIRecordingSource> sources{};
    sources.emplace_back(RHIRecordingSource{
        .command_list = first_list,
        .completion   = failed_gate,
    });
    sources.emplace_back(RHIRecordingSource{
        .command_list = second_list,
        .completion   = later_gate,
    });
    RHIExecutor::Get().SubmitRecording(std::move(sources), ERHIExecSubmitFlags::None);

    failed_gate->Fail();
    std::atomic<bool> sync_returned{false};
    std::jthread sync_thread([&] {
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        sync_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(30ms);
    bool okay = Expect(
                    !sync_returned.load(std::memory_order_acquire),
                    "failed recording group did not wait for a later producer"
                ) &&
                Expect(
                    ordinary_callbacks.load() == 0 && success_callbacks.load() == 0,
                    "recording callbacks ran while a producer could still mutate its CommandList"
                );

    later_gate->Signal();
    sync_thread.join();
    okay &= Expect(
        ordinary_callbacks.load() == 2,
        "failed recording group did not run every ordinary cleanup callback exactly once"
    );
    okay &= Expect(
        success_callbacks.load() == 0,
        "failed recording group ran a success-only callback"
    );
    CmdSubmit drained_first  = first_list->Submit();
    CmdSubmit drained_second = second_list->Submit();
    okay &= Expect(
        drained_first.cmds.empty() && drained_first.callbacks.empty() &&
            drained_first.success_callbacks.empty() && drained_first.segments.empty() &&
            drained_second.cmds.empty() && drained_second.callbacks.empty() &&
            drained_second.success_callbacks.empty() && drained_second.segments.empty(),
        "rejection drain left a partial command, callback, scope, or submit segment behind"
    );

    // A second ordering boundary and shutdown must not replay drained cleanup.
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    RHIExecutor::ShutDown();
    okay &= Expect(
        ordinary_callbacks.load() == 2 && success_callbacks.load() == 0,
        "failed recording cleanup was not terminal or ran more than once"
    );
    return okay;
}

bool FailedCommitRejectsCompletedSources() {
    RHIExecutor::StartUp();

    std::atomic<int> ordinary_callbacks{0};
    std::atomic<int> success_callbacks{0};
    auto command_list = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    command_list->AddCallback([&ordinary_callbacks] {
        ordinary_callbacks.fetch_add(1);
    });
    command_list->AddSuccessCallback([&success_callbacks] {
        success_callbacks.fetch_add(1);
    });

    auto producer_complete = RHIRecordingGate::Create(true);
    auto graph_commit      = RHIRecordingGate::Create();
    Moer::Array<RHIRecordingSource> sources{};
    sources.emplace_back(RHIRecordingSource{
        .command_list = command_list,
        .completion   = producer_complete,
        .commit       = graph_commit,
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );

    std::atomic<bool> sync_returned{false};
    std::jthread sync_thread([&] {
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        sync_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(30ms);
    bool okay = Expect(
                    !sync_returned.load(std::memory_order_acquire),
                    "completed producer bypassed its pending graph commit"
                ) &&
                Expect(
                    ordinary_callbacks.load() == 0 &&
                        success_callbacks.load() == 0,
                    "callbacks ran before the graph transaction resolved"
                );

    graph_commit->Fail();
    sync_thread.join();
    okay &= Expect(
        ordinary_callbacks.load() == 1,
        "failed graph commit did not run ordinary cleanup exactly once"
    );
    okay &= Expect(
        success_callbacks.load() == 0,
        "failed graph commit ran a success-only callback"
    );

    CmdSubmit drained = command_list->Submit();
    okay &= Expect(
        drained.cmds.empty() && drained.callbacks.empty() &&
            drained.success_callbacks.empty() && drained.segments.empty(),
        "failed graph commit left a submittable source behind"
    );
    RHIExecutor::ShutDown();
    return okay;
}

bool MetadataFailureRejectsEveryProducerSignal() {
    RHIExecutor::StartUp();

    auto invalid_source = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    auto later_source   = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    FenceRef signal_fence = MoerNew(RecordingFenceProbe)();
    auto* signal_probe =
        static_cast<RecordingFenceProbe*>(signal_fence.Get());

    Moer::Array<RHIRecordingSource> sources{};
    sources.emplace_back(RHIRecordingSource{
        .command_list = invalid_source,
        .completion   = RHIRecordingGate::Create(true),
        .submit_metadata = RHIRecordingSubmitMetadata{
            .wait_fences = {
                RHIRecordingFencePoint{.fence = {}, .value = 1},
            },
        },
    });
    sources.emplace_back(RHIRecordingSource{
        .command_list = later_source,
        .completion   = RHIRecordingGate::Create(true),
        .submit_metadata = RHIRecordingSubmitMetadata{
            .signal_fences = {
                RHIRecordingFencePoint{.fence = signal_fence, .value = 7},
            },
        },
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    RHIExecutor::ShutDown();

    bool okay = Expect(
        signal_probe->RejectedValue() == 7,
        "metadata failure did not reject a later producer signal"
    );

    RHIExecutor::StartUp();
    FenceRef failed_gate_fence = MoerNew(RecordingFenceProbe)();
    auto* failed_gate_probe =
        static_cast<RecordingFenceProbe*>(failed_gate_fence.Get());
    auto failed_gate = RHIRecordingGate::Create();
    Moer::Array<RHIRecordingSource> failed_sources{};
    failed_sources.emplace_back(RHIRecordingSource{
        .command_list = Moer::MakeShared<CommandList>(EQueueType::Graphics),
        .completion   = failed_gate,
        .submit_metadata = RHIRecordingSubmitMetadata{
            .signal_fences = {
                RHIRecordingFencePoint{
                    .fence = failed_gate_fence,
                    .value = 11,
                },
            },
        },
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(failed_sources),
        ERHIExecSubmitFlags::None
    );
    failed_gate->Fail();
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    RHIExecutor::ShutDown();
    okay &= Expect(
        failed_gate_probe->RejectedValue() == 11,
        "failed recording gate did not reject its producer signal"
    );

    RHIExecutor::StartUp();
    FenceRef cancelled_fence = MoerNew(RecordingFenceProbe)();
    auto* cancelled_probe =
        static_cast<RecordingFenceProbe*>(cancelled_fence.Get());
    Moer::Array<RHIRecordingSource> cancelled_sources{};
    cancelled_sources.emplace_back(RHIRecordingSource{
        .command_list = Moer::MakeShared<CommandList>(EQueueType::Graphics),
        .completion   = RHIRecordingGate::Create(),
        .submit_metadata = RHIRecordingSubmitMetadata{
            .signal_fences = {
                RHIRecordingFencePoint{
                    .fence = cancelled_fence,
                    .value = 13,
                },
            },
        },
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(cancelled_sources),
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::ShutDown();
    okay &= Expect(
        cancelled_probe->RejectedValue() == 13,
        "shutdown cancellation did not reject its producer signal"
    );
    return okay;
}

bool BlockingWaitReportsTerminalStatus() {
    auto succeeded_gate = RHIRecordingGate::Create();
    std::atomic<ERHIRecordingStatus> waited_status{ERHIRecordingStatus::Pending};
    std::atomic<bool> wait_started{false};
    std::jthread waiter([&] {
        wait_started.store(true, std::memory_order_release);
        waited_status.store(succeeded_gate->Wait(), std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!wait_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(20ms);
    bool okay = Expect(
        waited_status.load(std::memory_order_acquire) == ERHIRecordingStatus::Pending,
        "blocking Wait returned before a Pending gate completed"
    );
    succeeded_gate->Signal();
    waiter.join();
    okay &= Expect(
        waited_status.load(std::memory_order_acquire) == ERHIRecordingStatus::Succeeded,
        "blocking Wait did not report Succeeded"
    );

    auto failed_gate = RHIRecordingGate::Create();
    failed_gate->Fail();
    const auto failed_begin  = std::chrono::steady_clock::now();
    const auto failed_status = failed_gate->Wait();
    const auto failed_elapsed = std::chrono::steady_clock::now() - failed_begin;
    okay &= Expect(
        failed_status == ERHIRecordingStatus::Failed,
        "blocking Wait did not report Failed"
    );
    okay &= Expect(failed_elapsed < 100ms, "terminal Failed gate did not return immediately");
    return okay;
}

bool ShutdownDrainsTerminalSourceBehindPendingHead() {
    RHIExecutor::StartUp();

    std::atomic<int> ordinary_callbacks{0};
    std::atomic<int> success_callbacks{0};
    auto pending_list  = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    auto terminal_list = Moer::MakeShared<CommandList>(EQueueType::Graphics);
    QueryToken pending_query =
        pending_list->BeginTimestampQuery("OpaqueShutdownQuery");
    pending_list->EndTimestampQuery(pending_query);
    QueryFuture pending_query_future = pending_query.GetFuture();
    terminal_list->AddCallback([&ordinary_callbacks] {
        ordinary_callbacks.fetch_add(1);
    });
    terminal_list->AddSuccessCallback([&success_callbacks] {
        success_callbacks.fetch_add(1);
    });

    auto pending_gate  = RHIRecordingGate::Create();
    auto terminal_gate = RHIRecordingGate::Create(true);
    RHIExecutor::Get().SubmitRecording(
        pending_list,
        pending_gate,
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::Get().SubmitRecording(
        terminal_list,
        terminal_gate,
        ERHIExecSubmitFlags::None
    );

    // The pending head keeps the already-complete second source in the handoff
    // FIFO. Shutdown must remain bounded, leave the first source opaque, and
    // drain only the terminal source's ordinary cleanup exactly once.
    RHIExecutor::ShutDown();

    CmdSubmit drained_terminal = terminal_list->Submit();
    const bool okay =
        Expect(
            ordinary_callbacks.load() == 1,
            "shutdown did not run terminal recording cleanup exactly once"
        ) &&
        Expect(
            success_callbacks.load() == 0,
            "shutdown ran a success-only callback for an unpublished recording"
        ) &&
        Expect(
            drained_terminal.cmds.empty() && drained_terminal.callbacks.empty() &&
                drained_terminal.success_callbacks.empty() &&
                drained_terminal.segments.empty(),
            "shutdown left a terminal cancelled recording source undrained"
        ) &&
        Expect(
            pending_gate->Status() == ERHIRecordingStatus::Pending,
            "shutdown changed the producer-owned pending gate"
        ) &&
        Expect(
            pending_query_future.IsReady() &&
                pending_query_future.Status() == QueryStatus::Error,
            "shutdown left an opaque recording query Pending"
        );

    pending_gate->Signal();
    return okay;
}

bool ShutdownUsesOneTerminalSnapshotForSignalAndQueryNotification() {
    RHIExecutor::StartUp();

    auto promoted_gate =
        RHIRecordingGate::Create();
    RecordingFenceProbe promoted_signal{};
    auto promoted_list =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    promoted_list->Signal(&promoted_signal, 31);

    std::atomic<int>  query_callbacks{0};
    std::atomic<int>  ordinary_callbacks{0};
    std::atomic<int>  success_callbacks{0};
    std::atomic<bool> query_observed_signal_rejection{false};
    std::atomic<bool> ordinary_observed_query_notification{false};
    QueryToken promoted_query =
        promoted_list->BeginTimestampQuery("PromotedDuringCancellation");
    promoted_list->EndTimestampQuery(promoted_query);
    QueryFuture promoted_future = promoted_query.GetFuture();
    promoted_future.Then([&](const QueryResult& _result) {
        query_observed_signal_rejection.store(
            _result.status == QueryStatus::Error &&
                promoted_signal.IsRejected(31),
            std::memory_order_release
        );
        query_callbacks.fetch_add(1, std::memory_order_release);
    });
    promoted_list->AddCallback([&] {
        ordinary_observed_query_notification.store(
            promoted_signal.IsRejected(31) &&
                query_callbacks.load(std::memory_order_acquire) == 1,
            std::memory_order_release
        );
        ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    promoted_list->AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_release);
    });

    // The first source is Pending when cancellation classification starts.
    // Rejecting the later terminal source promotes it synchronously after that
    // first observation but before Query notification. A second status read
    // would release the promoted callback without rejecting its signal.
    RecordingFenceProbe promotion_trigger(
        {},
        [promoted_gate] {
            promoted_gate->Signal();
        }
    );
    auto terminal_list =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    terminal_list->Signal(&promotion_trigger, 32);

    Moer::Array<RHIRecordingSource> sources{};
    sources.emplace_back(RHIRecordingSource{
        .command_list = promoted_list,
        .completion   = promoted_gate,
    });
    sources.emplace_back(RHIRecordingSource{
        .command_list = terminal_list,
        .completion   = RHIRecordingGate::Create(true),
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::ShutDown();

    CmdSubmit drained_promoted = promoted_list->Submit();
    const std::optional<QueryResult> result =
        promoted_future.TryGet();
    return Expect(
               promoted_gate->Status() == ERHIRecordingStatus::Succeeded,
               "test did not promote the Pending source during cancellation"
           ) &&
           Expect(
               promoted_signal.IsRejected(31),
               "newly-terminal source signal was not rejected by its drain owner"
           ) &&
           Expect(
               query_callbacks.load(std::memory_order_acquire) == 1 &&
                   query_observed_signal_rejection.load(
                       std::memory_order_acquire
                   ),
               "newly-terminal Query callback ran before signal rejection or not exactly once"
           ) &&
           Expect(
               ordinary_callbacks.load(std::memory_order_acquire) == 1 &&
                   ordinary_observed_query_notification.load(
                       std::memory_order_acquire
                   ),
               "newly-terminal ordinary cleanup did not run once after Query notification"
           ) &&
           Expect(
               success_callbacks.load(std::memory_order_acquire) == 0,
               "shutdown cancellation invoked a success-only callback"
           ) &&
           Expect(
               result.has_value() && result->status == QueryStatus::Error,
               "newly-terminal Query did not preserve its cancellation Error"
           ) &&
           Expect(
               drained_promoted.cmds.empty() &&
                   drained_promoted.callbacks.empty() &&
                   drained_promoted.success_callbacks.empty() &&
                   drained_promoted.query_tokens.empty() &&
                   drained_promoted.segments.empty(),
               "newly-terminal source was not destructively drained"
           );
}

bool ShutdownCancelsAnUnsignalledGate() {
    RHIExecutorRecordingHandoffQueue queue{};
    queue.Start();

    ResolutionLog log{};
    auto never_signalled = RHIRecordingGate::Create();
    queue.EnqueueRecording(RHIExecutorRecordingHandoffWork{
        .prerequisites = {never_signalled},
        .resolve = [&log](ERHIRecordingHandoffResult _result) {
            log.Push(1, _result);
        },
    });
    queue.RouteReady(RHIExecutorRecordingHandoffWork{
        .prerequisites = {},
        .resolve = [&log](ERHIRecordingHandoffResult _result) {
            log.Push(2, _result);
        },
    });

    const auto begin = std::chrono::steady_clock::now();
    queue.ShutDown();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto entries = log.Snapshot();

    bool okay = Expect(elapsed < 1s, "shutdown waited for an unsignalled gate") &&
                Expect(entries.size() == 2, "shutdown did not resolve every FIFO entry") &&
                Expect(entries[0].first == 1 && entries[1].first == 2, "reject order changed") &&
                Expect(
                    entries[0].second == ERHIRecordingHandoffResult::Cancel &&
                        entries[1].second == ERHIRecordingHandoffResult::Cancel,
                    "shutdown consumed cancelled work"
                );

    std::atomic<int> stopped_result{-1};
    queue.RouteReady(RHIExecutorRecordingHandoffWork{
        .prerequisites = {},
        .resolve = [&stopped_result](ERHIRecordingHandoffResult _result) {
            stopped_result.store(
                _result == ERHIRecordingHandoffResult::Cancel ? 1 : 0,
                std::memory_order_release
            );
        },
    });
    okay &= Expect(
        stopped_result.load(std::memory_order_acquire) == 1,
        "post-shutdown work was not rejected synchronously"
    );

    // The queue is restartable across device lifetimes.
    queue.Start();
    ResolutionLog restarted{};
    queue.EnqueueRecording(RHIExecutorRecordingHandoffWork{
        .prerequisites = {RHIRecordingGate::Create(true)},
        .resolve = [&restarted](ERHIRecordingHandoffResult _result) {
            restarted.Push(3, _result);
        },
    });
    okay &= Expect(restarted.WaitForSize(1), "restarted queue did not consume work");
    const auto restarted_entries = restarted.Snapshot();
    okay &= Expect(
        restarted_entries.size() == 1 &&
            restarted_entries[0].second == ERHIRecordingHandoffResult::Consume,
        "restarted queue returned the wrong resolution"
    );
    queue.ShutDown();
    return okay;
}

bool ShutdownDrainsAcceptedReadyWork() {
    RHIExecutorRecordingHandoffQueue queue{};
    queue.Start();

    ResolutionLog        log{};
    std::mutex           mutex;
    std::condition_variable cv;
    bool                 first_entered{false};
    bool                 release_first{false};

    queue.RouteReady(RHIExecutorRecordingHandoffWork{
        .resolve = [&](ERHIRecordingHandoffResult _result) {
            {
                std::unique_lock lock(mutex);
                first_entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release_first; });
            }
            log.Push(1, _result);
        },
    });
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 2s, [&] { return first_entered; })) {
            release_first = true;
            cv.notify_all();
            queue.ShutDown();
            return Expect(false, "ready shutdown head did not enter the Executor");
        }
    }
    queue.RouteReady(RHIExecutorRecordingHandoffWork{
        .resolve = [&log](ERHIRecordingHandoffResult _result) {
            log.Push(2, _result);
        },
    });

    std::jthread shutdown_thread([&] { queue.ShutDown(); });
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    cv.notify_all();
    shutdown_thread.join();

    const auto entries = log.Snapshot();
    return Expect(entries.size() == 2, "shutdown dropped accepted ready work") &&
           Expect(entries[0].first == 1 && entries[1].first == 2, "shutdown reordered ready work") &&
           Expect(
               entries[0].second == ERHIRecordingHandoffResult::Consume &&
                   entries[1].second == ERHIRecordingHandoffResult::Consume,
               "shutdown cancelled work that was already safe for the Executor to drain"
           );
}

} // namespace

int main() {
    if (!CommandListSignalTracksNativeAcceptanceAndRejection() ||
        !FrontendQueryRejectionTerminalizesBeforeOrdinaryCallbacks() ||
        !FrontendCompletionRejectionIsOneTerminalTransaction() ||
        !FrontendPresentRejectionPublishesBatchBeforeReceipt() ||
        !DeferredOpaqueCancellationTicketSurvivesSubmit() ||
        !DirectCommandListGroupPreflightRejectsEverySource() ||
        !DirectCommandListGroupMaterializationFailureRejectsPrefixAndSuffix() ||
        !RecordingPreflightFailureTerminalizesEveryQueryGeneration() ||
        !RecordingPreflightFailureTerminalizesEveryCompletionGeneration() ||
        !SubmitRecordingRefreshesAStaleQueryCancellationView() ||
        !SubmitRecordingRefreshesAStaleCompletionCancellationView() ||
        !PerSourceGatesPreserveFifo() ||
        !ReadyWorkUsesStableExecutorOwnerAndCannotBeOvertaken() ||
        !FailedGateRejectsOnlyItsGroupAndPreservesFifo() ||
        !FailedRecordingDrainsCleanupExactlyOnce() ||
        !FailedCommitRejectsCompletedSources() ||
        !MetadataFailureRejectsEveryProducerSignal() ||
        !BlockingWaitReportsTerminalStatus() ||
        !ShutdownDrainsTerminalSourceBehindPendingHead() ||
        !ShutdownUsesOneTerminalSnapshotForSignalAndQueryNotification() ||
        !ShutdownCancelsAnUnsignalledGate() ||
        !ShutdownDrainsAcceptedReadyWork()) {
        return EXIT_FAILURE;
    }
    std::cout << "RHIExecutor recording handoff contract passed\n";
    return EXIT_SUCCESS;
}
