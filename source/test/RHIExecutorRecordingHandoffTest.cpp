#include "rhi/RHIExecutor.h"
#include "rhi/RHIThreadOwnership.h"
#include "renderer/raytracing/RaytracingExportSubmission.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
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

bool HasNoRejectedGenerationPayload(const CmdSubmit& _submit) {
    return _submit.cmds.empty() && _submit.callbacks.empty() &&
           _submit.success_callbacks.empty() &&
           _submit.gpu_completion_tokens.empty() &&
           !_submit.gpu_completion_publish_batch.Valid() &&
           _submit.query_tokens.empty() &&
           !_submit.query_publish_batch.Valid() &&
           _submit.cached_args.empty() &&
           _submit.segments.empty() && _submit.wait_events.empty() &&
           _submit.signal_events.empty() &&
           _submit.signal_rejection_keepalives.empty() &&
           !_submit.b_sync &&
           !_submit.b_tick_profiling &&
           _submit.profiling_phase == ERHIProfilingPhase::Disabled &&
           !_submit.b_delete_resources &&
           _submit.resource_state_ownership ==
               ERHIResourceStateOwnership::BackendTracked &&
           _submit.translate_execution_class ==
               ERHITranslateExecutionClass::Parallel &&
           _submit.async_queue_scope == 0 && _submit.debug_label.empty();
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

struct DirectMaterializationFaultContext {
    size_t              fail_source{1};
    std::atomic<size_t> visited_sources{0};
};

void InjectDirectMaterializationFailure(
    void*  _context,
    size_t _source_index
) {
    auto& context =
        *static_cast<DirectMaterializationFaultContext*>(_context);
    context.visited_sources.fetch_add(1, std::memory_order_release);
    if (_source_index == context.fail_source) {
        throw std::runtime_error(
            "injected direct CommandList materialization failure"
        );
    }
}

struct BlockingDirectMaterializationContext {
    std::mutex              mutex{};
    std::condition_variable cv{};
    bool                    entered{false};
    bool                    release{false};
};

void BlockDirectMaterializationUntilReleased(
    void*  _context,
    size_t
) {
    auto& context =
        *static_cast<BlockingDirectMaterializationContext*>(_context);
    std::unique_lock lock(context.mutex);
    context.entered = true;
    context.cv.notify_all();
    context.cv.wait(lock, [&] { return context.release; });
    throw std::runtime_error(
        "injected blocking direct materialization failure"
    );
}

class ReentrantDestructorCommand final : public Command {
public:
    explicit ReentrantDestructorCommand(
        std::function<void()> _on_destroy,
        EQueueType            _queue = EQueueType::Graphics
    ) :
        Command(EType::Custom),
        on_destroy(std::move(_on_destroy)),
        queue(_queue) {}

    ~ReentrantDestructorCommand() override {
        if (!on_destroy) {
            return;
        }
        try {
            on_destroy();
        } catch (...) {
        }
    }

    EQueueType GetQueueType() const override {
        return queue;
    }

private:
    std::function<void()> on_destroy{};
    EQueueType            queue{EQueueType::Graphics};
};

struct FinalReleaseHook {
    std::function<void()> callback{};
    std::atomic_bool*     threw{nullptr};

    ~FinalReleaseHook() noexcept {
        if (!callback) {
            return;
        }
        try {
            callback();
        } catch (...) {
            if (threw != nullptr) {
                threw->store(true, std::memory_order_release);
            }
        }
    }
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
    owned_submit.signal_rejection_keepalives.clear();
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

bool CommandListFenceReentryCannotExtractRejectedGeneration() {
    CommandList command_list(EQueueType::Graphics);
    std::atomic<int>  signal_callbacks{0};
    std::atomic<int>  ordinary_callbacks{0};
    std::atomic<int>  success_callbacks{0};
    std::atomic<bool> reentrant_submit_threw{false};
    std::atomic<bool> rejected_payload_escaped{false};

    RecordingFenceProbe signal(
        {},
        [&] {
            signal_callbacks.fetch_add(1, std::memory_order_release);
            try {
                CmdSubmit reentrant = command_list.Submit();
                rejected_payload_escaped.store(
                    !HasNoRejectedGenerationPayload(reentrant),
                    std::memory_order_release
                );

                // Keep the regression bounded even when run against an older
                // implementation that exposes the rejected signal/callbacks.
                // Their presence is recorded above; do not let destruction
                // recursively reject the same raw Fence.
                reentrant.cmds.clear();
                reentrant.callbacks.clear();
                reentrant.success_callbacks.clear();
                reentrant.gpu_completion_tokens.clear();
                reentrant.query_tokens.clear();
                reentrant.cached_args.clear();
                reentrant.segments.clear();
                reentrant.wait_events.clear();
                reentrant.signal_events.clear();
            } catch (...) {
                reentrant_submit_threw.store(
                    true, std::memory_order_release
                );
            }
        }
    );

    command_list.Signal(&signal, 19);
    command_list.AddCallback([&] {
        ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    command_list.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_release);
    });

    Moer::Array<std::function<void()>> cleanup =
        command_list.DrainOrdinaryCallbacksForRejection();
    for (std::function<void()>& callback : cleanup) {
        if (callback) {
            callback();
        }
    }

    return Expect(
               signal_callbacks.load(std::memory_order_acquire) == 1 &&
                   signal.IsRejected(19),
               "CommandList rejection did not invoke its Fence observer exactly once"
           ) &&
           Expect(
               !reentrant_submit_threw.load(std::memory_order_acquire),
               "Fence rejection could not re-enter Submit on the replacement generation"
           ) &&
           Expect(
               !rejected_payload_escaped.load(std::memory_order_acquire),
               "Fence rejection re-entry extracted the rejected generation"
           ) &&
           Expect(
               ordinary_callbacks.load(std::memory_order_acquire) == 1,
               "Fence rejection did not return ordinary cleanup exactly once"
           ) &&
           Expect(
               success_callbacks.load(std::memory_order_acquire) == 0,
               "Fence rejection invoked a success-only callback"
           ) &&
           Expect(
               command_list.IsEmpty(),
               "Fence rejection left payload in the replacement generation"
           );
}

bool FenceRejectionObservesPublishedHostsAndRetainsOwnedSignals() {
    CommandList wait_list(EQueueType::Graphics);
    const GpuCompletionFuture completion =
        wait_list.TrackGpuCompletion("FenceRejectWaitCompletion");
    QueryToken query =
        wait_list.BeginTimestampQuery("FenceRejectWaitQuery");
    wait_list.EndTimestampQuery(query);
    const QueryFuture query_future = query.GetFuture();
    std::atomic_bool completion_wait_satisfied{false};
    std::atomic_bool query_wait_satisfied{false};
    RecordingFenceProbe waiting_signal(
        {},
        [&] {
            completion_wait_satisfied.store(
                completion.WaitFor(100ms),
                std::memory_order_release
            );
            query_wait_satisfied.store(
                query_future.WaitFor(100ms),
                std::memory_order_release
            );
        }
    );
    wait_list.Signal(&waiting_signal, 29);
    auto cleanup = wait_list.DrainOrdinaryCallbacksForRejection();
    cleanup.clear();

    CommandList lifetime_list(EQueueType::Graphics);
    auto first_destroyed =
        std::make_shared<std::atomic_bool>(false);
    auto second_destroyed =
        std::make_shared<std::atomic_bool>(false);
    std::atomic_int first_rejections{0};
    std::atomic_int second_rejections{0};
    FenceRef first{
        MoerNew(RecordingFenceProbe)(
            first_destroyed,
            [&] {
                first_rejections.fetch_add(
                    1, std::memory_order_release
                );
                CmdSubmit reentrant = lifetime_list.Submit();
                // Destroy the original callback-based keepalives while the
                // outer rejection loop still has a second raw Fence pointer.
                reentrant.callbacks.clear();
                reentrant.signal_rejection_keepalives.clear();
            }
        )
    };
    FenceRef second{
        MoerNew(RecordingFenceProbe)(
            second_destroyed,
            [&] {
                second_rejections.fetch_add(
                    1, std::memory_order_release
                );
            }
        )
    };
    lifetime_list.Signal(first, 31);
    lifetime_list.Signal(second, 32);
    first = {};
    second = {};
    lifetime_list.RejectPendingSignals();

    return Expect(
               completion_wait_satisfied.load(
                   std::memory_order_acquire
               ) &&
                   query_wait_satisfied.load(
                       std::memory_order_acquire
                   ),
               "Fence::Reject observed Pending host state"
           ) &&
           Expect(
               first_rejections.load(std::memory_order_acquire) == 1 &&
                   second_rejections.load(
                       std::memory_order_acquire
                   ) == 1,
               "re-entrant signal rejection skipped an owned Fence"
           ) &&
           Expect(
               first_destroyed->load(std::memory_order_acquire) &&
                   second_destroyed->load(std::memory_order_acquire),
               "rejection keepalive outlived the rejected signal batch"
           );
}

bool ExportReceiptSignalsOwnDedicatedRejectionKeepalives() {
    using Moer::Render::Raytracing::ExportSubmissionTransaction;

    auto run_case = [](bool _readback, uint64_t _value) {
        auto destroyed =
            std::make_shared<std::atomic_bool>(false);
        auto rejected_value =
            std::make_shared<std::atomic<uint64_t>>(0);
        FenceRef receipt{
            MoerNew(RecordingFenceProbe)(
                destroyed,
                [rejected_value, _value] {
                    rejected_value->store(
                        _value, std::memory_order_release
                    );
                }
            )
        };
        Fence* const receipt_identity = receipt.Get();

        CommandList commands(EQueueType::Graphics);
        CmdSubmit   submit = commands.Submit();
        FenceRef    source_receipt{};
        {
            if (_readback) {
                source_receipt = FenceRef{
                    MoerNew(RecordingFenceProbe)()
                };
                ExportSubmissionTransaction transaction(
                    source_receipt, _value + 1
                );
                transaction.BeginReadback(receipt, _value);
                transaction.AttachReadbackSignal(submit);
            } else {
                ExportSubmissionTransaction transaction(
                    receipt, _value
                );
                transaction.AttachSignal(submit);
            }

            if (!Expect(
                        submit.signal_events.size() == 1 &&
                        submit.signal_events.front().timeline_handle ==
                            reinterpret_cast<std::uintptr_t>(
                                receipt_identity
                            ) &&
                        submit.signal_events.front().value == _value &&
                        submit.signal_rejection_keepalives.size() == 1 &&
                        submit.signal_rejection_keepalives.front().Get() ==
                            receipt_identity,
                    _readback ?
                        "readback receipt signal did not install its dedicated rejection keepalive" :
                        "export receipt signal did not install its dedicated rejection keepalive"
                )) {
                // Keep a failed structural assertion bounded when run against
                // an older implementation that owns the receipt only through
                // the ordinary callback tier.
                submit.signal_events.clear();
                submit.callbacks.clear();
                submit.signal_rejection_keepalives.clear();
                return false;
            }
        }

        receipt       = {};
        source_receipt = {};
        // Simulate re-entrant code destroying the historical callback-based
        // keeper while RejectPendingSignals is still responsible for the raw
        // backend identity.
        submit.callbacks.clear();
        if (!Expect(
                !destroyed->load(std::memory_order_acquire),
                _readback ?
                    "readback receipt died before rejection terminalization" :
                    "export receipt died before rejection terminalization"
            )) {
            submit.signal_events.clear();
            submit.signal_rejection_keepalives.clear();
            return false;
        }

        submit.RejectPendingSignals();
        return Expect(
                   rejected_value->load(std::memory_order_acquire) ==
                       _value,
                   _readback ?
                       "readback receipt was not rejected at its exact value" :
                       "export receipt was not rejected at its exact value"
               ) &&
               Expect(
                   destroyed->load(std::memory_order_acquire),
                   _readback ?
                       "readback receipt keepalive outlived its rejected signal" :
                       "export receipt keepalive outlived its rejected signal"
               );
    };

    return run_case(false, 41) && run_case(true, 43);
}

bool CommandDestructorReentryTargetsOnlyFreshGeneration() {
    CommandList command_list(EQueueType::Graphics);
    RecordingFenceProbe signal{};
    std::atomic<int> command_destructors{0};
    std::atomic<int> completion_callbacks{0};
    std::atomic<int> query_callbacks{0};
    std::atomic<int> ordinary_callbacks{0};
    std::atomic<int> success_callbacks{0};
    std::atomic<int> fresh_callbacks{0};
    std::atomic<bool> destructor_observed_terminal_batch{false};

    command_list.Signal(&signal, 23);
    const GpuCompletionFuture completion =
        command_list.TrackGpuCompletion(
            "CommandDestructorRejectedCompletion"
        );
    QueryToken query = command_list.BeginTimestampQuery(
        "CommandDestructorRejectedQuery"
    );
    command_list.EndTimestampQuery(query);
    const QueryFuture query_future = query.GetFuture();
    completion.Then([&](const GpuCompletionResult&) {
        completion_callbacks.fetch_add(1, std::memory_order_release);
    });
    query_future.Then([&](const QueryResult&) {
        query_callbacks.fetch_add(1, std::memory_order_release);
    });
    command_list.AddCallback([&] {
        ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    command_list.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_release);
    });
    command_list.AddCustomCommand(
        Moer::MakeUnique<ReentrantDestructorCommand>([&] {
            command_destructors.fetch_add(
                1, std::memory_order_release
            );
            destructor_observed_terminal_batch.store(
                signal.IsRejected(23) &&
                    completion.Status() ==
                        GpuCompletionStatus::Error &&
                    query_future.Status() == QueryStatus::Error &&
                    completion_callbacks.load(
                        std::memory_order_acquire
                    ) == 1 &&
                    query_callbacks.load(
                        std::memory_order_acquire
                    ) == 1,
                std::memory_order_release
            );
            command_list.AddCallback([&] {
                fresh_callbacks.fetch_add(
                    1, std::memory_order_release
                );
            });
        }),
        "RejectedDestructorReentry"
    );

    Moer::Array<std::function<void()>> cleanup =
        command_list.DrainOrdinaryCallbacksForRejection();
    for (std::function<void()>& callback : cleanup) {
        if (callback) {
            callback();
        }
    }
    CmdSubmit fresh = command_list.Submit();
    const bool fresh_generation_isolated =
        fresh.cmds.empty() && fresh.callbacks.size() == 1 &&
        fresh.success_callbacks.empty() &&
        fresh.gpu_completion_tokens.empty() &&
        fresh.query_tokens.empty() && fresh.signal_events.empty();
    for (std::function<void()>& callback : fresh.callbacks) {
        if (callback) {
            callback();
        }
    }
    fresh.callbacks.clear();

    return Expect(
               command_destructors.load(std::memory_order_acquire) == 1,
               "rejected custom Command was not destroyed exactly once"
           ) &&
           Expect(
               destructor_observed_terminal_batch.load(
                   std::memory_order_acquire
               ),
               "custom Command destructor ran before the rejected terminal batch was fully notified"
           ) &&
           Expect(
               ordinary_callbacks.load(std::memory_order_acquire) == 1 &&
                   success_callbacks.load(std::memory_order_acquire) == 0,
               "custom Command rejection violated ordinary/success callback semantics"
           ) &&
           Expect(
               fresh_generation_isolated &&
                   fresh_callbacks.load(std::memory_order_acquire) == 1,
               "custom Command destructor re-entry was lost or mixed with the rejected generation"
           );
}

bool CmdSubmitCallbackCaptureDestructorSeesCompleteReplacement() {
    RecordingFenceProbe incoming_signal{};
    std::optional<CmdSubmit> observed{};
    std::atomic_bool hook_threw{false};
    std::atomic<int> hook_calls{0};

    CommandList empty_destination(EQueueType::Graphics);
    CmdSubmit destination = empty_destination.Submit();
    auto hook = std::make_shared<FinalReleaseHook>();
    hook->threw = &hook_threw;
    hook->callback = [&] {
        hook_calls.fetch_add(1, std::memory_order_release);
        observed.emplace(std::move(destination));
    };
    destination.callbacks.emplace_back([hook] {});
    hook.reset();

    CommandList incoming_commands(EQueueType::Graphics);
    incoming_commands.AddCustomCommand(
        Moer::MakeUnique<ReentrantDestructorCommand>(
            std::function<void()>{}
        ),
        "CmdSubmitReplacementIncoming"
    );
    incoming_commands.AddCallback([] {});
    incoming_commands.AddSuccessCallback([] {});
    const GpuCompletionFuture completion =
        incoming_commands.TrackGpuCompletion(
            "CmdSubmitReplacementCompletion"
        );
    QueryToken query = incoming_commands.BeginTimestampQuery(
        "CmdSubmitReplacementQuery"
    );
    incoming_commands.EndTimestampQuery(query);
    const QueryFuture query_future = query.GetFuture();
    incoming_commands.Signal(&incoming_signal, 29);
    CmdSubmit incoming = incoming_commands.Submit();
    incoming.b_sync = true;
    incoming.b_delete_resources = true;
    incoming.SetProfilingPhase(ERHIProfilingPhase::Begin);
    incoming.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    incoming.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::SerialControl
    );
    incoming.async_queue_scope = 0xA5;
    incoming.DebugLabel("CmdSubmitReplacementLabel");
    const size_t expected_commands = incoming.cmds.size();
    const size_t expected_callbacks = incoming.callbacks.size();
    const size_t expected_success_callbacks =
        incoming.success_callbacks.size();
    const size_t expected_completion_tokens =
        incoming.gpu_completion_tokens.size();
    const size_t expected_query_tokens =
        incoming.query_tokens.size();
    const size_t expected_signals = incoming.signal_events.size();

    destination = std::move(incoming);

    const bool saw_complete_incoming =
        observed.has_value() &&
        observed->cmds.size() == expected_commands &&
        observed->callbacks.size() == expected_callbacks &&
        observed->success_callbacks.size() ==
            expected_success_callbacks &&
        observed->gpu_completion_tokens.size() ==
            expected_completion_tokens &&
        observed->query_tokens.size() == expected_query_tokens &&
        observed->signal_events.size() == expected_signals &&
        observed->signal_events.front().value == 29 &&
        observed->b_sync && observed->b_delete_resources &&
        observed->ProfilingPhase() == ERHIProfilingPhase::Begin &&
        observed->resource_state_ownership ==
            ERHIResourceStateOwnership::Explicit &&
        observed->translate_execution_class ==
            ERHITranslateExecutionClass::SerialControl &&
        observed->async_queue_scope == 0xA5 &&
        observed->debug_label == "CmdSubmitReplacementLabel";
    const bool destination_payload_was_consumed =
        destination.cmds.empty() && destination.callbacks.empty() &&
        destination.success_callbacks.empty() &&
        destination.gpu_completion_tokens.empty() &&
        !destination.gpu_completion_publish_batch.Valid() &&
        destination.query_tokens.empty() &&
        !destination.query_publish_batch.Valid() &&
        destination.cached_args.empty() &&
        destination.segments.empty() &&
        destination.wait_events.empty() &&
        destination.signal_events.empty() &&
        destination.debug_label.empty();

    observed.reset();
    return Expect(
               !hook_threw.load(std::memory_order_acquire) &&
                   hook_calls.load(std::memory_order_acquire) == 1,
               "CmdSubmit callback capture destructor did not re-enter exactly once"
           ) &&
           Expect(
               saw_complete_incoming &&
                   destination_payload_was_consumed,
               "CmdSubmit replacement exposed a partially installed incoming payload"
           ) &&
           Expect(
               incoming_signal.IsRejected(29) &&
                   completion.Status() ==
                       GpuCompletionStatus::Error &&
                   query_future.Status() == QueryStatus::Error,
               "observed replacement payload did not terminalize on destruction"
           );
}

bool CommandListCallbackCaptureDestructorSeesCompleteReplacement() {
    RecordingFenceProbe incoming_signal{};
    std::optional<CmdSubmit> observed{};
    std::atomic_bool hook_threw{false};
    std::atomic<int> hook_calls{0};

    CommandList destination(EQueueType::Graphics);
    auto hook = std::make_shared<FinalReleaseHook>();
    hook->threw = &hook_threw;
    hook->callback = [&] {
        hook_calls.fetch_add(1, std::memory_order_release);
        observed.emplace(destination.Submit());
    };
    destination.AddCallback([hook] {});
    hook.reset();

    CommandList incoming(EQueueType::Graphics);
    incoming.AddCustomCommand(
        Moer::MakeUnique<ReentrantDestructorCommand>(
            std::function<void()>{}
        ),
        "CommandListReplacementIncoming"
    );
    incoming.AddCallback([] {});
    incoming.AddSuccessCallback([] {});
    const GpuCompletionFuture completion =
        incoming.TrackGpuCompletion(
            "CommandListReplacementCompletion"
        );
    QueryToken query = incoming.BeginTimestampQuery(
        "CommandListReplacementQuery"
    );
    incoming.EndTimestampQuery(query);
    const QueryFuture query_future = query.GetFuture();
    incoming.Signal(&incoming_signal, 31);
    incoming.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    incoming.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::SerialControl
    );

    destination = std::move(incoming);

    const bool saw_complete_incoming =
        observed.has_value() && observed->cmds.size() == 3 &&
        observed->callbacks.size() == 2 &&
        observed->success_callbacks.size() == 1 &&
        observed->gpu_completion_tokens.size() == 1 &&
        observed->query_tokens.size() == 1 &&
        observed->signal_events.size() == 1 &&
        observed->signal_events.front().value == 31 &&
        observed->resource_state_ownership ==
            ERHIResourceStateOwnership::Explicit &&
        observed->translate_execution_class ==
            ERHITranslateExecutionClass::SerialControl;
    const bool destination_was_consumed = destination.IsEmpty();

    observed.reset();
    return Expect(
               !hook_threw.load(std::memory_order_acquire) &&
                   hook_calls.load(std::memory_order_acquire) == 1,
               "CommandList callback capture destructor did not re-enter exactly once"
           ) &&
           Expect(
               saw_complete_incoming && destination_was_consumed,
               "CommandList replacement exposed a mixed or partial generation"
           ) &&
           Expect(
               incoming_signal.IsRejected(31) &&
                   completion.Status() ==
                       GpuCompletionStatus::Error &&
                   query_future.Status() == QueryStatus::Error,
               "observed CommandList replacement did not terminalize on destruction"
           );
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
    std::optional<CmdSubmit> suffix_reentrant_submit{};
    std::atomic<bool> suffix_reentrant_submit_threw{false};
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
                try {
                    suffix_reentrant_submit.emplace(
                        command_lists[2].Submit()
                    );
                } catch (...) {
                    suffix_reentrant_submit_threw.store(
                        true, std::memory_order_release
                    );
                }
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
    okay &= Expect(
        !suffix_reentrant_submit_threw.load(std::memory_order_acquire) &&
            suffix_reentrant_submit.has_value(),
        "direct CommandList preflight callback could not re-enter the suffix"
    );
    if (suffix_reentrant_submit.has_value()) {
        okay &= Expect(
            HasNoRejectedGenerationPayload(*suffix_reentrant_submit),
            "direct CommandList preflight callback extracted the rejected suffix generation"
        );
    }
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

bool DirectCommandListGroupActiveLeaseFailsWithoutMutation() {
    std::array<RecordingFenceProbe, 3> signals{};
    Moer::Array<CommandList> command_lists{};
    command_lists.reserve(3);
    for (size_t index = 0; index < 3; ++index) {
        command_lists.emplace_back(EQueueType::Graphics);
    }

    std::array<QueryFuture, 3> futures{};
    std::array<std::atomic<int>, 3> query_callbacks{};
    std::array<std::atomic<int>, 3> ordinary_callbacks{};
    std::array<std::atomic<int>, 3> success_callbacks{};
    for (size_t index = 0; index < command_lists.size(); ++index) {
        command_lists[index].Signal(&signals[index], 50 + index);
        command_lists[index].AddCallback([&, index] {
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
            "DirectSubmitActiveLeaseTimestamp"
        );
        command_lists[index].EndTimestampQuery(token);
        futures[index] = token.GetFuture();
        futures[index].Then([&, index](const QueryResult&) {
            query_callbacks[index].fetch_add(
                1, std::memory_order_release
            );
        });
    }

    PresentReceiptRef receipt = std::make_shared<PresentReceipt>();
    RHIPresentRequest present{};
    present.receipt = receipt;
    auto blocking_lease =
        command_lists[1].AcquireManagedRecordingLease();
    bool threw_logic_error = false;
    try {
        RHIExecutor::Get().Submit(
            std::move(command_lists),
            ERHIExecSubmitFlags::None,
            &present
        );
    } catch (const std::logic_error&) {
        threw_logic_error = true;
    } catch (...) {
    }

    bool okay = Expect(
        threw_logic_error,
        "direct CommandList group accepted an active managed recording lease"
    );
    for (size_t index = 0; index < command_lists.size(); ++index) {
        okay &= Expect(
            !signals[index].IsRejected(50 + index) &&
                futures[index].Status() == QueryStatus::Pending &&
                query_callbacks[index].load(std::memory_order_acquire) == 0 &&
                ordinary_callbacks[index].load(std::memory_order_acquire) == 0 &&
                success_callbacks[index].load(std::memory_order_acquire) == 0 &&
                !command_lists[index].IsEmpty(),
            "active-lease preflight mutated or notified the intact transaction"
        );
    }
    okay &= Expect(
        !receipt->WaitForSubmission(0ms).resolved,
        "active-lease preflight resolved Present for an unconsumed transaction"
    );

    blocking_lease.Release();
    for (CommandList& command_list : command_lists) {
        auto callbacks =
            command_list.DrainOrdinaryCallbacksForRejection();
        for (auto& callback : callbacks) {
            if (callback) {
                callback();
            }
        }
    }
    for (size_t index = 0; index < command_lists.size(); ++index) {
        okay &= Expect(
            signals[index].IsRejected(50 + index) &&
                futures[index].WaitFor(100ms) &&
                futures[index].Status() == QueryStatus::Error &&
                query_callbacks[index].load(std::memory_order_acquire) == 1 &&
                ordinary_callbacks[index].load(std::memory_order_acquire) == 1 &&
                success_callbacks[index].load(std::memory_order_acquire) == 0,
            "caller-owned cleanup did not terminalize the intact active-lease transaction"
        );
    }
    (void)receipt->Resolve(false);
    return okay;
}

bool DirectCommandListGroupPartialMaterializationFailureIsAtomic() {
    constexpr size_t SourceCount = 2;
    Moer::Array<CommandList> command_lists{};
    command_lists.reserve(SourceCount);
    for (size_t index = 0; index < SourceCount; ++index) {
        command_lists.emplace_back(EQueueType::Graphics);
    }

    std::array<FenceRef, SourceCount> signals{};
    std::array<RecordingFenceProbe*, SourceCount> signal_probes{};
    std::array<std::atomic<int>, SourceCount> signal_rejections{};
    std::array<GpuCompletionFuture, SourceCount> completion_futures{};
    std::array<QueryFuture, SourceCount> query_futures{};
    std::array<std::atomic<int>, SourceCount> completion_callbacks{};
    std::array<std::atomic<int>, SourceCount> query_callbacks{};
    std::array<std::atomic<int>, SourceCount> ordinary_callbacks{};
    std::array<std::atomic<int>, SourceCount> success_callbacks{};
    std::array<std::atomic<bool>, SourceCount>
        signal_observed_host_publication{};
    std::array<std::atomic<bool>, SourceCount>
        ordinary_observed_notifications{};
    std::atomic<bool> first_completion_observed_atomic_group{false};
    std::array<std::optional<CmdSubmit>, SourceCount> reentrant_submits{};
    std::atomic<bool> reentrant_submit_threw{false};

    for (size_t index = 0; index < SourceCount; ++index) {
        signals[index] = MoerNew(RecordingFenceProbe)(
            std::shared_ptr<std::atomic_bool>{},
            [&, index] {
                bool host_published_without_notification = true;
                for (size_t sibling = 0;
                     sibling < SourceCount;
                     ++sibling) {
                    host_published_without_notification &=
                        completion_futures[sibling].Status() ==
                            GpuCompletionStatus::Error &&
                        query_futures[sibling].Status() ==
                            QueryStatus::Error &&
                        completion_callbacks[sibling].load(
                            std::memory_order_acquire
                        ) == 0 &&
                        query_callbacks[sibling].load(
                            std::memory_order_acquire
                        ) == 0 &&
                        ordinary_callbacks[sibling].load(
                            std::memory_order_acquire
                        ) == 0;
                }
                signal_observed_host_publication[index].store(
                    host_published_without_notification,
                    std::memory_order_release
                );
                signal_rejections[index].fetch_add(
                    1, std::memory_order_release
                );
                if (index != 0) {
                    return;
                }
                for (size_t source_index = 0;
                     source_index < SourceCount;
                     ++source_index) {
                    try {
                        reentrant_submits[source_index].emplace(
                            command_lists[source_index].Submit()
                        );
                    } catch (...) {
                        reentrant_submit_threw.store(
                            true, std::memory_order_release
                        );
                    }
                }
            }
        );
        signal_probes[index] =
            static_cast<RecordingFenceProbe*>(signals[index].Get());
        command_lists[index].Signal(signals[index], 70 + index);

        completion_futures[index] =
            command_lists[index].TrackGpuCompletion(
                "DirectPartialMaterializationCompletion"
            );
        completion_futures[index].Then(
            [&, index](const GpuCompletionResult& _result) {
                if (_result.status == GpuCompletionStatus::Error) {
                    completion_callbacks[index].fetch_add(
                        1, std::memory_order_release
                    );
                }
                if (index != 0) {
                    return;
                }
                const bool group_terminal =
                    completion_futures[1].WaitFor(0ms) &&
                    completion_futures[1].Status() ==
                        GpuCompletionStatus::Error &&
                    query_futures[0].WaitFor(0ms) &&
                    query_futures[1].WaitFor(0ms) &&
                    query_futures[0].Status() == QueryStatus::Error &&
                    query_futures[1].Status() == QueryStatus::Error &&
                    signal_probes[0]->IsRejected(70) &&
                    signal_probes[1]->IsRejected(71);
                first_completion_observed_atomic_group.store(
                    group_terminal, std::memory_order_release
                );
            }
        );

        QueryToken query = command_lists[index].BeginTimestampQuery(
            "DirectPartialMaterializationQuery"
        );
        command_lists[index].EndTimestampQuery(query);
        query_futures[index] = query.GetFuture();
        query_futures[index].Then(
            [&, index](const QueryResult& _result) {
                if (_result.status == QueryStatus::Error) {
                    query_callbacks[index].fetch_add(
                        1, std::memory_order_release
                    );
                }
            }
        );
        command_lists[index].AddCallback([&, index] {
            ordinary_observed_notifications[index].store(
                completion_callbacks[index].load(
                    std::memory_order_acquire
                ) == 1 &&
                    query_callbacks[index].load(
                        std::memory_order_acquire
                    ) == 1,
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
    }

    PresentReceiptRef receipt = std::make_shared<PresentReceipt>();
    RHIPresentRequest present{};
    present.receipt = receipt;
    DirectMaterializationFaultContext fault_context{};
    const RHIDirectCommandListMaterializationOverrideForTesting
        materialization_override{
            .context       = &fault_context,
            .before_source = &InjectDirectMaterializationFailure,
        };
    if (!Expect(
            TryInstallRHIDirectCommandListMaterializationOverrideForTesting(
                &materialization_override
            ),
            "could not install direct materialization failure override"
        )) {
        return false;
    }

    bool submit_threw = false;
    try {
        RHIExecutor::Get().Submit(
            std::move(command_lists),
            ERHIExecSubmitFlags::None,
            &present
        );
    } catch (...) {
        submit_threw = true;
    }
    const bool override_removed =
        RemoveRHIDirectCommandListMaterializationOverrideForTesting(
            &materialization_override
        );

    bool okay =
        Expect(
            !submit_threw && override_removed &&
                fault_context.visited_sources.load(
                    std::memory_order_acquire
                ) == SourceCount,
            "partial materialization fault did not traverse the intended mixed prefix/suffix path"
        ) &&
        Expect(
            first_completion_observed_atomic_group.load(
                std::memory_order_acquire
            ),
            "partial materialization callback observed a non-terminal sibling or signal"
        ) &&
        Expect(
            !reentrant_submit_threw.load(std::memory_order_acquire),
            "partial materialization callback could not re-enter a fresh generation"
        );
    for (size_t index = 0; index < SourceCount; ++index) {
        okay &= Expect(
            completion_futures[index].WaitFor(100ms) &&
                completion_futures[index].Status() ==
                    GpuCompletionStatus::Error &&
                query_futures[index].WaitFor(100ms) &&
                query_futures[index].Status() == QueryStatus::Error,
            "partial materialization left a host Future Pending"
        );
        okay &= Expect(
            signal_probes[index]->IsRejected(70 + index) &&
                signal_rejections[index].load(
                    std::memory_order_acquire
                ) == 1 &&
                signal_observed_host_publication[index].load(
                    std::memory_order_acquire
                ),
            "partial materialization did not publish hosts before rejecting a signal exactly once"
        );
        okay &= Expect(
            completion_callbacks[index].load(
                std::memory_order_acquire
            ) == 1 &&
                query_callbacks[index].load(
                    std::memory_order_acquire
                ) == 1 &&
                ordinary_callbacks[index].load(
                    std::memory_order_acquire
                ) == 1 &&
                ordinary_observed_notifications[index].load(
                    std::memory_order_acquire
                ),
            "partial materialization did not release callbacks once in terminal order"
        );
        okay &= Expect(
            success_callbacks[index].load(
                std::memory_order_acquire
            ) == 0,
            "partial materialization invoked a success-only callback"
        );
        okay &= Expect(
            reentrant_submits[index].has_value() &&
                HasNoRejectedGenerationPayload(
                    *reentrant_submits[index]
                ) &&
                command_lists[index].IsEmpty(),
            "partial materialization exposed or restored a rejected generation"
        );
    }
    const PresentReceiptResult present_result =
        receipt->WaitForSubmission(100ms);
    okay &= Expect(
        present_result.resolved && !present_result.submitted &&
            receipt->ResolutionAttemptCount() == 1,
        "partial materialization did not reject Present exactly once"
    );
    return okay;
}

bool DirectMaterializationOverrideRemovalWaitsForReaders() {
    BlockingDirectMaterializationContext context{};
    const RHIDirectCommandListMaterializationOverrideForTesting
        materialization_override{
            .context       = &context,
            .before_source = &BlockDirectMaterializationUntilReleased,
        };
    if (!Expect(
            TryInstallRHIDirectCommandListMaterializationOverrideForTesting(
                &materialization_override
            ),
            "could not install blocking materialization override"
        )) {
        return false;
    }

    RecordingFenceProbe signal{};
    Moer::Array<CommandList> command_lists{};
    command_lists.emplace_back(EQueueType::Graphics);
    command_lists.front().Signal(&signal, 79);
    std::atomic<bool> submit_returned{false};
    std::atomic<bool> submit_threw{false};
    std::jthread submit_thread([&] {
        try {
            RHIExecutor::Get().Submit(
                std::move(command_lists),
                ERHIExecSubmitFlags::None
            );
        } catch (...) {
            submit_threw.store(true, std::memory_order_release);
        }
        submit_returned.store(true, std::memory_order_release);
    });

    {
        std::unique_lock lock(context.mutex);
        if (!context.cv.wait_for(
                lock, 2s, [&] { return context.entered; }
            )) {
            context.release = true;
            context.cv.notify_all();
            submit_thread.join();
            (void)RemoveRHIDirectCommandListMaterializationOverrideForTesting(
                &materialization_override
            );
            return Expect(
                false,
                "blocking materialization override was not acquired"
            );
        }
    }

    std::atomic<bool> release_issued{false};
    std::jthread release_thread([&] {
        std::this_thread::sleep_for(30ms);
        {
            std::lock_guard lock(context.mutex);
            context.release = true;
            release_issued.store(true, std::memory_order_release);
        }
        context.cv.notify_all();
    });
    const bool remove_succeeded =
        RemoveRHIDirectCommandListMaterializationOverrideForTesting(
            &materialization_override
        );
    bool okay = Expect(
        remove_succeeded &&
            release_issued.load(std::memory_order_acquire),
        "override removal returned before its active reader released caller storage"
    );
    submit_thread.join();
    release_thread.join();

    okay &= Expect(
        submit_returned.load(std::memory_order_acquire) &&
            !submit_threw.load(std::memory_order_acquire) &&
            signal.IsRejected(79),
        "blocking materialization failure did not finish its rejected transaction"
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
    std::optional<CmdSubmit> completion_reentrant_submit{};
    std::atomic<bool> completion_reentrant_submit_threw{false};
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

    RecordingFenceProbe last_signal{};
    last_list->Signal(&last_signal, 61);
    QueryToken last_payload_query =
        last_list->BeginTimestampQuery(
            "CompletionPreflightSiblingPayload"
        );
    const QueryFuture last_payload_future =
        last_payload_query.GetFuture();
    last_list->EndTimestampQuery(last_payload_query);

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
            try {
                // Publication and notification are externally re-entrant.
                // Even though the last source has not reached Submit(), its
                // rejected generation must already be detached before the
                // first source's completion callback can inspect it.
                completion_reentrant_submit.emplace(
                    last_list->Submit()
                );
            } catch (...) {
                completion_reentrant_submit_threw.store(
                    true, std::memory_order_release
                );
            }
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
        !completion_reentrant_submit_threw.load(
            std::memory_order_acquire
        ) &&
            completion_reentrant_submit.has_value(),
        "completion callback could not re-enter an unmaterialized sibling"
    );
    if (completion_reentrant_submit.has_value()) {
        okay &= Expect(
            HasNoRejectedGenerationPayload(
                *completion_reentrant_submit
            ),
            "completion callback extracted a rejected sibling payload"
        );
    }
    okay &= Expect(
        last_signal.IsRejected(61),
        "completion preflight failure left its sibling signal Pending"
    );
    okay &= Expect(
        last_payload_future.WaitFor(100ms) &&
            last_payload_future.Status() == QueryStatus::Error,
        "completion preflight failure left its sibling Query token Pending"
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

bool ShutdownPreservesACompletedSourceUntilCommitHandoff() {
    RHIExecutor::StartUp();

    RecordingFenceProbe list_signal{};
    FenceRef metadata_signal = MoerNew(RecordingFenceProbe)();
    auto* metadata_probe =
        static_cast<RecordingFenceProbe*>(metadata_signal.Get());
    auto command_list =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    command_list->Signal(&list_signal, 37);
    const GpuCompletionFuture completion =
        command_list->TrackGpuCompletion(
            "CommitOpaqueShutdownCompletion"
        );
    QueryToken query = command_list->BeginTimestampQuery(
        "CommitOpaqueShutdownQuery"
    );
    command_list->EndTimestampQuery(query);
    const QueryFuture query_future = query.GetFuture();

    std::atomic<int> completion_callbacks{0};
    std::atomic<int> query_callbacks{0};
    std::atomic<int> ordinary_callbacks{0};
    std::atomic<int> success_callbacks{0};
    completion.Then([&](const GpuCompletionResult&) {
        completion_callbacks.fetch_add(
            1, std::memory_order_release
        );
    });
    query_future.Then([&](const QueryResult&) {
        query_callbacks.fetch_add(1, std::memory_order_release);
    });
    command_list->AddCallback([&] {
        ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    command_list->AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_release);
    });

    auto producer_complete = RHIRecordingGate::Create(true);
    auto graph_commit = RHIRecordingGate::Create();
    Moer::Array<RHIRecordingSource> sources{};
    sources.emplace_back(RHIRecordingSource{
        .command_list = command_list,
        .completion   = producer_complete,
        .commit       = graph_commit,
        .submit_metadata = RHIRecordingSubmitMetadata{
            .signal_fences = {
                RHIRecordingFencePoint{
                    .fence = metadata_signal,
                    .value = 41,
                },
            },
        },
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::ShutDown();

    bool okay =
        Expect(
            graph_commit->Status() == ERHIRecordingStatus::Pending,
            "shutdown changed a graph-owned Pending commit gate"
        ) &&
        Expect(
            !command_list->IsEmpty() &&
                !list_signal.IsRejected(37) &&
                metadata_probe->RejectedValue() == 0,
            "shutdown drained or rejected a source before commit handoff"
        ) &&
        Expect(
            completion.Status() == GpuCompletionStatus::Error &&
                query_future.Status() == QueryStatus::Error &&
                completion_callbacks.load(
                    std::memory_order_acquire
                ) == 0 &&
                query_callbacks.load(std::memory_order_acquire) == 0,
            "opaque cancellation did not defer Future callbacks behind commit ownership"
        ) &&
        Expect(
            ordinary_callbacks.load(std::memory_order_acquire) == 0 &&
                success_callbacks.load(std::memory_order_acquire) == 0,
            "shutdown released callbacks before commit ownership transferred"
        );

    graph_commit->Fail();
    Moer::Array<std::function<void()>> cleanup =
        command_list->DrainOrdinaryCallbacksForRejection();
    for (std::function<void()>& callback : cleanup) {
        if (callback) {
            callback();
        }
    }
    okay &= Expect(
        list_signal.IsRejected(37) &&
            completion_callbacks.load(
                std::memory_order_acquire
            ) == 1 &&
            query_callbacks.load(std::memory_order_acquire) == 1 &&
            ordinary_callbacks.load(std::memory_order_acquire) == 1 &&
            success_callbacks.load(std::memory_order_acquire) == 0 &&
            command_list->IsEmpty(),
        "commit owner could not terminally drain the deferred cancellation"
    );
    return okay;
}

bool MetadataFailureRejectsEveryProducerSignal() {
    RHIExecutor::StartUp();

    auto transferred_source =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    auto later_source =
        Moer::MakeShared<CommandList>(EQueueType::Graphics);
    std::array<GpuCompletionFuture, 2> completion_futures{
        transferred_source->TrackGpuCompletion(
            "MetadataTransferredCompletion"
        ),
        later_source->TrackGpuCompletion("MetadataFallbackCompletion"),
    };
    std::array<QueryFuture, 2> query_futures{};
    {
        QueryToken query = transferred_source->BeginTimestampQuery(
            "MetadataTransferredQuery"
        );
        transferred_source->EndTimestampQuery(query);
        query_futures[0] = query.GetFuture();
    }
    {
        QueryToken query = later_source->BeginTimestampQuery(
            "MetadataFallbackQuery"
        );
        later_source->EndTimestampQuery(query);
        query_futures[1] = query.GetFuture();
    }

    std::array<std::atomic<int>, 2> signal_rejections{};
    std::array<std::atomic<bool>, 2> signal_observed_host_terminal{};
    std::optional<CmdSubmit> metadata_reentrant_submit{};
    std::atomic<bool> metadata_reentrant_submit_threw{false};
    const auto all_hosts_terminal = [&] {
        return completion_futures[0].Status() ==
                   GpuCompletionStatus::Error &&
               completion_futures[1].Status() ==
                   GpuCompletionStatus::Error &&
               query_futures[0].Status() == QueryStatus::Error &&
               query_futures[1].Status() == QueryStatus::Error;
    };
    FenceRef transferred_signal = MoerNew(RecordingFenceProbe)(
        std::shared_ptr<std::atomic_bool>{},
        [&] {
            signal_observed_host_terminal[0].store(
                all_hosts_terminal(), std::memory_order_release
            );
            signal_rejections[0].fetch_add(
                1, std::memory_order_release
            );
            try {
                metadata_reentrant_submit.emplace(
                    transferred_source->Submit()
                );
            } catch (...) {
                metadata_reentrant_submit_threw.store(
                    true, std::memory_order_release
                );
            }
        }
    );
    auto* transferred_probe =
        static_cast<RecordingFenceProbe*>(transferred_signal.Get());
    FenceRef fallback_signal = MoerNew(RecordingFenceProbe)(
        std::shared_ptr<std::atomic_bool>{},
        [&] {
            signal_observed_host_terminal[1].store(
                all_hosts_terminal(), std::memory_order_release
            );
            signal_rejections[1].fetch_add(
                1, std::memory_order_release
            );
        }
    );
    auto* fallback_probe =
        static_cast<RecordingFenceProbe*>(fallback_signal.Get());

    Moer::Array<RHIRecordingSource> sources{};
    sources.emplace_back(RHIRecordingSource{
        .command_list = transferred_source,
        .completion   = RHIRecordingGate::Create(true),
        .submit_metadata = RHIRecordingSubmitMetadata{
            .signal_fences = {
                RHIRecordingFencePoint{
                    .fence = transferred_signal,
                    .value = 5,
                },
                // The first point has already transferred into CmdSubmit when
                // this deterministic validation failure is reached.
                RHIRecordingFencePoint{.fence = {}, .value = 6},
            },
        },
    });
    sources.emplace_back(RHIRecordingSource{
        .command_list = later_source,
        .completion   = RHIRecordingGate::Create(true),
        .submit_metadata = RHIRecordingSubmitMetadata{
            .signal_fences = {
                RHIRecordingFencePoint{
                    .fence = fallback_signal,
                    .value = 7,
                },
            },
        },
    });
    RHIExecutor::Get().SubmitRecording(
        std::move(sources),
        ERHIExecSubmitFlags::None
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    RHIExecutor::ShutDown();

    bool okay =
        Expect(
            transferred_probe->RejectedValue() == 5 &&
                signal_rejections[0].load(
                    std::memory_order_acquire
                ) == 1,
            "metadata failure rejected a transferred producer signal more than once"
        ) &&
        Expect(
            fallback_probe->RejectedValue() == 7 &&
                signal_rejections[1].load(
                    std::memory_order_acquire
                ) == 1,
            "metadata failure did not reject an untouched producer signal exactly once"
        ) &&
        Expect(
            signal_observed_host_terminal[0].load(
                std::memory_order_acquire
            ) &&
                signal_observed_host_terminal[1].load(
                    std::memory_order_acquire
                ),
            "metadata rejection ran before every host Future was terminal"
        ) &&
        Expect(
            !metadata_reentrant_submit_threw.load(
                std::memory_order_acquire
            ) &&
                metadata_reentrant_submit.has_value() &&
                HasNoRejectedGenerationPayload(
                    *metadata_reentrant_submit
                ),
            "metadata rejection re-entry extracted the rejected generation"
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
        cancelled_probe->RejectedValue() == 0,
        "shutdown cancellation touched metadata owned by an opaque producer"
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
        !CommandListFenceReentryCannotExtractRejectedGeneration() ||
        !FenceRejectionObservesPublishedHostsAndRetainsOwnedSignals() ||
        !ExportReceiptSignalsOwnDedicatedRejectionKeepalives() ||
        !CommandDestructorReentryTargetsOnlyFreshGeneration() ||
        !CmdSubmitCallbackCaptureDestructorSeesCompleteReplacement() ||
        !CommandListCallbackCaptureDestructorSeesCompleteReplacement() ||
        !FrontendQueryRejectionTerminalizesBeforeOrdinaryCallbacks() ||
        !FrontendCompletionRejectionIsOneTerminalTransaction() ||
        !FrontendPresentRejectionPublishesBatchBeforeReceipt() ||
        !DeferredOpaqueCancellationTicketSurvivesSubmit() ||
        !DirectCommandListGroupPreflightRejectsEverySource() ||
        !DirectCommandListGroupActiveLeaseFailsWithoutMutation() ||
        !DirectCommandListGroupPartialMaterializationFailureIsAtomic() ||
        !DirectMaterializationOverrideRemovalWaitsForReaders() ||
        !RecordingPreflightFailureTerminalizesEveryQueryGeneration() ||
        !RecordingPreflightFailureTerminalizesEveryCompletionGeneration() ||
        !SubmitRecordingRefreshesAStaleQueryCancellationView() ||
        !SubmitRecordingRefreshesAStaleCompletionCancellationView() ||
        !PerSourceGatesPreserveFifo() ||
        !ReadyWorkUsesStableExecutorOwnerAndCannotBeOvertaken() ||
        !FailedGateRejectsOnlyItsGroupAndPreservesFifo() ||
        !FailedRecordingDrainsCleanupExactlyOnce() ||
        !FailedCommitRejectsCompletedSources() ||
        !ShutdownPreservesACompletedSourceUntilCommitHandoff() ||
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
