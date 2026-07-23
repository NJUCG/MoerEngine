#include "VulkanSubmissionExecutor.h"

#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
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

VulkanSubmissionExecutor::VulkanSubmissionExecutor() :
    thread([this] { Run(); }) {
    LOG_INFO("[RHIExecutor][Vulkan] streaming runtime started window=1");
}

VulkanSubmissionExecutor::~VulkanSubmissionExecutor() {
    ShutDown();
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

void VulkanSubmissionExecutor::ShutDown() {
    std::shared_ptr<Completion> completion{};
    bool                        join_owner = false;
    {
        std::lock_guard lock(mutex);
        if (stopped) {
            return;
        }
        accepting = false;
        if (!stop_completion) {
            stop_completion = std::make_shared<Completion>();
            requests.emplace_back(Request{
                .kind       = ERequestKind::Stop,
                .sync_depth = ERHISyncDepth::Present,
                .completion = stop_completion,
            });
            join_owner = true;
        }
        completion = stop_completion;
    }
    cv.notify_one();
    completion->Wait();
    if (join_owner) {
        if (thread.joinable()) {
            thread.join();
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

void VulkanSubmissionExecutor::Run() {
    Platform::SetCurrentThreadName("Moer Vulkan Submit");
    while (true) {
        Request request{};
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] { return !requests.empty(); });
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

        if (ordered_tail_queue.has_value() && *ordered_tail_queue != entry.queue &&
            ordered_gpu_tail.has_value()) {
            // Native queue submit order only applies within one queue. Chain
            // adjacent cross-queue source submissions through their timeline
            // semaphores so the stable topology order is also a GPU order.
            entry.submit.wait_events.emplace_back(*ordered_gpu_tail);
        }
        if (entry.queue == EQueueType::Copy) {
            auto& copy_queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            const VulkanRuntimeSubmissionResult submit_result =
                copy_queue.ExecuteForRuntime(std::move(entry.submit));
            // ExecuteForRuntime has now either submitted or terminalized this
            // source. Never reject it again if later bookkeeping/logging throws.
            _exception_state.first_unconsumed_source = source_index + 1;
            if (submit_result.WasSubmitted()) {
                assert(submit_result.completion.has_value());
                last_copy_timeline = std::max(
                    last_copy_timeline, submit_result.completion->value
                );
                ordered_gpu_tail   = *submit_result.completion;
                ordered_tail_queue = EQueueType::Copy;
                used_queues[static_cast<size_t>(EQueueType::Copy)] = true;
            } else if (submit_result.IsHardFailure()) {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} Copy submission failed status={} result={}",
                    _batch.sequence,
                    static_cast<uint32>(submit_result.outcome.status),
                    static_cast<int32>(submit_result.outcome.result)
                );
                // A failed native Copy submit still publishes a CPU retirement
                // marker. Preserve its logical timeline so a later Sync drains
                // callbacks and allocator quarantine before device teardown.
                last_copy_timeline = std::max(
                    last_copy_timeline, static_cast<uint64>(copy_queue.last_frame)
                );
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
            const VulkanRuntimeSubmissionResult submit_result =
                queue.SubmitRecordedForRuntime(std::move(*recorded));
            // SubmitRecordedForRuntime owns the recorded packet through its
            // terminal result, including native rejection.
            _exception_state.first_unconsumed_source = source_index + 1;
            if (submit_result.WasSubmitted()) {
                assert(submit_result.completion.has_value());
                ordered_gpu_tail   = *submit_result.completion;
                ordered_tail_queue = entry.queue;
                used_queues[static_cast<size_t>(entry.queue)] = true;
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
        RHIPresentRequest& present = *_batch.present;
        auto& graphics_queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
        );
        if (ordered_tail_queue.has_value() &&
            *ordered_tail_queue != EQueueType::Graphics && ordered_gpu_tail.has_value()) {
            CommandList bridge_commands(EQueueType::Graphics);
            CmdSubmit   bridge = bridge_commands.Submit();
            bridge.wait_events.emplace_back(*ordered_gpu_tail);
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
            const VulkanRuntimeSubmissionResult bridge_result =
                graphics_queue.SubmitRecordedForRuntime(std::move(*recorded));
            if (!bridge_result.WasSubmitted()) {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} Present bridge failed status={} result={}",
                    _batch.sequence,
                    static_cast<uint32>(bridge_result.outcome.status),
                    static_cast<int32>(bridge_result.outcome.result)
                );
                reject_remaining(
                    _batch.submits.size(),
                    bridge_result.outcome.result,
                    "Present bridge submission failure"
                );
                return;
            }
            assert(bridge_result.completion.has_value());
            ordered_gpu_tail   = *bridge_result.completion;
            ordered_tail_queue = EQueueType::Graphics;
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
        }
        const VulkanRuntimeSubmissionResult present_result = graphics_queue.PresentForRuntime(
            std::move(present.swapchain),
            present.source,
            // Keep the request's receipt reference until PresentForRuntime
            // returns. If it throws after taking its by-value copy, Run can
            // still resolve this retained receipt through RejectBatch.
            present.receipt
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
            ordered_gpu_tail   = *present_result.completion;
            ordered_tail_queue = EQueueType::Graphics;
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
    LOG_INFO(
        "[RHIExecutor][Vulkan] sync begin depth={} gfx={} compute={} copy_timeline={}",
        static_cast<uint32>(_depth),
        used_queues[static_cast<size_t>(EQueueType::Graphics)],
        used_queues[static_cast<size_t>(EQueueType::Compute)],
        last_copy_timeline
    );
    if (used_queues[static_cast<size_t>(EQueueType::Graphics)] ||
        _depth == ERHISyncDepth::Present) {
        RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
    }
    if (used_queues[static_cast<size_t>(EQueueType::Compute)]) {
        RenderDevice::Get().GetCommandQueue(EQueueType::Compute).Sync();
    }
    if (last_copy_timeline != 0) {
        RenderDevice::Get().GetCopyQueue().Sync(last_copy_timeline);
    }
    ordered_gpu_tail.reset();
    ordered_tail_queue.reset();
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
