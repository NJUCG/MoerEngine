#include "VulkanSubmissionExecutor.h"

#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "rhi/RHIThreadOwnership.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <string>

namespace Moer::Render {
namespace {

GraphEventRef MakeCompletedEvent() {
    GraphEventRef event = GraphEvent::CreateGraphEvent();
    event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return event;
}

bool SubmitHasSideEffects(const CmdSubmit& _submit) {
    return !_submit.callbacks.empty() || !_submit.success_callbacks.empty() ||
           !_submit.wait_events.empty() || !_submit.signal_events.empty() || _submit.b_sync ||
           _submit.b_tick_profiling || _submit.b_delete_resources;
}

bool IsNativeQueue(EQueueType _queue) {
    return _queue == EQueueType::Graphics || _queue == EQueueType::Compute ||
           _queue == EQueueType::Copy;
}

bool IsCopyCommand(Command::EType _type) {
    switch (_type) {
        case Command::EType::UploadBuffer:
        case Command::EType::CopyBackBuffer:
        case Command::EType::BufferToBuffer:
        case Command::EType::BufferToTexture:
        case Command::EType::TextureToBuffer:
        case Command::EType::UploadTexture:
        case Command::EType::TextureToTexture:
        case Command::EType::CopyBackTexture:
        case Command::EType::Barrier:
        case Command::EType::QueueTransfer:
            return true;
        default:
            return false;
    }
}

struct BatchPreflightResult {
    bool             valid{true};
    std::string_view reason{"none"};
    size_t           source_index{std::numeric_limits<size_t>::max()};
    size_t           command_index{std::numeric_limits<size_t>::max()};
};

BatchPreflightResult RejectPreflight(
    std::string_view _reason,
    size_t           _source_index = std::numeric_limits<size_t>::max(),
    size_t           _command_index = std::numeric_limits<size_t>::max()
) {
    return {
        .valid         = false,
        .reason        = _reason,
        .source_index  = _source_index,
        .command_index = _command_index,
    };
}

BatchPreflightResult ValidateBatch(const RHIBackendSubmissionBatch& _batch) {
    const RHISubmissionTopologyPlan& topology = _batch.topology;
    if (!topology.IsValid()) {
        return RejectPreflight("invalid topology", topology.error_source_index);
    }
    if (topology.source_plans.size() != _batch.submits.size()) {
        return RejectPreflight("source/topology count mismatch");
    }
    if (_batch.present &&
        (!_batch.present->swapchain || _batch.present->source.texture == nullptr)) {
        return RejectPreflight("invalid Present request");
    }

    size_t expected_segment_begin = 0;
    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        const RHIBackendSubmissionBatchEntry& entry = _batch.submits[source_index];
        const CmdSubmit& submit = entry.submit;
        const RHISourceSubmitPlan& source_plan = topology.source_plans[source_index];
        const bool submit_has_work = !submit.cmds.empty() || SubmitHasSideEffects(submit);

        if (!IsNativeQueue(entry.queue)) {
            return RejectPreflight("invalid source queue", source_index);
        }
        if (source_plan.source_key.op_seq != _batch.sequence ||
            source_plan.source_key.submit_idx != source_index ||
            source_plan.root_queue != entry.queue ||
            source_plan.command_count != submit.cmds.size() ||
            source_plan.execution_class != submit.translate_execution_class ||
            source_plan.has_side_effects != submit_has_work) {
            return RejectPreflight("source plan does not match payload", source_index);
        }
        if (source_plan.segment_plan_begin != expected_segment_begin) {
            return RejectPreflight("non-contiguous source plans", source_index);
        }
        if (source_plan.segment_plan_count > 1) {
            return RejectPreflight("multi-segment source is not supported", source_index);
        }

        const size_t expected_segment_count = submit_has_work ? 1u : 0u;
        if (source_plan.segment_plan_count != expected_segment_count) {
            return RejectPreflight("source retention does not match payload", source_index);
        }

        if (source_plan.segment_plan_count == 1) {
            if (expected_segment_begin >= topology.segments.size()) {
                return RejectPreflight("source segment is out of range", source_index);
            }
            const RHISubmissionSegmentPlan& segment_plan =
                topology.segments[expected_segment_begin];
            if (segment_plan.key.op_seq != _batch.sequence ||
                segment_plan.key.submit_idx != expected_segment_begin ||
                segment_plan.source_key != source_plan.source_key ||
                segment_plan.source_segment_index != 0 ||
                segment_plan.segment.queue != source_plan.root_queue ||
                segment_plan.segment.begin != 0 ||
                segment_plan.segment.end != submit.cmds.size() ||
                segment_plan.execution_class != source_plan.execution_class ||
                !segment_plan.inherit_source_wait_events ||
                !segment_plan.inherit_source_signal_events_and_callbacks ||
                !segment_plan.inherit_source_runtime_payload) {
                return RejectPreflight("single-segment plan does not match source", source_index);
            }

            if (!submit.segments.empty()) {
                if (submit.segments.size() != 1 ||
                    submit.segments.front().queue != source_plan.root_queue ||
                    submit.segments.front().begin != 0 ||
                    submit.segments.front().end != submit.cmds.size()) {
                    return RejectPreflight(
                        "source segment does not match root queue", source_index
                    );
                }
            }
        }

        for (size_t command_index = 0; command_index < submit.cmds.size(); ++command_index) {
            const UniquePtr<Command>& command = submit.cmds[command_index];
            if (!command) {
                return RejectPreflight("null command", source_index, command_index);
            }
            if (command->Type() >= Command::EType::Count) {
                return RejectPreflight("invalid command type", source_index, command_index);
            }
            if (entry.queue == EQueueType::Copy && !IsCopyCommand(command->Type())) {
                return RejectPreflight(
                    "unsupported command on Copy queue", source_index, command_index
                );
            }
        }

        for (const WaitEvent& wait : submit.wait_events) {
            if (wait.timeline_handle == 0) {
                return RejectPreflight("null wait fence", source_index);
            }
        }
        for (const SignalEvent& signal : submit.signal_events) {
            if (signal.timeline_handle == 0) {
                return RejectPreflight("null signal fence", source_index);
            }
        }

        if (entry.queue == EQueueType::Copy) {
            if (!submit.cached_args.empty()) {
                return RejectPreflight("Copy submit contains cached arguments", source_index);
            }
            if (submit.b_sync || submit.b_tick_profiling || submit.b_delete_resources) {
                return RejectPreflight("Copy submit contains unsupported control flags", source_index);
            }
            if (!submit.debug_label.empty()) {
                return RejectPreflight("Copy submit debug labels are not supported", source_index);
            }
        }

        expected_segment_begin += source_plan.segment_plan_count;
    }

    if (expected_segment_begin != topology.segments.size()) {
        return RejectPreflight("unclaimed topology segments");
    }
    return {};
}

const char* TopologyErrorName(ERHISubmissionTopologyError _error) {
    switch (_error) {
        case ERHISubmissionTopologyError::None:
            return "none";
        case ERHISubmissionTopologyError::InvalidRootQueue:
            return "invalid-root-queue";
        case ERHISubmissionTopologyError::InvalidExecutionClass:
            return "invalid-execution-class";
        case ERHISubmissionTopologyError::InvalidSegmentQueue:
            return "invalid-segment-queue";
        case ERHISubmissionTopologyError::InvalidSegmentRange:
            return "invalid-segment-range";
        case ERHISubmissionTopologyError::NonContiguousSegmentCoverage:
            return "non-contiguous-segment-coverage";
        case ERHISubmissionTopologyError::IndexOverflow:
            return "index-overflow";
    }
    return "unknown";
}

void ResolveRejectedPresent(std::optional<RHIPresentRequest>& _present) {
    if (_present && _present->receipt) {
        _present->receipt->Resolve(false);
    }
}

} // namespace

VulkanSubmissionExecutor::VulkanSubmissionExecutor() {
    auto& graphics_queue = static_cast<VkCommandQueue&>(
        RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
    );
    auto& compute_queue = static_cast<VkCommandQueue&>(
        RenderDevice::Get().GetCommandQueue(EQueueType::Compute)
    );
    auto& copy_queue =
        static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
    bool graphics_claimed = false;
    bool compute_claimed  = false;
    try {
        graphics_claimed = graphics_queue.ClaimRuntimeOwnership();
        if (!graphics_claimed) {
            throw std::runtime_error(
                "Vulkan Graphics queue ownership conflicts with legacy execution"
            );
        }
        compute_claimed = compute_queue.ClaimRuntimeOwnership();
        if (!compute_claimed) {
            throw std::runtime_error(
                "Vulkan Compute queue ownership conflicts with legacy execution"
            );
        }
        copy_queue.EnableRuntimeDependencyWaits();
        claims_owned = true;

        submission_thread = std::jthread([this] { RunSubmission(); });
        executor_thread   = std::jthread([this] { RunExecutor(); });
        LOG_INFO(
            "[RHIExecutor][Vulkan] ownership runtime started "
            "Executor=1 Translate=1 Submission=1 window=1"
        );
    } catch (...) {
        graphics_queue.CancelRuntimeDependencyWaits();
        compute_queue.CancelRuntimeDependencyWaits();
        copy_queue.CancelRuntimeDependencyWaits();
        if (executor_thread.joinable()) {
            {
                std::lock_guard lock(mutex);
                accepting        = false;
                constructor_abort = true;
            }
            cv.notify_all();
            executor_thread.join();
        }
        if (submission_thread.joinable()) {
            StopSubmissionThread();
        }
        if (compute_claimed) {
            compute_queue.ReleaseRuntimeOwnership();
        }
        if (graphics_claimed) {
            graphics_queue.ReleaseRuntimeOwnership();
        }
        claims_owned = false;
        throw;
    }
}

VulkanSubmissionExecutor::~VulkanSubmissionExecutor() {
    ShutDown();
    if (claims_owned) {
        auto& graphics_queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
        );
        auto& compute_queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Compute)
        );
        compute_queue.ReleaseRuntimeOwnership();
        graphics_queue.ReleaseRuntimeOwnership();
        claims_owned = false;
    }
}

void VulkanSubmissionExecutor::Completion::Signal() {
    {
        std::lock_guard lock(mutex);
        done = true;
    }
    cv.notify_all();
}

void VulkanSubmissionExecutor::Completion::Wait() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return done; });
}

void VulkanSubmissionExecutor::Enqueue(RHIBackendSubmissionBatch&& _batch) {
    bool rejected = false;
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            rejected = true;
        } else {
            requests.emplace_back(Request{
                .kind       = ERequestKind::Submit,
                .batch      = std::move(_batch),
            });
        }
    }
    if (rejected) {
        // User completion callbacks must never run while the backend FIFO lock
        // is held: they may enqueue more RHI work or acquire unrelated locks.
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(VK_ERROR_UNKNOWN),
            "submission runtime is stopping or stopped"
        );
        return;
    }
    cv.notify_one();
}

GraphEventRef VulkanSubmissionExecutor::Sync(ERHISyncDepth _depth) {
    auto completion = std::make_shared<Completion>();
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            return MakeCompletedEvent();
        }
        requests.emplace_back(Request{
            .kind       = ERequestKind::Sync,
            .sync_depth = _depth,
            .completion = completion,
        });
    }
    cv.notify_one();
    completion->Wait();
    return MakeCompletedEvent();
}

void VulkanSubmissionExecutor::Flush(ERHIFlushDepth) {
    // Enqueue publishes directly to the runtime FIFO and wakes its owner.
    // SubmitGPU intentionally does not wait; Sync is the explicit host boundary.
    cv.notify_one();
}

void VulkanSubmissionExecutor::BeginShutdown() noexcept {
    try {
        auto& graphics_queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
        );
        auto& compute_queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Compute)
        );
        auto& copy_queue =
            static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());

        // A Submission owner can be blocked waiting for a producer timeline
        // which shutdown has stopped admitting. Publish cancellation without
        // waiting for the backend FIFO so concurrent Sync calls can drain.
        graphics_queue.CancelRuntimeDependencyWaits();
        compute_queue.CancelRuntimeDependencyWaits();
        copy_queue.CancelRuntimeDependencyWaits();
    } catch (...) {
        // RenderDevice queue access is expected to remain valid until backend
        // destruction. Keep this hook noexcept so lifecycle cleanup can still
        // advance if that contract is broken by an exceptional teardown.
    }
}

void VulkanSubmissionExecutor::ShutDown() {
    BeginShutdown();
    std::shared_ptr<Completion> completion{};
    bool                        join_owner = false;

    // Allocate the candidate stop completion before mutating shared lifecycle
    // state. A failed allocation must leave the runtime fully retryable.
    {
        std::lock_guard lock(mutex);
        if (stopped) {
            return;
        }
        completion = stop_completion;
    }
    std::shared_ptr<Completion> candidate{};
    if (!completion) {
        candidate = std::make_shared<Completion>();
    }

    {
        std::lock_guard lock(mutex);
        if (stopped) {
            return;
        }
        if (!stop_completion) {
            Request stop_request{
                .kind       = ERequestKind::Stop,
                .sync_depth = ERHISyncDepth::Present,
                .completion = candidate,
            };
            // deque insertion has the strong exception guarantee. Publish the
            // shared stop state only after the request is owned by the FIFO.
            requests.emplace_back(std::move(stop_request));
            stop_completion = candidate;
            join_owner = true;
        }
        accepting = false;
        completion = stop_completion;
    }
    cv.notify_one();
    completion->Wait();
    if (join_owner) {
        if (executor_thread.joinable()) {
            executor_thread.join();
        }
        {
            std::lock_guard lock(mutex);
            stopped = true;
        }
        cv.notify_all();
        return;
    }

    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return stopped; });
}

void VulkanSubmissionExecutor::RunExecutor() {
    Platform::SetCurrentThreadName("Moer Vulkan Translate");
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Translate);
    while (true) {
        Request request{};
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] {
                return constructor_abort || !requests.empty();
            });
            if (constructor_abort && requests.empty()) {
                break;
            }
            request = std::move(requests.front());
            requests.pop_front();
        }

        BatchExceptionState exception_state{};
        const VulkanSubmissionDetail::WorkerRequestDispatchResult dispatch_result =
            VulkanSubmissionDetail::DispatchWorkerRequestNoexcept(
                request.kind,
                [&] {
                    if (request.kind == ERequestKind::Submit) {
                        ProcessBatch(request.batch, exception_state);
                    } else {
                        ProcessSync(request.sync_depth);
                    }
                },
                [&] {
                    hard_failed         = true;
                    hard_failure_result = static_cast<int32>(VK_ERROR_UNKNOWN);
                    RejectBatch(
                        std::move(request.batch),
                        hard_failure_result,
                        "unhandled submission request exception",
                        exception_state.first_unconsumed_source,
                        &exception_state.first_unconsumed_source
                    );
                },
                [&] { CompleteRequest(request.completion); },
                [&](VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
                    const std::exception_ptr& _exception) {
                    hard_failed         = true;
                    hard_failure_result = static_cast<int32>(VK_ERROR_UNKNOWN);
                    ReportRequestFailure(request.kind, _phase, _exception);
                }
            );
        if (dispatch_result.stop) {
            break;
        }
    }
    StopSubmissionThread();
}

bool VulkanSubmissionExecutor::EnqueueSubmissionWork(SubmissionWork&& _work) {
    {
        std::lock_guard lock(submission_mutex);
        if (!submission_accepting) {
            return false;
        }
        submission_work.emplace_back(std::move(_work));
    }
    submission_cv.notify_one();
    return true;
}

void VulkanSubmissionExecutor::RunSubmission() {
    Platform::SetCurrentThreadName("Moer Vulkan Submission");
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Submission);
    for (;;) {
        SubmissionWork work{};
        {
            std::unique_lock lock(submission_mutex);
            submission_cv.wait(lock, [this] {
                return !submission_accepting || !submission_work.empty();
            });
            if (submission_work.empty()) {
                assert(!submission_accepting);
                break;
            }
            work = std::move(submission_work.front());
            submission_work.pop_front();
        }
        if (work.execute.valid()) {
            // packaged_task captures exceptions and publishes them to the
            // waiting Translate owner; none may escape this service thread.
            work.execute();
        }
    }
}

void VulkanSubmissionExecutor::StopSubmissionThread() {
    {
        std::lock_guard lock(submission_mutex);
        submission_accepting = false;
    }
    submission_cv.notify_all();
    if (submission_thread.joinable()) {
        submission_thread.join();
    }
}

void VulkanSubmissionExecutor::ReportRequestFailure(
    ERequestKind                                        _kind,
    VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
    const std::exception_ptr&                           _exception
) noexcept {
    const char* kind_name = "Submit";
    switch (_kind) {
        case ERequestKind::Submit:
            break;
        case ERequestKind::Sync:
            kind_name = "Sync";
            break;
        case ERequestKind::Stop:
            kind_name = "Stop";
            break;
    }

    const char* phase_name = "process";
    switch (_phase) {
        case VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process:
            break;
        case VulkanSubmissionDetail::EWorkerRequestFailurePhase::Reject:
            phase_name = "reject";
            break;
        case VulkanSubmissionDetail::EWorkerRequestFailurePhase::Complete:
            phase_name = "complete";
            break;
    }

    try {
        if (_exception) {
            std::rethrow_exception(_exception);
        }
    } catch (const std::exception& error) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] request={} phase={} threw: {}",
                kind_name,
                phase_name,
                error.what()
            );
        } catch (...) {
        }
        return;
    } catch (...) {
    }

    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] request={} phase={} threw an unknown exception",
            kind_name,
            phase_name
        );
    } catch (...) {
    }
}

void VulkanSubmissionExecutor::RejectBatch(
    RHIBackendSubmissionBatch&& _batch,
    int32                       _result,
    std::string_view            _reason,
    size_t                      _first_source,
    size_t*                     _next_unconsumed_source
) {
    const size_t first_source = std::min(_first_source, _batch.submits.size());
    ResolveRejectedPresent(_batch.present);
    const VkResult result = static_cast<VkResult>(_result);
    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        RHIBackendSubmissionBatchEntry& entry = _batch.submits[source_index];
        if (entry.queue == EQueueType::Copy) {
            auto& queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            queue.RejectForRuntime(std::move(entry.submit), result);
            if (_next_unconsumed_source != nullptr) {
                *_next_unconsumed_source = source_index + 1;
            }
            continue;
        }

        const EQueueType queue_type =
            IsNativeQueue(entry.queue) && entry.queue != EQueueType::Copy ?
                entry.queue :
                EQueueType::Graphics;
        auto& queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(queue_type)
        );
        queue.RejectForRuntime(std::move(entry.submit), result);
        if (_next_unconsumed_source != nullptr) {
            *_next_unconsumed_source = source_index + 1;
        }
    }

    LOG_ERROR(
        "[RHIExecutor][Vulkan] reject batch={} sources={} reason={}",
        _batch.sequence,
        _batch.submits.size() - first_source,
        _reason
    );
}

void VulkanSubmissionExecutor::ProcessBatch(
    RHIBackendSubmissionBatch& _batch,
    BatchExceptionState&       _exception_state
) {
    if (hard_failed) {
        RejectBatch(
            std::move(_batch),
            hard_failure_result,
            "submission runtime is latched after a hard failure",
            0,
            &_exception_state.first_unconsumed_source
        );
        return;
    }

    const BatchPreflightResult preflight = ValidateBatch(_batch);
    if (!preflight.valid) {
        LOG_ERROR(
            "[RHIExecutor][Topology] batch={} preflight rejected reason={} "
            "topology_error={} source={} command={}",
            _batch.sequence,
            preflight.reason,
            TopologyErrorName(_batch.topology.error),
            preflight.source_index,
            preflight.command_index
        );
        hard_failed         = true;
        hard_failure_result = static_cast<int32>(VK_ERROR_UNKNOWN);
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(VK_ERROR_UNKNOWN),
            preflight.reason,
            0,
            &_exception_state.first_unconsumed_source
        );
        return;
    }

    if (!logged_multi_source_topology && _batch.submits.size() > 1) {
        logged_multi_source_topology = true;
        LOG_INFO(
            "[RHIExecutor][Vulkan] upper submission topology active: batch={} "
            "sources={} segments={} present={}",
            _batch.sequence,
            _batch.submits.size(),
            _batch.topology.segments.size(),
            _batch.present.has_value()
        );
    }

    auto reject_remaining = [&](size_t _begin, VkResult _result, std::string_view _reason) {
        if (_result == VK_SUCCESS) {
            _result = VK_ERROR_UNKNOWN;
        }
        hard_failed         = true;
        hard_failure_result = static_cast<int32>(_result);
        _exception_state.first_unconsumed_source =
            std::min(_begin, _batch.submits.size());
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(_result),
            _reason,
            _exception_state.first_unconsumed_source,
            &_exception_state.first_unconsumed_source
        );
        _exception_state.first_unconsumed_source = _batch.submits.size();
    };
    auto reject_remaining_recoverable =
        [&](size_t _begin, VkResult _result, std::string_view _reason) {
            if (_result == VK_SUCCESS) {
                _result = VK_ERROR_UNKNOWN;
            }
            _exception_state.first_unconsumed_source =
                std::min(_begin, _batch.submits.size());
            RejectBatch(
                std::move(_batch),
                static_cast<int32>(_result),
                _reason,
                _exception_state.first_unconsumed_source,
                &_exception_state.first_unconsumed_source
            );
            _exception_state.first_unconsumed_source = _batch.submits.size();
        };

    const RHIQueueTopology queue_topology =
        RenderDevice::Get().GetQueueTopology();
    auto same_native_queue = [&](EQueueType lhs, EQueueType rhs) {
        return queue_topology.Resolve(lhs).native_queue_id ==
               queue_topology.Resolve(rhs).native_queue_id;
    };
    auto append_unique_wait = [](CmdSubmit& submit, WaitEvent event) {
        const bool exists = std::any_of(
            submit.wait_events.begin(),
            submit.wait_events.end(),
            [&](const WaitEvent& candidate) {
                return candidate.timeline_handle == event.timeline_handle &&
                       candidate.value == event.value;
            }
        );
        if (!exists) {
            submit.wait_events.emplace_back(event);
        }
    };
    auto append_frontier_waits =
        [&](CmdSubmit& submit,
            EQueueType target_queue,
            const auto& frontier) {
            for (size_t index = 0; index < frontier.size(); ++index) {
                if (!frontier[index].has_value()) {
                    continue;
                }
                const auto source_queue = static_cast<EQueueType>(index);
                if (same_native_queue(source_queue, target_queue)) {
                    continue;
                }
                append_unique_wait(submit, *frontier[index]);
            }
        };
    auto mark_scope_queue_seen = [&](EQueueType queue) {
        for (size_t index = 0; index < async_scope_seen_queues.size();
             ++index) {
            if (same_native_queue(static_cast<EQueueType>(index), queue)) {
                async_scope_seen_queues[index] = true;
            }
        }
    };
    auto update_frontier =
        [&](EQueueType queue, WaitEvent completion, bool collapse) {
            if (collapse) {
                gpu_frontier.fill(std::nullopt);
            } else {
                for (size_t index = 0; index < gpu_frontier.size(); ++index) {
                    if (same_native_queue(static_cast<EQueueType>(index), queue)) {
                        gpu_frontier[index].reset();
                    }
                }
            }
            gpu_frontier[static_cast<size_t>(queue)] = completion;
        };

    // Keep every source in the request-owned batch until its queue call has
    // returned. If reorder, descriptor-lease, allocator, or native recording
    // setup throws, Run's request barrier can still terminalize the current
    // source callbacks/signals and every later source. Moving all sources into
    // a temporary expansion array here would lose those obligations on unwind.
    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        RHIBackendSubmissionBatchEntry& entry = _batch.submits[source_index];
        const RHISourceSubmitPlan& source_plan =
            _batch.topology.source_plans[source_index];
        if (source_plan.segment_plan_count == 0) {
            _exception_state.first_unconsumed_source = source_index + 1;
            continue;
        }

        assert(source_plan.segment_plan_count == 1);
        const RHISubmissionSegmentPlan& segment_plan =
            _batch.topology.segments[source_plan.segment_plan_begin];
        entry.submit.segments = {RHISubmitSegment{
            .queue = segment_plan.segment.queue,
            .begin = 0,
            .end   = entry.submit.cmds.size(),
        }};

        const uint64 async_scope = entry.submit.async_queue_scope;
        if (async_scope == 0) {
            // Legacy/direct submits retain a conservative total GPU order.
            active_async_queue_scope = 0;
            async_scope_entry_frontier.fill(std::nullopt);
            async_scope_seen_queues.fill(false);
            append_frontier_waits(entry.submit, entry.queue, gpu_frontier);
        } else {
            if (active_async_queue_scope != async_scope) {
                // A new graph transaction begins after the complete prior
                // frontier. Each native queue waits that frozen entry
                // frontier once, while explicit RDG waits order work inside
                // the scope.
                active_async_queue_scope    = async_scope;
                async_scope_entry_frontier  = gpu_frontier;
                async_scope_seen_queues.fill(false);
                if (!logged_async_queue_scope) {
                    LOG_INFO(
                        "[RHIExecutor][Vulkan][AsyncQueueScope] "
                        "mode=dependency-led native_submit_owner=serial "
                        "legacy_boundary=frontier"
                    );
                    logged_async_queue_scope = true;
                }
            }
            const size_t queue_index = static_cast<size_t>(entry.queue);
            if (!async_scope_seen_queues[queue_index]) {
                append_frontier_waits(
                    entry.submit, entry.queue, async_scope_entry_frontier
                );
                mark_scope_queue_seen(entry.queue);
            }
        }
        if (entry.queue == EQueueType::Copy) {
            auto& copy_queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            const VulkanRuntimeSubmissionResult submit_result =
                ExecuteOnSubmissionThread(
                    [&copy_queue, submit = &entry.submit]() mutable {
                        return copy_queue.ExecuteForRuntime(std::move(*submit));
                    }
                );
            // ExecuteForRuntime has now either submitted or terminalized this
            // source. Never reject it again if later bookkeeping/logging throws.
            _exception_state.first_unconsumed_source = source_index + 1;
            if (submit_result.WasSubmitted()) {
                assert(submit_result.completion.has_value());
                last_copy_timeline = std::max(
                    last_copy_timeline, submit_result.completion->value
                );
                update_frontier(
                    EQueueType::Copy,
                    *submit_result.completion,
                    async_scope == 0
                );
                used_queues[static_cast<size_t>(EQueueType::Copy)] = true;
            } else if (submit_result.IsRecoverableRejection()) {
                // A failed prerequisite did not reach vkQueueSubmit. Drain the
                // current source on Completion, reject the rest of this
                // topology batch, and keep the runtime available for a later
                // independent batch.
                used_queues[static_cast<size_t>(EQueueType::Copy)] = true;
                reject_remaining_recoverable(
                    source_index + 1,
                    submit_result.outcome.result,
                    "Copy submission dependency rejection"
                );
                return;
            } else if (submit_result.IsHardFailure()) {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} Copy submission failed status={} result={}",
                    _batch.sequence,
                    static_cast<uint32>(submit_result.outcome.status),
                    static_cast<int32>(submit_result.outcome.result)
                );
                // A failed native Copy submit still publishes a CPU retirement
                // marker. ProcessSync always enters Copy Sync, whose independent
                // retirement serial drains it without reading Copy-owner state.
                used_queues[static_cast<size_t>(EQueueType::Copy)] = true;
                reject_remaining(
                    source_index + 1,
                    submit_result.outcome.result,
                    "Copy submission hard failure"
                );
                return;
            }
        } else {
            auto& queue = static_cast<VkCommandQueue&>(
                RenderDevice::Get().GetCommandQueue(entry.queue)
            );
            std::optional<VkCommandQueue::CurrentVulkanRecordedSubmit> recorded =
                queue.TranslateForRuntime(std::move(entry.submit));
            // Translation reserves a logical queue timeline and always
            // publishes a CPU completion marker, including rejected paths.
            used_queues[static_cast<size_t>(entry.queue)] = true;
            if (!recorded) {
                _exception_state.first_unconsumed_source = source_index + 1;
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} source_queue={} translation failed",
                    _batch.sequence,
                    static_cast<uint32>(entry.queue)
                );
                VulkanDevice& vk_device = queue.vk_device;
                const VkResult result = vk_device.IsFaulted() ?
                                            vk_device.GetFirstFaultResult() :
                                            VK_ERROR_UNKNOWN;
                reject_remaining(
                    source_index + 1, result, "command translation failure"
                );
                return;
            }
            VulkanRuntimeSubmissionResult submit_result{};
            try {
                submit_result = ExecuteOnSubmissionThread(
                    [&queue, packet = &*recorded]() mutable {
                        return queue.SubmitRecordedForRuntime(std::move(*packet));
                    }
                );
            } catch (...) {
                ReportRequestFailure(
                    ERequestKind::Submit,
                    VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                    std::current_exception()
                );
                queue.RejectRecordedForRuntime(
                    std::move(*recorded), VK_ERROR_UNKNOWN
                );
                submit_result.outcome = {
                    EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
                };
            }
            // SubmitRecordedForRuntime owns the recorded packet through its
            // terminal result, including native rejection.
            _exception_state.first_unconsumed_source = source_index + 1;
            if (submit_result.WasSubmitted()) {
                assert(submit_result.completion.has_value());
                update_frontier(
                    entry.queue,
                    *submit_result.completion,
                    async_scope == 0
                );
                used_queues[static_cast<size_t>(entry.queue)] = true;
            } else if (submit_result.IsRecoverableRejection()) {
                reject_remaining_recoverable(
                    source_index + 1,
                    submit_result.outcome.result,
                    "command submission dependency rejection"
                );
                return;
            } else if (submit_result.IsHardFailure()) {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} source_queue={} submission failed "
                    "status={} result={}",
                    _batch.sequence,
                    static_cast<uint32>(entry.queue),
                    static_cast<uint32>(submit_result.outcome.status),
                    static_cast<int32>(submit_result.outcome.result)
                );
                reject_remaining(
                    source_index + 1,
                    submit_result.outcome.result,
                    "command submission hard failure"
                );
                return;
            }
        }
    }

    if (_batch.present) {
        active_async_queue_scope = 0;
        async_scope_entry_frontier.fill(std::nullopt);
        async_scope_seen_queues.fill(false);
        RHIPresentRequest& present = *_batch.present;
        auto& graphics_queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
        );
        CmdSubmit bridge = CommandList(EQueueType::Graphics).Submit();
        append_frontier_waits(bridge, EQueueType::Graphics, gpu_frontier);
        if (!bridge.wait_events.empty()) {
            std::optional<VkCommandQueue::CurrentVulkanRecordedSubmit> recorded =
                graphics_queue.TranslateForRuntime(std::move(bridge));
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
            if (!recorded) {
                const VkResult result = graphics_queue.vk_device.IsFaulted() ?
                                            graphics_queue.vk_device.GetFirstFaultResult() :
                                            VK_ERROR_UNKNOWN;
                reject_remaining(
                    _batch.submits.size(), result, "Present bridge translation failure"
                );
                return;
            }
            VulkanRuntimeSubmissionResult bridge_result{};
            try {
                bridge_result = ExecuteOnSubmissionThread(
                    [&graphics_queue, packet = &*recorded]() mutable {
                        return graphics_queue.SubmitRecordedForRuntime(
                            std::move(*packet)
                        );
                    }
                );
            } catch (...) {
                ReportRequestFailure(
                    ERequestKind::Submit,
                    VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                    std::current_exception()
                );
                graphics_queue.RejectRecordedForRuntime(
                    std::move(*recorded), VK_ERROR_UNKNOWN
                );
                bridge_result.outcome = {
                    EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
                };
            }
            if (!bridge_result.WasSubmitted()) {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} Present bridge failed status={} result={}",
                    _batch.sequence,
                    static_cast<uint32>(bridge_result.outcome.status),
                    static_cast<int32>(bridge_result.outcome.result)
                );
                if (bridge_result.IsRecoverableRejection()) {
                    reject_remaining_recoverable(
                        _batch.submits.size(),
                        bridge_result.outcome.result,
                        "Present bridge dependency rejection"
                    );
                } else {
                    reject_remaining(
                        _batch.submits.size(),
                        bridge_result.outcome.result,
                        "Present bridge submission failure"
                    );
                }
                return;
            }
            assert(bridge_result.completion.has_value());
            update_frontier(
                EQueueType::Graphics, *bridge_result.completion, true
            );
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
        }
        const VulkanRuntimeSubmissionResult present_result = ExecuteOnSubmissionThread(
            [&graphics_queue, present = &present]() mutable {
                return graphics_queue.PresentForRuntime(
                    std::move(present->swapchain),
                    present->source,
                    // Keep the request's receipt copy until the Submission
                    // owner returns so the exception barrier can resolve it.
                    present->receipt
                );
            }
        );
        // PresentForRuntime has now terminalized or retained the copied receipt.
        // Clear the request copy before any later diagnostic/bookkeeping can
        // throw, preserving an accepted/retry Present's existing semantics.
        _batch.present.reset();
        // Retry/Recreate still owns a logical timeline and completion marker;
        // Sync must drain that CPU-side retirement even though no GPU tail was
        // produced.
        used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
        if (present_result.WasSubmitted()) {
            assert(present_result.completion.has_value());
            update_frontier(
                EQueueType::Graphics, *present_result.completion, true
            );
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
        }
        if (present_result.IsHardFailure()) {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] batch={} Present failed status={} result={}",
                _batch.sequence,
                static_cast<uint32>(present_result.outcome.status),
                static_cast<int32>(present_result.outcome.result)
            );
            hard_failed = true;
            hard_failure_result = static_cast<int32>(
                present_result.outcome.result == VK_SUCCESS ?
                    VK_ERROR_UNKNOWN :
                    present_result.outcome.result
            );
        }
        // Retry/Recreate means no copy submit happened.  Keep the previous
        // ordered tail (including a cross-queue bridge) as the stream tail.
    }
}

void VulkanSubmissionExecutor::ProcessSync(ERHISyncDepth _depth) {
    ExecuteOnSubmissionThread([this, _depth] {
        LOG_INFO(
            "[RHIExecutor][Vulkan] sync begin depth={} gfx={} compute={} copy_timeline={}",
            static_cast<uint32>(_depth),
            used_queues[static_cast<size_t>(EQueueType::Graphics)],
            used_queues[static_cast<size_t>(EQueueType::Compute)],
            last_copy_timeline
        );
        // Runtime-side rejections deliberately own no GPU timeline.  Drain
        // every Completion queue by its independent CPU retirement serial so
        // Sync also observes callbacks/fence failure for those packets.
        RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
        RenderDevice::Get().GetCommandQueue(EQueueType::Compute).Sync();
        RenderDevice::Get().GetCopyQueue().Sync(last_copy_timeline);
    });
    gpu_frontier.fill(std::nullopt);
    async_scope_entry_frontier.fill(std::nullopt);
    async_scope_seen_queues.fill(false);
    active_async_queue_scope = 0;
    used_queues.fill(false);
    last_copy_timeline = 0;
    LOG_INFO("[RHIExecutor][Vulkan] sync complete depth={}", static_cast<uint32>(_depth));
}

void VulkanSubmissionExecutor::CompleteRequest(
    const std::shared_ptr<Completion>& _completion
) {
    if (_completion) {
        _completion->Signal();
    }
}

} // namespace Moer::Render
