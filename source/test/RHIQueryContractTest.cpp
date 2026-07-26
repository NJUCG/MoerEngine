#include "rhi/RHICommand.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIQuery.h"

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

bool WaitForFlag(const std::atomic_bool& _flag, std::chrono::milliseconds _timeout = 500ms) {
    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    while (!_flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return _flag.load(std::memory_order_acquire);
}

class QueryFenceProbe final : public Fence {
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

const QueryCmd& QueryAt(const CmdSubmit& _submit, size_t _index) {
    Expect(_index < _submit.cmds.size(), "query command index is out of range");
    Expect(_submit.cmds[_index]->Type() == Command::EType::Query, "expected QueryCmd");
    return *static_cast<const QueryCmd*>(_submit.cmds[_index].get());
}

TimestampQueryResult TestTimestampResult() {
    return TimestampQueryResult{
        .begin_tick     = 120,
        .end_tick       = 180,
        .valid_bits     = 64,
        .tick_period_ns = 2.0,
        .duration_ns    = 120.0,
    };
}

void FutureIsThreadSafeOneShotAndContainsCallbackExceptions() {
    CommandList list(EQueueType::Graphics);
    QueryToken  token = list.BeginTimestampQuery("ConcurrentTimestamp");
    list.EndTimestampQuery(token);
    CmdSubmit submit = list.Submit();

    QueryFuture future = token.GetFuture();
    Expect(future.Valid(), "recorded query returned an invalid future");
    Expect(!future.IsReady(), "new query future was already terminal");
    Expect(future.Status() == QueryStatus::Pending, "new query was not Pending");
    Expect(!future.WaitFor(1ms), "pending query unexpectedly satisfied WaitFor");
    Expect(!future.TryGet().has_value(), "pending query unexpectedly had a result");

    constexpr int            callback_thread_count = 32;
    std::atomic<int>         callback_count{0};
    std::atomic<bool>        start{false};
    std::vector<std::thread> threads{};
    threads.reserve(callback_thread_count);
    for (int index = 0; index < callback_thread_count; ++index) {
        threads.emplace_back([&, future] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            future.Then([&](const QueryResult& _result) {
                Expect(
                    _result.status == QueryStatus::Ready, "concurrent callback observed a non-Ready result"
                );
                callback_count.fetch_add(1, std::memory_order_relaxed);
            });
        });
    }

    future.Then([](const QueryResult&) {
        throw std::runtime_error("client callback failure");
    });
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    Expect(
        QueryBackendAccess::ResolveTimestamp(token, TestTimestampResult()),
        "first timestamp completion did not win the one-shot transition"
    );
    future.Wait();
    Expect(future.WaitFor(1ms), "terminal future did not satisfy WaitFor");
    Expect(
        callback_count.load(std::memory_order_relaxed) == callback_thread_count,
        "pre-terminal callbacks were not invoked exactly once"
    );

    future.Then([&](const QueryResult& _result) {
        Expect(_result.status == QueryStatus::Ready, "post-terminal callback observed the wrong status");
        callback_count.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("post-terminal callback failure");
    });
    Expect(
        callback_count.load(std::memory_order_relaxed) == callback_thread_count + 1,
        "post-terminal callback was not invoked synchronously exactly once"
    );

    Expect(
        !QueryBackendAccess::ResolveErrorIfPending(token, "late rejection"),
        "a second terminal transition overwrote the Ready result"
    );
    const QueryResult result = future.Get();
    Expect(result.status == QueryStatus::Ready, "Get lost Ready status");
    Expect(result.kind == QueryKind::Timestamp, "Get lost query kind");
    Expect(result.query_id == token.Id(), "Get lost query identity");
    Expect(result.name == "ConcurrentTimestamp", "Get lost query name");
    Expect(result.error_reason.empty(), "Ready result retained an error reason");
    const auto* timestamp = std::get_if<TimestampQueryResult>(&result.payload);
    Expect(timestamp != nullptr, "Ready result has the wrong payload type");
    Expect(
        timestamp->begin_tick == 120 && timestamp->end_tick == 180 && timestamp->valid_bits == 64 &&
            timestamp->duration_ns == 120.0,
        "timestamp payload changed during resolution"
    );

    // Simulate Completion ordering: backend resolves first and the ordinary
    // rejection fallback becomes an idempotent no-op.
    for (auto& callback : submit.callbacks) {
        callback();
    }
    submit.callbacks.clear();
    submit.query_tokens.clear();
}

void ReadyAndErrorResolutionRaceHasOneWinner() {
    constexpr int race_iterations = 128;
    for (int iteration = 0; iteration < race_iterations; ++iteration) {
        CommandList list(EQueueType::Graphics);
        QueryToken  token = list.BeginTimestampQuery("ResolveRace");
        list.EndTimestampQuery(token);
        CmdSubmit submit = list.Submit();

        std::atomic<int>  callback_count{0};
        std::atomic<bool> start{false};
        bool              ready_won = false;
        bool              error_won = false;
        token.Then([&](const QueryResult& _result) {
            Expect(
                _result.status == QueryStatus::Ready || _result.status == QueryStatus::Error,
                "resolve race callback observed a non-terminal result"
            );
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });

        std::thread ready_thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            ready_won = QueryBackendAccess::ResolveTimestamp(token, TestTimestampResult());
        });
        std::thread error_thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            error_won = QueryBackendAccess::ResolveErrorIfPending(token, "injected completion failure");
        });
        start.store(true, std::memory_order_release);
        ready_thread.join();
        error_thread.join();

        Expect(ready_won != error_won, "Ready/Error race did not have exactly one winner");
        Expect(token.GetFuture().IsReady(), "Ready/Error race left the future Pending");
        Expect(
            callback_count.load(std::memory_order_relaxed) == 1,
            "Ready/Error race invoked its callback more or less than once"
        );
        const QueryResult result = token.GetFuture().Get();
        Expect(
            result.status == (ready_won ? QueryStatus::Ready : QueryStatus::Error),
            "Ready/Error race result disagrees with the winning resolver"
        );

        for (auto& callback : submit.callbacks) {
            callback();
        }
        submit.callbacks.clear();
        submit.query_tokens.clear();
    }
}

void ReadyBatchPublishesEveryPeerBeforeNotification() {
    CommandList list(EQueueType::Graphics);
    QueryToken  first = list.BeginTimestampQuery("ReadyBatchFirst");
    list.EndTimestampQuery(first);
    QueryToken second = list.BeginTimestampQuery("ReadyBatchSecond");
    list.EndTimestampQuery(second);
    CmdSubmit submit = list.Submit();

    const QueryFuture second_future = second.GetFuture();
    std::atomic<int>  callback_count{0};
    std::atomic_bool  first_observed_second_ready{false};
    first.Then([&](const QueryResult& _result) {
        const bool peer_ready = second_future.WaitFor(100ms) && second_future.Status() == QueryStatus::Ready;
        first_observed_second_ready.store(
            _result.status == QueryStatus::Ready && peer_ready, std::memory_order_release
        );
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });
    second.Then([&](const QueryResult& _result) {
        if (_result.status == QueryStatus::Ready) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const QueryPublishBatch batch = QueryBackendAccess::BeginPublishBatch();
    Expect(
        QueryBackendAccess::PublishTimestamp(first, TestTimestampResult(), batch),
        "Ready batch did not publish its first query"
    );
    Expect(
        QueryBackendAccess::PublishTimestamp(second, TestTimestampResult(), batch),
        "Ready batch did not publish its second query"
    );
    Expect(
        callback_count.load(std::memory_order_relaxed) == 0,
        "Ready batch released a callback during its publish phase"
    );

    QueryBackendAccess::NotifyTerminals(submit.query_tokens, batch);
    Expect(
        first_observed_second_ready.load(std::memory_order_acquire),
        "first Ready callback could not observe its peer as terminal"
    );
    Expect(
        callback_count.load(std::memory_order_relaxed) == 2,
        "Ready batch did not notify both callbacks exactly once"
    );
}

void BulkErrorPathsPublishEveryPeerBeforeNotification() {
    QueryFuture      submit_first_future{};
    QueryFuture      submit_second_future{};
    std::atomic<int> submit_callback_count{0};
    std::atomic_bool submit_first_observed_second_error{false};
    {
        CommandList list(EQueueType::Graphics);
        QueryToken  first = list.BeginTimestampQuery("SubmitErrorFirst");
        list.EndTimestampQuery(first);
        QueryToken second = list.BeginTimestampQuery("SubmitErrorSecond");
        list.EndTimestampQuery(second);
        submit_first_future  = first.GetFuture();
        submit_second_future = second.GetFuture();

        first.Then([&](const QueryResult& _result) {
            const bool peer_error =
                submit_second_future.WaitFor(100ms) && submit_second_future.Status() == QueryStatus::Error;
            submit_first_observed_second_error.store(
                _result.status == QueryStatus::Error && peer_error, std::memory_order_release
            );
            submit_callback_count.fetch_add(1, std::memory_order_relaxed);
        });
        second.Then([&](const QueryResult& _result) {
            if (_result.status == QueryStatus::Error) {
                submit_callback_count.fetch_add(1, std::memory_order_relaxed);
            }
        });

        [[maybe_unused]] CmdSubmit abandoned = list.Submit();
    }
    Expect(
        submit_first_observed_second_error.load(std::memory_order_acquire),
        "CmdSubmit Error callback could not observe its peer as terminal"
    );
    Expect(
        submit_callback_count.load(std::memory_order_relaxed) == 2,
        "CmdSubmit bulk Error did not notify both callbacks exactly once"
    );

    CommandList           cancellation_list(EQueueType::Graphics);
    QueryCancellationView cancellation = cancellation_list.GetQueryCancellationView();
    QueryToken            cancel_first = cancellation_list.BeginTimestampQuery("CancellationFirst");
    cancellation_list.EndTimestampQuery(cancel_first);
    QueryToken cancel_second = cancellation_list.BeginTimestampQuery("CancellationSecond");
    cancellation_list.EndTimestampQuery(cancel_second);

    const QueryFuture cancel_second_future = cancel_second.GetFuture();
    std::atomic<int>  cancellation_callback_count{0};
    std::atomic_bool  cancel_first_observed_second_error{false};
    cancel_first.Then([&](const QueryResult& _result) {
        const bool peer_error =
            cancel_second_future.WaitFor(100ms) && cancel_second_future.Status() == QueryStatus::Error;
        cancel_first_observed_second_error.store(
            _result.status == QueryStatus::Error && peer_error, std::memory_order_release
        );
        cancellation_callback_count.fetch_add(1, std::memory_order_relaxed);
    });
    cancel_second.Then([&](const QueryResult& _result) {
        if (_result.status == QueryStatus::Error) {
            cancellation_callback_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::atomic_bool cancellation_won{false};
    std::thread cancellation_thread([&] {
        cancellation_won.store(
            cancellation.Cancel("test cancellation"),
            std::memory_order_release
        );
    });
    cancellation_thread.join();
    Expect(
        cancellation_won.load(std::memory_order_acquire),
        "QueryCancellationView did not win its cancellation transition"
    );
    Expect(
        cancel_first.GetFuture().Status() == QueryStatus::Error &&
            cancel_second.GetFuture().Status() == QueryStatus::Error,
        "cancellation did not publish every token before returning"
    );
    Expect(
        cancellation_callback_count.load(std::memory_order_relaxed) == 0,
        "opaque cancellation released callbacks on the cancellation thread"
    );

    auto cleanup = cancellation_list.DrainOrdinaryCallbacksForRejection();
    Expect(
        cancel_first_observed_second_error.load(std::memory_order_acquire),
        "deferred cancellation callback could not observe its peer as terminal"
    );
    Expect(
        cancellation_callback_count.load(std::memory_order_relaxed) == 2,
        "ownership-boundary cancellation did not notify both callbacks exactly once"
    );
    for (auto& callback : cleanup) {
        callback();
    }
}

void LosingBatchCannotStealNotificationOwnership() {
    CommandList list(EQueueType::Graphics);
    QueryToken  token = list.BeginTimestampQuery("PublishTicketOwner");
    list.EndTimestampQuery(token);
    CmdSubmit submit = list.Submit();

    std::atomic<int> callback_count{0};
    std::atomic_bool callback_observed_ready{false};
    token.Then([&](const QueryResult& _result) {
        callback_observed_ready.store(_result.status == QueryStatus::Ready, std::memory_order_release);
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    const QueryPublishBatch ready_batch = QueryBackendAccess::BeginPublishBatch();
    Expect(
        QueryBackendAccess::PublishTimestamp(token, TestTimestampResult(), ready_batch),
        "Ready publisher did not win the terminal transition"
    );

    QueryBackendAccess::ResolveErrorsIfPending(submit.query_tokens, "competing rejection");
    Expect(
        callback_count.load(std::memory_order_relaxed) == 0,
        "losing Error batch stole the Ready publisher's callback ownership"
    );

    QueryBackendAccess::NotifyTerminal(token, ready_batch);
    Expect(
        callback_count.load(std::memory_order_relaxed) == 1 &&
            callback_observed_ready.load(std::memory_order_acquire),
        "Ready publisher did not retain exactly-once callback ownership"
    );

    CommandList stored_ticket_list(EQueueType::Graphics);
    QueryToken  stored_ticket_token =
        stored_ticket_list.BeginTimestampQuery("StoredPublishTicketOwner");
    stored_ticket_list.EndTimestampQuery(stored_ticket_token);
    CmdSubmit stored_ticket_submit = stored_ticket_list.Submit();

    std::atomic<int> stored_ticket_callback_count{0};
    stored_ticket_token.Then([&](const QueryResult& _result) {
        if (_result.status == QueryStatus::Ready) {
            stored_ticket_callback_count.fetch_add(
                1, std::memory_order_relaxed
            );
        }
    });

    const QueryPublishBatch stored_ready_batch =
        QueryBackendAccess::BeginPublishBatch();
    stored_ticket_submit.query_publish_batch = stored_ready_batch;
    Expect(
        QueryBackendAccess::PublishTimestamp(
            stored_ticket_token,
            TestTimestampResult(),
            stored_ready_batch
        ),
        "stored-ticket Ready publisher did not win"
    );

    const QueryPublishBatch competing_error_batch =
        QueryBackendAccess::BeginPublishBatch();
    stored_ticket_submit.PublishPendingQueryErrors(
        "competing submit rejection",
        competing_error_batch
    );
    stored_ticket_submit.NotifyPendingQueries(
        stored_ticket_submit.query_publish_batch
    );
    Expect(
        stored_ticket_callback_count.load(std::memory_order_relaxed) == 1,
        "CmdSubmit overwrote the stored winning notification ticket"
    );
    QueryBackendAccess::NotifyTerminal(
        stored_ticket_token,
        stored_ready_batch
    );
    Expect(
        stored_ticket_callback_count.load(std::memory_order_relaxed) == 1,
        "stored winning ticket released its callback more than once"
    );
}

void PublishWakesWaitersBeforeDeferredCallbackNotification() {
    CommandList           list(EQueueType::Graphics);
    QueryCancellationView cancellation = list.GetQueryCancellationView();
    QueryToken            token        = list.BeginTimestampQuery("DeferredNotification");
    list.EndTimestampQuery(token);
    QueryFuture future = token.GetFuture();

    std::atomic<int>         callback_count{0};
    std::atomic_bool         waiter_entered{false};
    std::atomic_bool         waiter_returned{false};
    std::atomic<QueryStatus> waiter_status{QueryStatus::Pending};
    future.Then([&](const QueryResult& _result) {
        if (_result.status == QueryStatus::Error) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread waiter([&] {
        waiter_entered.store(true, std::memory_order_release);
        future.Wait();
        waiter_status.store(future.Status(), std::memory_order_release);
        waiter_returned.store(true, std::memory_order_release);
    });
    const bool  waiter_started = WaitForFlag(waiter_entered);
    std::this_thread::sleep_for(10ms);
    const bool pending_before_publication = !waiter_returned.load(std::memory_order_acquire);

    const QueryPublishBatch batch     = QueryBackendAccess::BeginPublishBatch();
    const bool              published = cancellation.PublishCancellation("deferred shutdown", batch);
    const bool              woke_before_notification      = published && WaitForFlag(waiter_returned);
    const int               callbacks_before_notification = callback_count.load(std::memory_order_relaxed);

    // Always release the callback owner before joining so a regression reports
    // a bounded failure instead of hanging the test executable.
    cancellation.NotifyCancellation(batch);
    if (!published) {
        QueryBackendAccess::ResolveErrorIfPending(token, "deferred shutdown test cleanup");
    }
    waiter.join();

    Expect(waiter_started, "waiter did not enter QueryFuture::Wait");
    Expect(pending_before_publication, "Pending QueryFuture waiter returned before publication");
    Expect(published, "deferred cancellation did not publish");
    Expect(
        woke_before_notification && waiter_status.load(std::memory_order_acquire) == QueryStatus::Error,
        "Publish did not wake a pre-blocked waiter before callback notification"
    );
    Expect(callbacks_before_notification == 0, "Publish released a callback that was intentionally deferred");
    Expect(
        callback_count.load(std::memory_order_relaxed) == 1, "deferred callback was not released exactly once"
    );

    auto cleanup = list.DrainOrdinaryCallbacksForRejection();
    for (auto& callback : cleanup) {
        callback();
    }
}

void CallbacksMayReleaseTheirLastQueryHandles() {
    CommandList terminal_list(EQueueType::Graphics);
    QueryToken  terminal_token = terminal_list.BeginTimestampQuery("SynchronousSelfRelease");
    terminal_list.EndTimestampQuery(terminal_token);
    CmdSubmit   terminal_submit = terminal_list.Submit();
    QueryFuture terminal_future = terminal_token.GetFuture();
    Expect(
        QueryBackendAccess::ResolveTimestamp(terminal_token, TestTimestampResult()),
        "self-release setup did not resolve its query"
    );

    // Leave terminal_future as the only state owner before Then invokes the
    // already-terminal callback synchronously.
    terminal_submit.cmds.clear();
    terminal_submit.callbacks.clear();
    terminal_submit.query_tokens.clear();
    terminal_token = {};

    std::atomic<int> terminal_callback_count{0};
    std::atomic_bool result_survived_synchronous_release{false};
    terminal_future.Then([&](const QueryResult& _result) {
        terminal_future = {};
        result_survived_synchronous_release.store(
            _result.status == QueryStatus::Ready && _result.name == "SynchronousSelfRelease",
            std::memory_order_release
        );
        terminal_callback_count.fetch_add(1, std::memory_order_relaxed);
    });
    Expect(
        !terminal_future.Valid() && result_survived_synchronous_release.load(std::memory_order_acquire) &&
            terminal_callback_count.load(std::memory_order_relaxed) == 1,
        "synchronous Then callback could not release its last QueryFuture safely"
    );

    CommandList notify_list(EQueueType::Graphics);
    QueryToken  notify_token = notify_list.BeginTimestampQuery("NotifySelfRelease");
    notify_list.EndTimestampQuery(notify_token);
    CmdSubmit   notify_submit = notify_list.Submit();
    QueryFuture notify_future = notify_token.GetFuture();

    std::atomic<int> notify_callback_count{0};
    std::atomic_bool first_result_survived_release{false};
    std::atomic_bool second_callback_observed_ready{false};
    notify_future.Then([&](const QueryResult& _result) {
        notify_future = {};
        notify_token  = {};
        first_result_survived_release.store(
            _result.status == QueryStatus::Ready && _result.name == "NotifySelfRelease",
            std::memory_order_release
        );
        notify_callback_count.fetch_add(1, std::memory_order_relaxed);
    });
    notify_future.Then([&](const QueryResult& _result) {
        second_callback_observed_ready.store(_result.status == QueryStatus::Ready, std::memory_order_release);
        notify_callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    // Remove every packet-held token. NotifyTerminal's local strong state must
    // carry the result and remaining callbacks after the first callback drops
    // the last public handles.
    notify_submit.cmds.clear();
    notify_submit.callbacks.clear();
    notify_submit.query_tokens.clear();
    const QueryPublishBatch batch = QueryBackendAccess::BeginPublishBatch();
    Expect(
        QueryBackendAccess::PublishTimestamp(notify_token, TestTimestampResult(), batch),
        "multi-callback self-release query was not published"
    );
    QueryBackendAccess::NotifyTerminal(notify_token, batch);

    Expect(
        !notify_future.Valid() && !notify_token.Valid() &&
            first_result_survived_release.load(std::memory_order_acquire) &&
            second_callback_observed_ready.load(std::memory_order_acquire) &&
            notify_callback_count.load(std::memory_order_relaxed) == 2,
        "self-release invalidated the result or skipped a later callback"
    );
}

void PairingIsSameListSameKindAndStrictLifo() {
    CommandList first(EQueueType::Compute);
    QueryToken  outer = first.BeginTimestampQuery("Outer");
    QueryToken  inner = first.BeginTimestampQuery("Inner");

    ExpectThrows<std::logic_error>(
        [&] {
            first.EndTimestampQuery(outer);
        },
        "out-of-order query end was accepted"
    );
    first.EndTimestampQuery(inner);
    first.EndTimestampQuery(outer);
    CmdSubmit nested = first.Submit();
    Expect(nested.cmds.size() == 4, "nested query pair emitted the wrong command count");
    Expect(QueryAt(nested, 0).IsBegin(), "outer query did not begin first");
    Expect(QueryAt(nested, 1).IsBegin(), "inner query did not begin second");
    Expect(QueryAt(nested, 2).IsEnd(), "inner query did not end first");
    Expect(QueryAt(nested, 3).IsEnd(), "outer query did not end last");
    Expect(
        QueryAt(nested, 0).Token().Id() == QueryAt(nested, 3).Token().Id() &&
            QueryAt(nested, 1).Token().Id() == QueryAt(nested, 2).Token().Id(),
        "nested query command identities were not paired"
    );
    Expect(
        QueryAt(nested, 0).GetQueueType() == EQueueType::Compute, "query command lost its recording queue"
    );

    CommandList owner(EQueueType::Graphics);
    CommandList foreign(EQueueType::Graphics);
    QueryToken  owned = owner.BeginTimestampQuery("Owned");
    ExpectThrows<std::invalid_argument>(
        [&] {
            foreign.EndTimestampQuery(owned);
        },
        "a different CommandList accepted the token"
    );
    owner.EndTimestampQuery(owned);
    CmdSubmit owner_submit = owner.Submit();

    QueryToken foreign_token = foreign.BeginTimestampQuery("Foreign");
    foreign.EndTimestampQuery(foreign_token);
    CmdSubmit foreign_submit = foreign.Submit();

    ExpectThrows<std::invalid_argument>(
        [&] {
            CommandList invalid_owner;
            invalid_owner.EndTimestampQuery(QueryToken{});
        },
        "an invalid token was accepted"
    );

    // Keep these test-only packets from reporting abandonment during teardown.
    QueryBackendAccess::ResolveErrorIfPending(outer, "test retired");
    QueryBackendAccess::ResolveErrorIfPending(inner, "test retired");
    QueryBackendAccess::ResolveErrorIfPending(owned, "test retired");
    QueryBackendAccess::ResolveErrorIfPending(foreign_token, "test retired");
}

void InvalidSubmissionRejectionAndDestructionAreTerminal() {
    QueryFuture unclosed_future{};
    {
        CommandList unclosed(EQueueType::Graphics);
        QueryToken  token = unclosed.BeginTimestampQuery("UnclosedSubmit");
        unclosed_future   = token.GetFuture();
        ExpectThrows<std::logic_error>(
            [&] {
                [[maybe_unused]] CmdSubmit rejected = unclosed.Submit();
            },
            "Submit accepted an unclosed timestamp query"
        );
        Expect(unclosed.IsEmpty(), "invalid Submit retained a partial query stream");
    }
    QueryResult unclosed_result = unclosed_future.Get();
    Expect(
        unclosed_result.status == QueryStatus::Error && !unclosed_result.error_reason.empty(),
        "invalid Submit did not terminalize its query"
    );

    QueryFuture drained_future{};
    {
        CommandList rejected(EQueueType::Graphics);
        QueryToken  token = rejected.BeginTimestampQuery("Rejected");
        rejected.EndTimestampQuery(token);
        drained_future = token.GetFuture();
        auto callbacks = rejected.DrainOrdinaryCallbacksForRejection();
        Expect(drained_future.Status() == QueryStatus::Error, "rejection drain left a closed query Pending");
        for (auto& callback : callbacks) {
            callback();
        }
        Expect(rejected.IsEmpty(), "rejection drain retained query state");
    }

    QueryFuture list_destruction_future{};
    {
        CommandList abandoned(EQueueType::Graphics);
        QueryToken  token = abandoned.BeginTimestampQuery("AbandonedList");
        abandoned.EndTimestampQuery(token);
        list_destruction_future = token.GetFuture();
    }
    Expect(
        list_destruction_future.Get().status == QueryStatus::Error,
        "CommandList destruction left a query Pending"
    );

    QueryFuture submit_destruction_future{};
    {
        CommandList list(EQueueType::Graphics);
        QueryToken  token = list.BeginTimestampQuery("AbandonedSubmit");
        list.EndTimestampQuery(token);
        submit_destruction_future                   = token.GetFuture();
        [[maybe_unused]] CmdSubmit abandoned_submit = list.Submit();
    }
    Expect(
        submit_destruction_future.Get().status == QueryStatus::Error,
        "CmdSubmit destruction left a query Pending"
    );

    CommandList copy(EQueueType::Copy);
    QueryToken  copy_token = copy.BeginTimestampQuery("UnsupportedCopy");
    Expect(
        copy_token.GetFuture().Get().status == QueryStatus::Error,
        "Copy queue timestamp query did not fail closed"
    );
    copy.EndTimestampQuery(copy_token);
    Expect(copy.IsEmpty(), "Copy queue capability failure emitted a QueryCmd");
}

void SignalRejectionPrecedesQueryCallbacksAndPreservesReentrantState() {
    QueryFenceProbe  list_destruction_fence{};
    std::atomic<int> list_destruction_callback_count{0};
    std::atomic_bool list_destruction_observed_signal{false};
    {
        CommandList list(EQueueType::Graphics);
        list.Signal(&list_destruction_fence, 11);
        QueryToken token = list.BeginTimestampQuery("ListDestructionOrder");
        list.EndTimestampQuery(token);
        token.Then([&](const QueryResult& _result) {
            list_destruction_observed_signal.store(
                _result.status == QueryStatus::Error && list_destruction_fence.IsRejected(11),
                std::memory_order_release
            );
            list_destruction_callback_count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    Expect(
        list_destruction_observed_signal.load(std::memory_order_acquire) &&
            list_destruction_callback_count.load(std::memory_order_relaxed) == 1,
        "CommandList destruction notified Query before rejecting its signal"
    );

    QueryFenceProbe  submit_destruction_fence{};
    std::atomic<int> submit_destruction_callback_count{0};
    std::atomic_bool submit_destruction_observed_signal{false};
    {
        CommandList list(EQueueType::Graphics);
        list.Signal(&submit_destruction_fence, 12);
        QueryToken token = list.BeginTimestampQuery("SubmitDestructionOrder");
        list.EndTimestampQuery(token);
        token.Then([&](const QueryResult& _result) {
            submit_destruction_observed_signal.store(
                _result.status == QueryStatus::Error && submit_destruction_fence.IsRejected(12),
                std::memory_order_release
            );
            submit_destruction_callback_count.fetch_add(1, std::memory_order_relaxed);
        });
        [[maybe_unused]] CmdSubmit submit = list.Submit();
    }
    Expect(
        submit_destruction_observed_signal.load(std::memory_order_acquire) &&
            submit_destruction_callback_count.load(std::memory_order_relaxed) == 1,
        "CmdSubmit destruction notified Query before rejecting its signal"
    );

    QueryFenceProbe  replaced_submit_fence{};
    QueryFenceProbe  incoming_submit_fence{};
    QueryFenceProbe  reentrant_submit_fence{};
    std::atomic<int> submit_move_callback_count{0};
    std::atomic_bool submit_move_observed_signal{false};
    CommandList      old_submit_list(EQueueType::Graphics);
    old_submit_list.Signal(&replaced_submit_fence, 21);
    QueryToken replaced_submit_token = old_submit_list.BeginTimestampQuery("ReplacedSubmit");
    old_submit_list.EndTimestampQuery(replaced_submit_token);
    CmdSubmit destination_submit = old_submit_list.Submit();
    replaced_submit_token.Then([&](const QueryResult& _result) {
        submit_move_observed_signal.store(
            _result.status == QueryStatus::Error && replaced_submit_fence.IsRejected(21),
            std::memory_order_release
        );
        destination_submit.Signal(&reentrant_submit_fence, 23);
        submit_move_callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    CommandList incoming_submit_list(EQueueType::Graphics);
    incoming_submit_list.Signal(&incoming_submit_fence, 22);
    CmdSubmit incoming_submit = incoming_submit_list.Submit();
    destination_submit        = std::move(incoming_submit);
    Expect(
        submit_move_observed_signal.load(std::memory_order_acquire) &&
            submit_move_callback_count.load(std::memory_order_relaxed) == 1,
        "CmdSubmit move assignment notified Query before rejecting its signal"
    );
    Expect(
        destination_submit.signal_events.size() == 2, "CmdSubmit move assignment overwrote a reentrant signal"
    );
    destination_submit.RejectPendingSignals();
    Expect(
        incoming_submit_fence.IsRejected(22) && reentrant_submit_fence.IsRejected(23),
        "CmdSubmit move assignment did not preserve both installed signals"
    );

    QueryFenceProbe  replaced_list_fence{};
    QueryFenceProbe  incoming_list_fence{};
    QueryFenceProbe  reentrant_list_fence{};
    std::atomic<int> list_move_callback_count{0};
    std::atomic_bool list_move_observed_signal{false};
    std::atomic_bool list_move_recorded_reentrant_query{false};
    QueryFuture      reentrant_move_future{};
    CommandList      destination_list(EQueueType::Graphics);
    destination_list.Signal(&replaced_list_fence, 31);
    QueryToken replaced_list_token = destination_list.BeginTimestampQuery("ReplacedList");
    destination_list.EndTimestampQuery(replaced_list_token);
    replaced_list_token.Then([&](const QueryResult& _result) {
        list_move_observed_signal.store(
            _result.status == QueryStatus::Error && replaced_list_fence.IsRejected(31),
            std::memory_order_release
        );
        destination_list.Signal(&reentrant_list_fence, 33);
        QueryToken reentrant = destination_list.BeginTimestampQuery("ReentrantMoveQuery");
        destination_list.EndTimestampQuery(reentrant);
        reentrant_move_future = reentrant.GetFuture();
        list_move_recorded_reentrant_query.store(true, std::memory_order_release);
        list_move_callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    CommandList incoming_list(EQueueType::Graphics);
    incoming_list.Signal(&incoming_list_fence, 32);
    destination_list = std::move(incoming_list);
    Expect(
        list_move_observed_signal.load(std::memory_order_acquire) &&
            list_move_recorded_reentrant_query.load(std::memory_order_acquire) &&
            list_move_callback_count.load(std::memory_order_relaxed) == 1,
        "CommandList move assignment lost ordering or reentrant recording"
    );
    {
        CmdSubmit preserved = destination_list.Submit();
        Expect(
            preserved.signal_events.size() == 2 && preserved.query_tokens.size() == 1,
            "CommandList move assignment overwrote reentrant Query/signal state"
        );
        preserved.RejectPendingSignals();
        preserved.RejectPendingQueries("test cleanup");
    }
    Expect(
        incoming_list_fence.IsRejected(32) && reentrant_list_fence.IsRejected(33) &&
            reentrant_move_future.Get().status == QueryStatus::Error,
        "CommandList move assignment did not retain its reentrant generation"
    );

    QueryFenceProbe  drained_list_fence{};
    QueryFenceProbe  reentrant_drain_fence{};
    std::atomic<int> drain_callback_count{0};
    std::atomic_bool drain_observed_signal{false};
    std::atomic_bool drain_recorded_reentrant_query{false};
    QueryFuture      reentrant_drain_future{};
    CommandList      drained_list(EQueueType::Graphics);
    drained_list.Signal(&drained_list_fence, 41);
    QueryToken drained_token = drained_list.BeginTimestampQuery("DrainedList");
    drained_list.EndTimestampQuery(drained_token);
    drained_token.Then([&](const QueryResult& _result) {
        drain_observed_signal.store(
            _result.status == QueryStatus::Error && drained_list_fence.IsRejected(41),
            std::memory_order_release
        );
        drained_list.Signal(&reentrant_drain_fence, 42);
        QueryToken reentrant = drained_list.BeginTimestampQuery("ReentrantDrainQuery");
        drained_list.EndTimestampQuery(reentrant);
        reentrant_drain_future = reentrant.GetFuture();
        drain_recorded_reentrant_query.store(true, std::memory_order_release);
        drain_callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    auto cleanup = drained_list.DrainOrdinaryCallbacksForRejection();
    for (auto& callback : cleanup) {
        callback();
    }
    Expect(
        drain_observed_signal.load(std::memory_order_acquire) &&
            drain_recorded_reentrant_query.load(std::memory_order_acquire) &&
            drain_callback_count.load(std::memory_order_relaxed) == 1,
        "rejection drain lost signal ordering or reentrant recording"
    );
    {
        CmdSubmit preserved = drained_list.Submit();
        Expect(
            preserved.signal_events.size() == 1 && preserved.query_tokens.size() == 1,
            "rejection drain cleared its reentrant Query/signal generation"
        );
        preserved.RejectPendingSignals();
        preserved.RejectPendingQueries("test cleanup");
    }
    Expect(
        reentrant_drain_fence.IsRejected(42) && reentrant_drain_future.Get().status == QueryStatus::Error,
        "rejection drain did not retain its reentrant generation"
    );
}

void MoveAndSubmitRetainIdentityAndLifetime() {
    QueryFuture moved_future{};
    CmdSubmit   moved_submit = CommandList(EQueueType::Graphics).Submit();
    {
        CommandList source(EQueueType::Graphics);
        QueryToken  token = source.BeginTimestampQuery("MovedList");
        moved_future      = token.GetFuture();

        CommandList destination(std::move(source));
        destination.EndTimestampQuery(token);
        ExpectThrows<std::invalid_argument>(
            [&] {
                source.EndTimestampQuery(token);
            },
            "moved-from CommandList retained query ownership"
        );

        CmdSubmit sealed = destination.Submit();
        Expect(sealed.query_tokens.size() == 1, "Submit lost its strong QueryToken owner");
        moved_submit = std::move(sealed);
        Expect(
            moved_submit.query_tokens.size() == 1 && sealed.query_tokens.empty(),
            "CmdSubmit move duplicated or dropped query ownership"
        );
    }
    Expect(
        moved_future.Status() == QueryStatus::Pending,
        "CommandList/token destruction outlived the moved CmdSubmit owner"
    );
    Expect(
        QueryBackendAccess::ResolveTimestamp(moved_submit.query_tokens.front(), TestTimestampResult()),
        "moved CmdSubmit token could not be resolved"
    );
    for (auto& callback : moved_submit.callbacks) {
        callback();
    }
    moved_submit.callbacks.clear();
    moved_submit.query_tokens.clear();
    Expect(moved_future.Get().status == QueryStatus::Ready, "moved query did not retain the Ready result");

    QueryFuture overwritten_future{};
    CmdSubmit   destination = CommandList(EQueueType::Graphics).Submit();
    {
        CommandList list(EQueueType::Graphics);
        QueryToken  token = list.BeginTimestampQuery("OverwrittenSubmit");
        list.EndTimestampQuery(token);
        overwritten_future = token.GetFuture();
        destination        = list.Submit();
    }
    destination = CommandList(EQueueType::Graphics).Submit();
    Expect(
        overwritten_future.Get().status == QueryStatus::Error,
        "CmdSubmit move assignment left replaced query ownership Pending"
    );
}

void CancellationViewIsStableAndGenerationScoped() {
    CommandList sealed_list(EQueueType::Graphics);
    QueryCancellationView sealed_view =
        sealed_list.GetQueryCancellationView();
    QueryToken sealed_token =
        sealed_list.BeginTimestampQuery("SealedGenerationTimestamp");
    sealed_list.EndTimestampQuery(sealed_token);
    QueryFuture sealed_future = sealed_token.GetFuture();
    CmdSubmit sealed_submit = sealed_list.Submit();
    Expect(
        !sealed_view.Cancel("late stale-view cancellation"),
        "a stale cancellation view mutated a submitted generation"
    );
    Expect(
        sealed_future.Status() == QueryStatus::Pending,
        "stale cancellation changed the submitted query result"
    );
    Expect(
        QueryBackendAccess::ResolveTimestamp(
            sealed_submit.query_tokens.front(), TestTimestampResult()
        ),
        "submitted generation could not resolve after stale cancellation lost"
    );
    Expect(
        sealed_future.Get().status == QueryStatus::Ready,
        "submitted generation lost its Ready result after stale cancellation"
    );

    CommandList           list(EQueueType::Graphics);
    QueryCancellationView first_generation = list.GetQueryCancellationView();
    Expect(first_generation.Valid(), "CommandList exposed an invalid cancellation view");

    QueryToken existing = list.BeginTimestampQuery("BeforeCancel");
    list.EndTimestampQuery(existing);
    Expect(
        first_generation.Cancel("recording source shutdown"),
        "first cancellation did not win the generation transition"
    );
    Expect(!first_generation.Cancel("duplicate shutdown"), "cancellation domain was not one-shot");
    Expect(
        existing.GetFuture().Get().status == QueryStatus::Error,
        "cancellation did not terminate an existing token"
    );

    QueryToken late = list.BeginTimestampQuery("AfterCancel");
    Expect(
        late.GetFuture().Status() == QueryStatus::Error,
        "registration after cancellation did not fail immediately"
    );
    list.EndTimestampQuery(late);
    CmdSubmit cancelled_submit = list.Submit();

    QueryCancellationView second_generation = list.GetQueryCancellationView();
    Expect(
        second_generation.Valid() && !second_generation.IsCancelled(),
        "Submit did not create an independent cancellation generation"
    );
    Expect(first_generation.IsCancelled(), "old cancellation view changed identity after Submit");

    QueryToken next = list.BeginTimestampQuery("NextGeneration");
    list.EndTimestampQuery(next);
    Expect(
        next.GetFuture().Status() == QueryStatus::Pending, "old cancellation leaked into the next generation"
    );
    Expect(
        second_generation.Cancel("next generation shutdown"),
        "new cancellation view could not cancel its own generation"
    );
    Expect(
        next.GetFuture().Get().status == QueryStatus::Error,
        "new generation cancellation left its token Pending"
    );
}

} // namespace

int main() {
    static_assert(std::is_copy_constructible_v<QueryFuture>);
    static_assert(std::is_copy_constructible_v<QueryToken>);
    static_assert(std::is_nothrow_copy_constructible_v<QueryToken>);
    static_assert(!std::is_copy_constructible_v<QueryCancellationDomain>);
    static_assert(std::is_copy_constructible_v<QueryCancellationView>);

    try {
        FutureIsThreadSafeOneShotAndContainsCallbackExceptions();
        ReadyAndErrorResolutionRaceHasOneWinner();
        ReadyBatchPublishesEveryPeerBeforeNotification();
        BulkErrorPathsPublishEveryPeerBeforeNotification();
        LosingBatchCannotStealNotificationOwnership();
        PublishWakesWaitersBeforeDeferredCallbackNotification();
        CallbacksMayReleaseTheirLastQueryHandles();
        PairingIsSameListSameKindAndStrictLifo();
        InvalidSubmissionRejectionAndDestructionAreTerminal();
        SignalRejectionPrecedesQueryCallbacksAndPreservesReentrantState();
        MoveAndSubmitRetainIdentityAndLifetime();
        CancellationViewIsStableAndGenerationScoped();
    } catch (const std::exception& exception) {
        std::cerr << "RHIQueryContract: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "RHIQueryContract: all checks passed\n";
    return 0;
}
