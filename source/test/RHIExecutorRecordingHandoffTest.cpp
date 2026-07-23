#include "rhi/RHIExecutor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
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

bool InlineReadyWorkCannotBeOvertaken() {
    RHIExecutorRecordingHandoffQueue queue{};
    queue.Start();

    ResolutionLog log{};
    std::mutex block_mutex;
    std::condition_variable block_cv;
    bool inline_entered = false;
    bool release_inline = false;

    std::jthread inline_thread([&] {
        queue.RouteReady(RHIExecutorRecordingHandoffWork{
            .prerequisites = {},
            .resolve = [&](ERHIRecordingHandoffResult _result) {
                {
                    std::unique_lock lock(block_mutex);
                    inline_entered = true;
                    block_cv.notify_all();
                    block_cv.wait(lock, [&] { return release_inline; });
                }
                log.Push(1, _result);
            },
        });
    });

    {
        std::unique_lock lock(block_mutex);
        if (!block_cv.wait_for(lock, 2s, [&] { return inline_entered; })) {
            release_inline = true;
            block_cv.notify_all();
            queue.ShutDown();
            return Expect(false, "inline ready work did not start");
        }
    }

    auto completed_gate = RHIRecordingGate::Create(true);
    queue.EnqueueRecording(RHIExecutorRecordingHandoffWork{
        .prerequisites = {completed_gate},
        .resolve = [&log](ERHIRecordingHandoffResult _result) {
            log.Push(2, _result);
        },
    });
    std::this_thread::sleep_for(30ms);
    if (!Expect(log.Snapshot().empty(), "recording work overtook active ready work")) {
        {
            std::lock_guard lock(block_mutex);
            release_inline = true;
        }
        block_cv.notify_all();
        queue.ShutDown();
        return false;
    }

    {
        std::lock_guard lock(block_mutex);
        release_inline = true;
    }
    block_cv.notify_all();
    inline_thread.join();

    const bool completed = log.WaitForSize(2);
    const auto entries   = log.Snapshot();
    queue.ShutDown();
    return Expect(completed, "queued recording work did not run") &&
           Expect(entries.size() == 2, "unexpected inline-order result count") &&
           Expect(entries[0].first == 1 && entries[1].first == 2, "inline order changed");
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
        );

    pending_gate->Signal();
    return okay;
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

} // namespace

int main() {
    if (!PerSourceGatesPreserveFifo() || !InlineReadyWorkCannotBeOvertaken() ||
        !FailedGateRejectsOnlyItsGroupAndPreservesFifo() ||
        !FailedRecordingDrainsCleanupExactlyOnce() ||
        !BlockingWaitReportsTerminalStatus() ||
        !ShutdownDrainsTerminalSourceBehindPendingHead() ||
        !ShutdownCancelsAnUnsignalledGate()) {
        return EXIT_FAILURE;
    }
    std::cout << "RHIExecutor recording handoff contract passed\n";
    return EXIT_SUCCESS;
}
