#include "VulkanSubmissionExecutor.h"

#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "rhi/RHISubmissionPipelinePolicy.h"
#include "rhi/RHIThreadOwnership.h"
#include "taskgraph/TaskGraph.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanSubmissionDiagnostics.h"
#include "VulkanTranslateWaveScheduler.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace Moer::Render {
namespace {

std::atomic<const VulkanSourceSubmissionObserver*> g_source_submission_observer{nullptr};
std::atomic<const VulkanSourceTranslationObserver*>
    g_source_translation_observer{nullptr};
std::atomic<const VulkanBatchPreflightRejectionObserver*>
    g_batch_preflight_rejection_observer{nullptr};
std::atomic<const VulkanSubmissionBoundaryObserver*>
    g_submission_boundary_observer{nullptr};

struct SubmissionBoundaryIdentity {
    uint64                       batch_sequence{0};
    uint32                       operation_index{0};
    EQueueType                   queue{EQueueType::Ignore};
    EVulkanSubmissionBoundaryKind kind{
        EVulkanSubmissionBoundaryKind::SerialControl
    };
    uint32                       dependency_wait_count{0};
};

void NotifySubmissionBoundary(
    const SubmissionBoundaryIdentity&    _identity,
    EVulkanSubmissionBoundaryPhase       _phase,
    const VulkanRuntimeSubmissionResult* _result = nullptr,
    uint32 _present_receipt_resolution_attempts = 0
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "submission boundary diagnostics must run on the Submission owner"
    );
    const VulkanSubmissionBoundaryObserver* observer =
        g_submission_boundary_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanSubmissionBoundaryEvent{
            .batch_sequence       = _identity.batch_sequence,
            .operation_index      = _identity.operation_index,
            .queue                 = _identity.queue,
            .kind                  = _identity.kind,
            .phase                 = _phase,
            .dependency_wait_count =
                _identity.dependency_wait_count,
            .thread_id             = Platform::GetCurrentThreadID(),
            .thread_role           = GetCurrentRHIThreadRole(),
            .outcome               =
                _result != nullptr ?
                    _result->outcome :
                    VulkanOperationResult{},
            .outcome_valid         = _result != nullptr,
            .gpu_submitted =
                _result != nullptr && _result->WasSubmitted(),
            .recoverable_rejection =
                _result != nullptr &&
                _result->IsRecoverableRejection(),
            .present_receipt_resolution_attempts =
                _present_receipt_resolution_attempts,
        }
    );
}

template<typename SubmitCallable>
VulkanRuntimeSubmissionResult ExecuteObservedSubmissionBoundary(
    const SubmissionBoundaryIdentity& _identity,
    SubmitCallable&&                 _submit,
    const PresentReceiptRef&         _present_receipt = {}
) noexcept {
    NotifySubmissionBoundary(
        _identity,
        EVulkanSubmissionBoundaryPhase::Dispatch
    );
    VulkanRuntimeSubmissionResult result = _submit();
    NotifySubmissionBoundary(
        _identity,
        EVulkanSubmissionBoundaryPhase::Terminal,
        &result,
        _present_receipt ?
            _present_receipt->ResolutionAttemptCount() :
            0
    );
    return result;
}

void NotifySourceSubmitted(
    uint64     _batch_sequence,
    uint32     _source_index,
    uint32     _original_source_index,
    uint32     _source_segment_index,
    uint32     _source_segment_count,
    EQueueType _queue,
    uint64     _async_queue_scope,
    bool       _cross_native_predecessor_wait
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "source submission diagnostics must run on the Submission owner"
    );
    const VulkanSourceSubmissionObserver* observer =
        g_source_submission_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanSourceSubmissionEvent{
            .batch_sequence    = _batch_sequence,
            .source_index      = _source_index,
            .original_source_index         = _original_source_index,
            .source_segment_index          = _source_segment_index,
            .source_segment_count          = _source_segment_count,
            .queue             = _queue,
            .async_queue_scope = _async_queue_scope,
            .cross_native_predecessor_wait = _cross_native_predecessor_wait,
        }
    );
}

void NotifySourceTranslation(
    uint64                        _batch_sequence,
    uint32                        _source_index,
    uint32                        _original_source_index,
    uint32                        _source_segment_index,
    uint32                        _source_segment_count,
    EQueueType                    _queue,
    uint32                        _native_queue_id,
    uint64                        _async_queue_scope,
    EVulkanSourceTranslationPhase _phase
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Translate &&
        "source translation diagnostics must run on a Translate owner"
    );
    const VulkanSourceTranslationObserver* observer =
        g_source_translation_observer.load(
            std::memory_order_acquire
        );
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanSourceTranslationEvent{
            .batch_sequence       = _batch_sequence,
            .source_index         = _source_index,
            .original_source_index = _original_source_index,
            .source_segment_index = _source_segment_index,
            .source_segment_count = _source_segment_count,
            .queue                 = _queue,
            .native_queue_id       = _native_queue_id,
            .async_queue_scope     = _async_queue_scope,
            .thread_id             = Platform::GetCurrentThreadID(),
            .thread_role           = GetCurrentRHIThreadRole(),
            .phase                 = _phase,
        }
    );
}

void NotifyBatchPreflightRejected(
    uint64 _batch_sequence,
    bool   _executable_preflight
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Translate &&
        "batch preflight diagnostics must run on the Translate owner"
    );
    const VulkanBatchPreflightRejectionObserver* observer =
        g_batch_preflight_rejection_observer.load(
            std::memory_order_acquire
        );
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanBatchPreflightRejectionEvent{
            .batch_sequence       = _batch_sequence,
            .thread_id            = Platform::GetCurrentThreadID(),
            .thread_role          = GetCurrentRHIThreadRole(),
            .executable_preflight = _executable_preflight,
        }
    );
}

GraphEventRef MakeCompletedEvent() {
    GraphEventRef event = GraphEvent::CreateGraphEvent();
    event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return event;
}

bool SubmitHasSideEffects(const CmdSubmit& _submit) {
    return !_submit.callbacks.empty() || !_submit.success_callbacks.empty() ||
           !_submit.query_tokens.empty() ||
           !_submit.wait_events.empty() || !_submit.signal_events.empty() || _submit.b_sync ||
           _submit.b_tick_profiling || _submit.b_delete_resources;
}

bool SubmitContainsQuery(const CmdSubmit& _submit) {
    return !_submit.query_tokens.empty() ||
           std::any_of(
               _submit.cmds.begin(),
               _submit.cmds.end(),
               [](const UniquePtr<Command>& _command) {
                   return _command &&
                          _command->Type() == Command::EType::Query;
               }
           );
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

bool IsParallelTranslateCandidate(const RHIBackendSubmissionBatchEntry& _entry) {
    return IsNativeQueue(_entry.queue) &&
           _entry.submit.translate_execution_class == ERHITranslateExecutionClass::Parallel &&
           _entry.submit.HasExplicitResourceStateOwnership() && _entry.submit.async_queue_scope != 0;
}

bool IsSerialControlSource(
    ERHITranslateExecutionClass _execution_class
) noexcept {
    return _execution_class ==
           ERHITranslateExecutionClass::SerialControl;
}

void InvokeSourceCallbacksNoexcept(
    Array<std::function<void()>>& _callbacks,
    std::string_view              _context
) noexcept {
    for (std::function<void()>& callback : _callbacks) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (const std::exception& error) {
            try {
                LOG_ERROR("{} callback threw: {}", _context, error.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR("{} callback threw", _context);
            } catch (...) {
            }
        }
    }
    _callbacks.clear();
}

class VulkanMultiSegmentCompletionState final {
public:
    explicit VulkanMultiSegmentCompletionState(Array<FenceRef>&& _segment_outcomes) :
        segment_outcomes(std::move(_segment_outcomes)),
        retired(segment_outcomes.size(), uint8{0}),
        remaining(segment_outcomes.size()) {}

    void AdoptSourceCallbacks(
        Array<std::function<void()>>&& _callbacks,
        Array<std::function<void()>>&& _success_callbacks
    ) noexcept {
        callbacks         = std::move(_callbacks);
        success_callbacks = std::move(_success_callbacks);
    }

    const FenceRef& SegmentOutcome(size_t _segment_index) const noexcept {
        return segment_outcomes[_segment_index];
    }

    void Retire(size_t _segment_index) noexcept {
        bool segment_success = false;
        if (_segment_index < segment_outcomes.size()) {
            auto* outcome   = ResourceCast(segment_outcomes[_segment_index].Get());
            segment_success = outcome != nullptr && outcome->GetValue() >= 1 && !outcome->IsRejected(1) &&
                              !outcome->IsFailed();
        }

        Array<std::function<void()>> final_callbacks{};
        Array<std::function<void()>> final_success_callbacks{};
        bool                         invoke_success = false;
        {
            std::lock_guard lock(mutex);
            if (_segment_index >= retired.size() || retired[_segment_index] != 0) {
                return;
            }
            retired[_segment_index] = 1;
            all_success             = all_success && segment_success;
            assert(remaining != 0);
            --remaining;
            if (remaining != 0) {
                return;
            }
            invoke_success          = all_success;
            final_callbacks         = std::move(callbacks);
            final_success_callbacks = std::move(success_callbacks);
        }

        InvokeSourceCallbacksNoexcept(final_callbacks, "[RHIExecutor][Vulkan][MultiSegment] ordinary");
        if (invoke_success) {
            InvokeSourceCallbacksNoexcept(
                final_success_callbacks, "[RHIExecutor][Vulkan][MultiSegment] success"
            );
        }
    }

private:
    Array<FenceRef>              segment_outcomes{};
    Array<uint8>                 retired{};
    size_t                       remaining{0};
    bool                         all_success{true};
    std::mutex                   mutex{};
    Array<std::function<void()>> callbacks{};
    Array<std::function<void()>> success_callbacks{};
};

struct VulkanExecutableSourceMetadata {
    uint32 original_source_index{0};
    uint32 source_segment_index{0};
    uint32 source_segment_count{1};
    bool   cross_native_predecessor_wait{false};
};

struct VulkanSegmentMaterializationResult {
    bool                                  changed{false};
    size_t                                original_source_count{0};
    size_t                                cross_native_wait_count{0};
    Array<VulkanExecutableSourceMetadata> sources{};
};

struct VulkanOriginalSourceMaterialization {
    bool                                               multi_segment{false};
    size_t                                             executable_begin{0};
    size_t                                             executable_count{1};
    std::shared_ptr<VulkanMultiSegmentCompletionState> completion{};
};

CmdSubmit MakeEmptySubmit() {
    return CmdSubmit(
        Array<UniquePtr<Command>>{},
        Array<std::function<void()>>{},
        Array<std::function<void()>>{},
        TCachedArgArray{}
    );
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
    if (_batch.present && (!_batch.present->swapchain || _batch.present->source.texture == nullptr)) {
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
            source_plan.source_key.submit_idx != source_index || source_plan.root_queue != entry.queue ||
            source_plan.command_count != submit.cmds.size() ||
            source_plan.execution_class != submit.translate_execution_class ||
            source_plan.has_side_effects != submit_has_work) {
            return RejectPreflight("source plan does not match payload", source_index);
        }
        if (source_plan.segment_plan_begin != expected_segment_begin) {
            return RejectPreflight("non-contiguous source plans", source_index);
        }

        const RHISubmitSegment synthesized_segment{
            .queue = source_plan.root_queue,
            .begin = 0,
            .end   = submit.cmds.size(),
        };
        const std::span<const RHISubmitSegment> source_segments =
            submit.segments.empty() ? std::span<const RHISubmitSegment>(&synthesized_segment, 1) :
                                      std::span<const RHISubmitSegment>(submit.segments);
        const size_t expected_segment_count = submit_has_work ? source_segments.size() : 0;
        if (source_plan.segment_plan_count != expected_segment_count) {
            return RejectPreflight("source retention does not match payload", source_index);
        }

        const bool multi_segment = expected_segment_count > 1;
        if (multi_segment && (!submit.HasExplicitResourceStateOwnership() || submit.async_queue_scope == 0)) {
            return RejectPreflight(
                "multi-segment source requires explicit state and async scope", source_index
            );
        }
        if (multi_segment && SubmitContainsQuery(submit)) {
            // A Phase 17A timestamp pair is allocator-local and must not cross
            // a materialized native command-buffer/queue segment boundary.
            return RejectPreflight(
                "multi-segment source contains timestamp query", source_index
            );
        }
        if (multi_segment && (submit.b_sync || submit.b_delete_resources ||
                              submit.ProfilingPhase() != ERHIProfilingPhase::Disabled)) {
            return RejectPreflight("multi-segment source contains unsupported control payload", source_index);
        }

        std::optional<size_t> runtime_payload_segment{};
        for (size_t segment_index = expected_segment_count; segment_index > 0; --segment_index) {
            if (source_segments[segment_index - 1].queue != EQueueType::Copy) {
                runtime_payload_segment = segment_index - 1;
                break;
            }
        }
        if (!runtime_payload_segment &&
            (!submit.cached_args.empty() || !submit.debug_label.empty() || submit.b_sync ||
             submit.b_delete_resources || submit.ProfilingPhase() != ERHIProfilingPhase::Disabled)) {
            return RejectPreflight("Copy-only source contains unsupported runtime payload", source_index);
        }

        for (size_t segment_index = 0; segment_index < expected_segment_count; ++segment_index) {
            const size_t flat_segment_index = expected_segment_begin + segment_index;
            if (flat_segment_index >= topology.segments.size()) {
                return RejectPreflight("source segment is out of range", source_index);
            }
            const RHISubmitSegment&         descriptor   = source_segments[segment_index];
            const RHISubmissionSegmentPlan& segment_plan = topology.segments[flat_segment_index];
            if (segment_plan.key.op_seq != _batch.sequence ||
                segment_plan.key.submit_idx != flat_segment_index ||
                segment_plan.source_key != source_plan.source_key ||
                segment_plan.source_segment_index != segment_index ||
                segment_plan.segment.queue != descriptor.queue ||
                segment_plan.segment.begin != descriptor.begin ||
                segment_plan.segment.end != descriptor.end ||
                segment_plan.execution_class != source_plan.execution_class ||
                segment_plan.inherit_source_wait_events != (segment_index == 0) ||
                segment_plan.inherit_source_signal_events_and_callbacks !=
                    (segment_index + 1 == expected_segment_count) ||
                segment_plan.inherit_source_runtime_payload !=
                    (runtime_payload_segment.has_value() && *runtime_payload_segment == segment_index)) {
                return RejectPreflight("segment plan does not match source", source_index);
            }
            for (const SubmissionKey& dependency : segment_plan.dependencies) {
                if (dependency.op_seq != _batch.sequence || dependency.submit_idx >= flat_segment_index) {
                    return RejectPreflight(
                        "source segment has an unknown or non-preceding dependency", source_index
                    );
                }
            }

            if (expected_segment_count == 1 &&
                (descriptor.queue != source_plan.root_queue || descriptor.begin != 0 ||
                 descriptor.end != submit.cmds.size())) {
                return RejectPreflight("single segment does not match root queue", source_index);
            }

            for (size_t command_index = descriptor.begin; command_index < descriptor.end; ++command_index) {
                const UniquePtr<Command>& command = submit.cmds[command_index];
                if (!command) {
                    return RejectPreflight("null command", source_index, command_index);
                }
                if (command->Type() >= Command::EType::Count) {
                    return RejectPreflight("invalid command type", source_index, command_index);
                }
                if (descriptor.queue == EQueueType::Copy && !IsCopyCommand(command->Type())) {
                    return RejectPreflight(
                        "unsupported command on Copy segment", source_index, command_index
                    );
                }
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

        if (expected_segment_count == 1 && source_segments.front().queue == EQueueType::Copy) {
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

VulkanSegmentMaterializationResult
MaterializeMultiSegmentBatch(RHIBackendSubmissionBatch& _batch, const RHIQueueTopology& _queue_topology) {
    VulkanSegmentMaterializationResult result{};
    result.original_source_count = _batch.submits.size();

    size_t executable_source_count = 0;
    for (const RHISourceSubmitPlan& source_plan : _batch.topology.source_plans) {
        result.changed              = result.changed || source_plan.segment_plan_count > 1;
        const size_t retained_count = source_plan.segment_plan_count > 1 ? source_plan.segment_plan_count : 1;
        if (retained_count > std::numeric_limits<size_t>::max() - executable_source_count) {
            throw std::runtime_error("multi-segment materialization source count overflow");
        }
        executable_source_count += retained_count;
    }

    if (executable_source_count > std::numeric_limits<uint32>::max()) {
        throw std::runtime_error("multi-segment materialization exceeds SubmissionKey capacity");
    }

    result.sources.reserve(executable_source_count);
    if (!result.changed) {
        for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
            result.sources.emplace_back(VulkanExecutableSourceMetadata{
                .original_source_index = static_cast<uint32>(source_index),
                .source_segment_index  = 0,
                .source_segment_count  = 1,
            });
        }
        return result;
    }

    Array<VulkanOriginalSourceMaterialization> source_materializations(_batch.submits.size());
    Array<RHIBackendSubmissionBatchEntry>      executable_sources{};
    executable_sources.reserve(executable_source_count);

    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        RHIBackendSubmissionBatchEntry& source_entry  = _batch.submits[source_index];
        CmdSubmit&                      source_submit = source_entry.submit;
        const RHISourceSubmitPlan&      source_plan   = _batch.topology.source_plans[source_index];

        VulkanOriginalSourceMaterialization& materialization = source_materializations[source_index];
        materialization.multi_segment                        = source_plan.segment_plan_count > 1;
        materialization.executable_begin                     = executable_sources.size();
        materialization.executable_count = materialization.multi_segment ? source_plan.segment_plan_count : 1;

        if (!materialization.multi_segment) {
            executable_sources.emplace_back(source_entry.queue, MakeEmptySubmit());
            result.sources.emplace_back(VulkanExecutableSourceMetadata{
                .original_source_index = static_cast<uint32>(source_index),
                .source_segment_index  = 0,
                .source_segment_count  = 1,
            });
            continue;
        }

        Array<FenceRef> segment_outcomes{};
        segment_outcomes.reserve(source_plan.segment_plan_count);
        for (size_t segment_index = 0; segment_index < source_plan.segment_plan_count; ++segment_index) {
            FenceRef outcome = RenderDevice::Get().CreateFence();
            if (!outcome.IsValid()) {
                throw std::runtime_error("multi-segment materialization failed to create completion fence");
            }
            segment_outcomes.emplace_back(std::move(outcome));
        }
        materialization.completion =
            std::make_shared<VulkanMultiSegmentCompletionState>(std::move(segment_outcomes));

        for (size_t segment_index = 0; segment_index < source_plan.segment_plan_count; ++segment_index) {
            const RHISubmissionSegmentPlan& segment_plan =
                _batch.topology.segments[source_plan.segment_plan_begin + segment_index];
            const RHISubmitSegment& descriptor    = segment_plan.segment;
            const size_t            command_count = descriptor.end - descriptor.begin;

            CmdSubmit segment_submit = MakeEmptySubmit();
            segment_submit.cmds.reserve(command_count);
            segment_submit.callbacks.reserve(1);
            segment_submit.signal_events.reserve(
                1 + (segment_plan.inherit_source_signal_events_and_callbacks ?
                         source_submit.signal_events.size() :
                         0)
            );
            segment_submit.wait_events.reserve(
                (segment_plan.inherit_source_wait_events ? source_submit.wait_events.size() : 0) + 1
            );
            segment_submit.segments.reserve(1);

            segment_submit.resource_state_ownership  = source_submit.resource_state_ownership;
            segment_submit.translate_execution_class = source_submit.translate_execution_class;
            segment_submit.async_queue_scope         = source_submit.async_queue_scope;
            segment_submit.debug_label_color         = source_submit.debug_label_color;

            if (descriptor.queue != EQueueType::Copy) {
                segment_submit.cached_args = source_submit.cached_args;
            }
            if (segment_plan.inherit_source_runtime_payload) {
                segment_submit.debug_label = source_submit.debug_label;
            }

            segment_submit.segments.emplace_back(RHISubmitSegment{
                .queue = descriptor.queue,
                .begin = 0,
                .end   = command_count,
            });

            const FenceRef& segment_outcome = materialization.completion->SegmentOutcome(segment_index);
            segment_submit.signal_events.emplace_back(SignalEvent{
                .timeline_handle = uint64(segment_outcome.Get()),
                .value           = 1,
            });
            segment_submit.callbacks.emplace_back([completion = materialization.completion, segment_index] {
                completion->Retire(segment_index);
            });

            bool cross_native_predecessor_wait = false;
            if (segment_index != 0) {
                const RHISubmissionSegmentPlan& predecessor_plan =
                    _batch.topology.segments[source_plan.segment_plan_begin + segment_index - 1];
                cross_native_predecessor_wait =
                    _queue_topology.Resolve(predecessor_plan.segment.queue).native_queue_id !=
                    _queue_topology.Resolve(descriptor.queue).native_queue_id;
                if (cross_native_predecessor_wait) {
                    const FenceRef& predecessor_outcome =
                        materialization.completion->SegmentOutcome(segment_index - 1);
                    segment_submit.wait_events.emplace_back(WaitEvent{
                        .timeline_handle = uint64(predecessor_outcome.Get()),
                        .value           = 1,
                    });
                    ++result.cross_native_wait_count;
                }
            }

            executable_sources.emplace_back(descriptor.queue, std::move(segment_submit));
            result.sources.emplace_back(VulkanExecutableSourceMetadata{
                .original_source_index         = static_cast<uint32>(source_index),
                .source_segment_index          = static_cast<uint32>(segment_index),
                .source_segment_count          = static_cast<uint32>(source_plan.segment_plan_count),
                .cross_native_predecessor_wait = cross_native_predecessor_wait,
            });
        }
    }

    assert(executable_sources.size() == executable_source_count);
    assert(result.sources.size() == executable_source_count);

    // Build and validate the flattened immutable topology before moving any
    // command, callback, or external synchronization ownership out of the
    // original sources. Every operation after this point is pre-reserved and
    // noexcept for the owned payload types.
    Array<RHISourceSubmitDescription> descriptions{};
    descriptions.reserve(executable_source_count);
    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        const RHIBackendSubmissionBatchEntry&      source_entry    = _batch.submits[source_index];
        const VulkanOriginalSourceMaterialization& materialization = source_materializations[source_index];
        if (!materialization.multi_segment) {
            descriptions.emplace_back(RHISourceSubmitDescription{
                .root_queue      = source_entry.queue,
                .command_count   = source_entry.submit.cmds.size(),
                .segments        = source_entry.submit.segments,
                .execution_class = source_entry.submit.translate_execution_class,
                .has_side_effects =
                    SubmitHasSideEffects(source_entry.submit) || !source_entry.submit.cmds.empty(),
            });
            continue;
        }

        for (size_t segment_index = 0; segment_index < materialization.executable_count; ++segment_index) {
            const RHIBackendSubmissionBatchEntry& executable_entry =
                executable_sources[materialization.executable_begin + segment_index];
            descriptions.emplace_back(RHISourceSubmitDescription{
                .root_queue       = executable_entry.queue,
                .command_count    = executable_entry.submit.segments.front().end,
                .segments         = executable_entry.submit.segments,
                .execution_class  = executable_entry.submit.translate_execution_class,
                .has_side_effects = true,
            });
        }
    }

    RHISubmissionTopologyPlan executable_topology = BuildRHISubmissionTopology(descriptions, _batch.sequence);
    if (!executable_topology.IsValid() ||
        executable_topology.source_plans.size() != executable_sources.size()) {
        throw std::runtime_error("multi-segment materialization produced an invalid executable topology");
    }

    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        RHIBackendSubmissionBatchEntry&      source_entry    = _batch.submits[source_index];
        VulkanOriginalSourceMaterialization& materialization = source_materializations[source_index];
        if (!materialization.multi_segment) {
            executable_sources[materialization.executable_begin].submit = std::move(source_entry.submit);
            continue;
        }

        materialization.completion->AdoptSourceCallbacks(
            std::move(source_entry.submit.callbacks), std::move(source_entry.submit.success_callbacks)
        );

        const RHISourceSubmitPlan& source_plan = _batch.topology.source_plans[source_index];
        for (size_t segment_index = 0; segment_index < materialization.executable_count; ++segment_index) {
            const RHISubmitSegment& descriptor =
                _batch.topology.segments[source_plan.segment_plan_begin + segment_index].segment;
            CmdSubmit& executable_submit =
                executable_sources[materialization.executable_begin + segment_index].submit;
            for (size_t command_index = descriptor.begin; command_index < descriptor.end; ++command_index) {
                executable_submit.cmds.emplace_back(std::move(source_entry.submit.cmds[command_index]));
            }
        }

        CmdSubmit& first_submit = executable_sources[materialization.executable_begin].submit;
        for (const WaitEvent& wait : source_entry.submit.wait_events) {
            first_submit.wait_events.emplace_back(wait);
        }

        CmdSubmit& last_submit =
            executable_sources[materialization.executable_begin + materialization.executable_count - 1]
                .submit;
        for (const SignalEvent& signal : source_entry.submit.signal_events) {
            last_submit.signal_events.emplace_back(signal);
        }
        // Signal ownership has moved to the last executable segment. Leaving
        // the original handles live would make CmdSubmit::~CmdSubmit reject
        // values that have already been accepted by the native queue.
        source_entry.submit.signal_events.clear();
    }

    _batch.submits  = std::move(executable_sources);
    _batch.topology = std::move(executable_topology);
    return result;
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

VulkanMultiSegmentCompletionProbeResult RunVulkanMultiSegmentCompletionProbeForTesting() {
    Array<FenceRef> segment_outcomes{};
    segment_outcomes.reserve(2);
    segment_outcomes.emplace_back(RenderDevice::Get().CreateFence());
    segment_outcomes.emplace_back(RenderDevice::Get().CreateFence());
    if (!segment_outcomes[0].IsValid() || !segment_outcomes[1].IsValid()) {
        throw std::runtime_error("multi-segment completion probe failed to create fences");
    }

    auto   completion = std::make_shared<VulkanMultiSegmentCompletionState>(std::move(segment_outcomes));
    uint32 ordinary_callback_count = 0;
    uint32 success_callback_count  = 0;
    completion->AdoptSourceCallbacks(
        Array<std::function<void()>>{[&ordinary_callback_count] {
            ++ordinary_callback_count;
        }},
        Array<std::function<void()>>{[&success_callback_count] {
            ++success_callback_count;
        }}
    );

    VulkanMultiSegmentCompletionProbeResult result{};
    auto*                                   suffix = ResourceCast(completion->SegmentOutcome(1).Get());
    auto*                                   prefix = ResourceCast(completion->SegmentOutcome(0).Get());
    if (suffix == nullptr || prefix == nullptr) {
        throw std::runtime_error("multi-segment completion probe received non-Vulkan fences");
    }

    suffix->Fail(VK_ERROR_UNKNOWN);
    completion->Retire(1);
    result.suffix_retirement_deferred_callbacks = ordinary_callback_count == 0 && success_callback_count == 0;

    prefix->Notify(1);
    completion->Retire(0);
    result.prefix_retirement_completed_callbacks =
        ordinary_callback_count == 1 && success_callback_count == 0;

    completion->Retire(1);
    completion->Retire(0);
    result.repeated_retirement_suppressed = ordinary_callback_count == 1 && success_callback_count == 0;
    result.ordinary_callback_count        = ordinary_callback_count;
    result.success_callback_count         = success_callback_count;
    return result;
}

bool TryInstallVulkanSourceSubmissionObserver(const VulkanSourceSubmissionObserver* _observer) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const VulkanSourceSubmissionObserver* expected = nullptr;
    return g_source_submission_observer.compare_exchange_strong(
        expected, _observer, std::memory_order_release, std::memory_order_relaxed
    );
}

bool RemoveVulkanSourceSubmissionObserver(const VulkanSourceSubmissionObserver* _observer) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const VulkanSourceSubmissionObserver* expected = _observer;
    return g_source_submission_observer.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire
    );
}

bool TryInstallVulkanSubmissionBoundaryObserver(
    const VulkanSubmissionBoundaryObserver* _observer
) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const VulkanSubmissionBoundaryObserver* expected = nullptr;
    return g_submission_boundary_observer.compare_exchange_strong(
        expected,
        _observer,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanSubmissionBoundaryObserver(
    const VulkanSubmissionBoundaryObserver* _observer
) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const VulkanSubmissionBoundaryObserver* expected = _observer;
    return g_submission_boundary_observer.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool TryInstallVulkanSourceTranslationObserver(
    const VulkanSourceTranslationObserver* _observer
) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const VulkanSourceTranslationObserver* expected = nullptr;
    return g_source_translation_observer.compare_exchange_strong(
        expected,
        _observer,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanSourceTranslationObserver(
    const VulkanSourceTranslationObserver* _observer
) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const VulkanSourceTranslationObserver* expected = _observer;
    return g_source_translation_observer.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool TryInstallVulkanBatchPreflightRejectionObserver(
    const VulkanBatchPreflightRejectionObserver* _observer
) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const VulkanBatchPreflightRejectionObserver* expected = nullptr;
    return g_batch_preflight_rejection_observer.compare_exchange_strong(
        expected,
        _observer,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanBatchPreflightRejectionObserver(
    const VulkanBatchPreflightRejectionObserver* _observer
) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const VulkanBatchPreflightRejectionObserver* expected = _observer;
    return g_batch_preflight_rejection_observer.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

VulkanSubmissionExecutor::VulkanSubmissionExecutor(uint32 _batch_window) :
    batch_window(
        RHISubmissionPipelinePolicy::ClampBatchWindow(_batch_window)
    ) {
    const size_t configured_batch_window = batch_window;
    const RHIQueueTopology queue_topology =
        RenderDevice::Get().GetQueueTopology();
    runtime_queues_share_native_lane =
        RHISubmissionPipelinePolicy::HasAvailableNativeLaneAlias(
            queue_topology
        );
    // Logical queue wrappers own independent transferable gates. Keep one
    // batch in flight whenever any available Graphics/Compute/Copy pair
    // aliases so those gates cannot admit two packets for one native lane.
    batch_window =
        RHISubmissionPipelinePolicy::ResolveEffectiveBatchWindow(
            _batch_window, queue_topology
        );

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
    bool copy_claimed     = false;
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
        copy_claimed = copy_queue.ClaimRuntimeOwnership();
        if (!copy_claimed) {
            throw std::runtime_error(
                "Vulkan Copy queue ownership conflicts with legacy execution"
            );
        }
        claims_owned = true;

        submission_thread = std::jthread([this] { RunSubmission(); });
        executor_thread   = std::jthread([this] { RunExecutor(); });
        LOG_INFO(
            "[RHIExecutor][Vulkan] ownership runtime started "
            "Executor=1 TranslateCoordinator=1 TranslateWorkers=TaskGraph "
            "Submission=1 configured_batch_window={} batch_window={} "
            "runtime_native_alias={}",
            configured_batch_window,
            batch_window,
            runtime_queues_share_native_lane
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
        if (copy_claimed) {
            copy_queue.ReleaseRuntimeOwnership();
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
        auto& copy_queue =
            static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
        copy_queue.ReleaseRuntimeOwnership();
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

VulkanSubmissionExecutor::BatchRejectionPublication::
    BatchRejectionPublication(RHIBackendSubmissionBatch& _batch) {
    sources.reserve(_batch.submits.size());
    const QueryPublishBatch common_query_batch =
        QueryBackendAccess::BeginPublishBatch();

    for (RHIBackendSubmissionBatchEntry& entry : _batch.submits) {
        RejectionSourceSnapshot source{};
        source.signals.reserve(entry.submit.signal_events.size());
        for (const SignalEvent& signal : entry.submit.signal_events) {
            auto* fence = reinterpret_cast<Fence*>(signal.timeline_handle);
            if (fence != nullptr) {
                source.signals.emplace_back(RejectionSignalHandle{
                    .fence = FenceRef(fence),
                    .value = signal.value,
                });
            }
        }

        source.query_tokens = entry.submit.query_tokens;
        if (!source.query_tokens.empty()) {
            source.query_batch =
                entry.submit.query_publish_batch.Valid() ?
                    entry.submit.query_publish_batch :
                    common_query_batch;
            // Install the notification ticket before the CmdSubmit can move
            // into either a recorded packet or a per-queue Completion packet.
            entry.submit.query_publish_batch = source.query_batch;
        }
        sources.emplace_back(std::move(source));
    }
}

void VulkanSubmissionExecutor::BatchRejectionPublication::PublishSuffix(
    size_t           _reject_from,
    int32            _result,
    bool             _recoverable,
    std::string_view _reason
) noexcept {
    std::lock_guard lock(mutex);
    const size_t begin = std::min(_reject_from, sources.size());
    const VkResult result =
        _result == static_cast<int32>(VK_SUCCESS) ?
            VK_ERROR_UNKNOWN :
            static_cast<VkResult>(_result);

    // Every signal in the suffix is terminal before any Query state is
    // published. User Query callbacks remain deferred to their queue
    // Completion packets, but Wait/Get can already observe the whole suffix.
    for (size_t source_index = begin;
         source_index < sources.size();
         ++source_index) {
        RejectionSourceSnapshot& source = sources[source_index];
        if (source.terminalized) {
            continue;
        }
        for (const RejectionSignalHandle& signal : source.signals) {
            auto* fence =
                reinterpret_cast<VulkanFence*>(signal.fence.Get());
            if (fence == nullptr) {
                continue;
            }
            if (_recoverable) {
                fence->Reject(signal.value);
                continue;
            }
            try {
                fence->Fail(result);
            } catch (...) {
                // Preserve terminal progress even if platform synchronization
                // bookkeeping itself cannot publish the richer hard-fault
                // classification.
                fence->Reject(signal.value);
            }
        }
    }

    const std::string_view reason =
        _reason.empty() ?
            "Vulkan submission suffix was rejected before GPU completion" :
            _reason;
    for (size_t source_index = begin;
         source_index < sources.size();
         ++source_index) {
        RejectionSourceSnapshot& source = sources[source_index];
        if (source.terminalized) {
            continue;
        }
        if (source.query_batch.Valid()) {
            QueryBackendAccess::PublishErrorsIfPending(
                source.query_tokens, reason, source.query_batch
            );
        }
    }
    for (size_t source_index = begin;
         source_index < sources.size();
         ++source_index) {
        sources[source_index].terminalized = true;
    }
}

void VulkanSubmissionExecutor::PublishRuntimeFailureBeforeCompletion(
    void*                                _context,
    const VulkanRuntimeSubmissionResult& _result
) noexcept {
    if (_result.WasSubmitted() || _context == nullptr) {
        return;
    }
    auto& publication =
        *static_cast<RuntimePreCompletionPublication*>(_context);
    if (!publication.rejection_publication) {
        return;
    }
    const int32 result =
        _result.outcome.result == VK_SUCCESS ?
            static_cast<int32>(VK_ERROR_UNKNOWN) :
            static_cast<int32>(_result.outcome.result);
    const bool recoverable = _result.IsRecoverableRejection();
    publication.rejection_publication->PublishSuffix(
        publication.reject_from,
        result,
        recoverable,
        recoverable ?
            "Vulkan submission suffix was rejected before Completion notification" :
            "Vulkan submission suffix failed before Completion notification"
    );
}

VulkanSubmissionExecutor::PipelineBatchState::PipelineBatchState(
    uint64                      _sequence,
    size_t                      _source_count,
    std::shared_ptr<Completion> _completion,
    std::shared_ptr<BatchRejectionPublication>
        _rejection_publication
) :
    sequence(_sequence),
    slots(_source_count),
    completion(std::move(_completion)),
    rejection_publication(std::move(_rejection_publication)) {}

void VulkanSubmissionExecutor::PipelineBatchState::AddWork() noexcept {
    work_state.AddWork();
}

void VulkanSubmissionExecutor::PipelineBatchState::FinishWork() noexcept {
    if (!work_state.FinishWork()) {
        return;
    }
    bool expected = false;
    if (completion_signalled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
        completion->Signal();
    }
}

void VulkanSubmissionExecutor::PipelineBatchState::Seal() noexcept {
    if (!work_state.Seal()) {
        return;
    }
    bool expected = false;
    if (completion_signalled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
        completion->Signal();
    }
}

void VulkanSubmissionExecutor::PipelineBatchState::PublishFailure(
    size_t _reject_from,
    int32  _result,
    bool   _recoverable
) noexcept {
    bool published_earlier_failure = false;
    {
        std::lock_guard lock(failure_mutex);
        if (_reject_from >= failure.reject_from) {
            return;
        }
        failure.reject_from = _reject_from;
        failure.result =
            _result == static_cast<int32>(VK_SUCCESS) ?
                static_cast<int32>(VK_ERROR_UNKNOWN) :
                _result;
        failure.recoverable = _recoverable;
        published_earlier_failure = true;
    }
    if (published_earlier_failure && rejection_publication) {
        rejection_publication->PublishSuffix(
            _reject_from,
            _result,
            _recoverable,
            _recoverable ?
                "Vulkan submission pipeline suffix was rejected by a dependency" :
                "Vulkan submission pipeline suffix was rejected after a hard failure"
        );
    }
}

VulkanSubmissionExecutor::PipelineFailure
VulkanSubmissionExecutor::PipelineBatchState::ReadFailure() const noexcept {
    std::lock_guard lock(failure_mutex);
    return failure;
}

void VulkanSubmissionExecutor::Enqueue(RHIBackendSubmissionBatch&& _batch) {
    static_assert(
        std::is_nothrow_move_assignable_v<RHIBackendSubmissionBatch>,
        "backend FIFO publication requires a nonthrowing batch handoff"
    );
    bool rejected = false;
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            rejected = true;
        } else {
            // Allocate the deque slot before moving the batch. If allocation
            // throws, RHIExecutor still owns an intact batch and can reject
            // every signal/receipt. The subsequent assignment is statically
            // required to be nonthrowing.
            requests.emplace_back();
            Request& request = requests.back();
            request.kind     = ERequestKind::Submit;
            request.batch    = std::move(_batch);
        }
    }
    if (rejected) {
        // User completion callbacks must never run while the backend FIFO lock
        // is held: they may enqueue more RHI work or acquire unrelated locks.
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(VK_ERROR_UNKNOWN),
            "submission runtime is stopping or stopped",
            true
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
    NotifyVulkanBackendSyncWait();
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
                        DrainPipelineBatches();
                        ProcessSync(request.sync_depth);
                    }
                },
                [&] {
                    // ProcessBatch may throw before it reaches the ordinary
                    // pipeline admission/drain decision. Preserve the
                    // already-admitted stream prefix before publishing any
                    // rejection markers for this request.
                    DrainPipelineBatches();
                    LatchHardFailure(
                        StreamPosition{
                            request.batch.sequence,
                            exception_state.first_unconsumed_source,
                        },
                        static_cast<int32>(VK_ERROR_UNKNOWN)
                    );
                    RejectBatch(
                        std::move(request.batch),
                        static_cast<int32>(VK_ERROR_UNKNOWN),
                        "unhandled submission request exception",
                        false,
                        exception_state.first_unconsumed_source,
                        &exception_state.first_unconsumed_source
                    );
                },
                [&] { CompleteRequest(request.completion); },
                [&](VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
                    const std::exception_ptr& _exception) {
                    LatchHardFailure(
                        StreamPosition{
                            request.batch.sequence,
                            exception_state.first_unconsumed_source,
                        },
                        static_cast<int32>(VK_ERROR_UNKNOWN)
                    );
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

void VulkanSubmissionExecutor::WaitForPipelineCapacity() {
    while (in_flight_batches.size() >= batch_window) {
        std::shared_ptr<Completion> completion =
            std::move(in_flight_batches.front());
        in_flight_batches.pop_front();
        completion->Wait();
    }
}

void VulkanSubmissionExecutor::DrainPipelineBatches() {
    assert(
        GetCurrentRHIThreadRole() != ERHIThreadRole::Submission &&
        "Submission owner must not wait on its own pipeline work"
    );
    while (!in_flight_batches.empty()) {
        std::shared_ptr<Completion> completion =
            std::move(in_flight_batches.front());
        in_flight_batches.pop_front();
        completion->Wait();
    }
}

void VulkanSubmissionExecutor::ResetStreamScope() noexcept {
    active_async_queue_scope = 0;
    async_scope_entry_frontier.fill(std::nullopt);
    async_scope_seen_queues.fill(false);
}

void VulkanSubmissionExecutor::PrepareStreamSubmit(
    CmdSubmit& _submit,
    EQueueType _queue
) {
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
        [&](const auto& frontier) {
            for (size_t index = 0; index < frontier.size(); ++index) {
                if (!frontier[index].has_value()) {
                    continue;
                }
                const auto source_queue = static_cast<EQueueType>(index);
                if (same_native_queue(source_queue, _queue)) {
                    continue;
                }
                append_unique_wait(_submit, *frontier[index]);
            }
        };
    auto mark_scope_queue_seen = [&] {
        for (size_t index = 0; index < async_scope_seen_queues.size();
             ++index) {
            if (same_native_queue(static_cast<EQueueType>(index), _queue)) {
                async_scope_seen_queues[index] = true;
            }
        }
    };

    const uint64 async_scope = _submit.async_queue_scope;
    if (async_scope == 0) {
        ResetStreamScope();
        append_frontier_waits(gpu_frontier);
        return;
    }

    if (active_async_queue_scope != async_scope) {
        active_async_queue_scope   = async_scope;
        async_scope_entry_frontier = gpu_frontier;
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

    const size_t queue_index = static_cast<size_t>(_queue);
    if (!async_scope_seen_queues[queue_index]) {
        append_frontier_waits(async_scope_entry_frontier);
        mark_scope_queue_seen();
    }
}

void VulkanSubmissionExecutor::UpdateStreamFrontier(
    EQueueType _queue,
    WaitEvent  _completion,
    bool       _collapse
) {
    const RHIQueueTopology queue_topology =
        RenderDevice::Get().GetQueueTopology();
    auto same_native_queue = [&](EQueueType lhs, EQueueType rhs) {
        return queue_topology.Resolve(lhs).native_queue_id ==
               queue_topology.Resolve(rhs).native_queue_id;
    };

    if (_collapse) {
        gpu_frontier.fill(std::nullopt);
    } else {
        for (size_t index = 0; index < gpu_frontier.size(); ++index) {
            if (same_native_queue(static_cast<EQueueType>(index), _queue)) {
                gpu_frontier[index].reset();
            }
        }
    }
    gpu_frontier[static_cast<size_t>(_queue)] = _completion;
}

void VulkanSubmissionExecutor::LatchHardFailure(
    StreamPosition _position,
    int32          _result
) noexcept {
    std::lock_guard lock(hard_failure_mutex);
    const bool earlier =
        !hard_failure_position.has_value() ||
        _position.batch_sequence < hard_failure_position->batch_sequence ||
        (_position.batch_sequence == hard_failure_position->batch_sequence &&
         _position.source_index < hard_failure_position->source_index);
    if (!earlier) {
        return;
    }
    hard_failure_position = _position;
    hard_failure_result =
        _result == static_cast<int32>(VK_SUCCESS) ?
            static_cast<int32>(VK_ERROR_UNKNOWN) :
            _result;
}

bool VulkanSubmissionExecutor::IsHardFailureAtOrBefore(
    StreamPosition _position,
    int32*         _result
) const noexcept {
    std::lock_guard lock(hard_failure_mutex);
    if (!hard_failure_position.has_value()) {
        return false;
    }
    const bool rejected =
        hard_failure_position->batch_sequence < _position.batch_sequence ||
        (hard_failure_position->batch_sequence == _position.batch_sequence &&
         hard_failure_position->source_index <= _position.source_index);
    if (rejected && _result != nullptr) {
        *_result = hard_failure_result;
    }
    return rejected;
}

bool VulkanSubmissionExecutor::HasRecordedSourcePacket(
    const RecordedSourcePacket& _packet
) noexcept {
    return !std::holds_alternative<std::monostate>(_packet);
}

CmdSubmit* VulkanSubmissionExecutor::GetRecordedSourceSubmit(
    RecordedSourcePacket& _packet
) noexcept {
    if (auto* packet =
            std::get_if<VkCommandQueue::CurrentVulkanRecordedSubmit>(
                &_packet
            )) {
        return &packet->submit;
    }
    if (auto* packet =
            std::get_if<VkCopyQueue::CurrentVulkanCopyRecordedSubmit>(
                &_packet
            )) {
        return &packet->submit;
    }
    return nullptr;
}

VulkanSubmissionExecutor::RecordedSourcePacket
VulkanSubmissionExecutor::TranslateSourceForRuntime(
    EQueueType _queue,
    CmdSubmit&& _submit
) noexcept {
    if (_queue == EQueueType::Copy) {
        auto& copy_queue =
            static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
        auto recorded =
            copy_queue.TranslateForRuntime(std::move(_submit));
        if (recorded) {
            return RecordedSourcePacket{
                std::in_place_type<
                    VkCopyQueue::CurrentVulkanCopyRecordedSubmit>,
                std::move(*recorded)
            };
        }
        return {};
    }

    if (_queue == EQueueType::Graphics ||
        _queue == EQueueType::Compute) {
        auto& queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(_queue)
        );
        auto recorded =
            queue.TranslateForRuntime(std::move(_submit));
        if (recorded) {
            return RecordedSourcePacket{
                std::in_place_type<
                    VkCommandQueue::CurrentVulkanRecordedSubmit>,
                std::move(*recorded)
            };
        }
        return {};
    }

    RejectSourceForRuntime(
        _queue, std::move(_submit), VK_ERROR_UNKNOWN, false
    );
    return {};
}

VulkanRuntimeSubmissionResult
VulkanSubmissionExecutor::SubmitRecordedSourceForRuntime(
    EQueueType                            _queue,
    RecordedSourcePacket&&                _packet,
    const VulkanRuntimePreCompletionHook* _pre_completion
) noexcept {
    if (_queue == EQueueType::Copy) {
        if (auto* packet =
                std::get_if<
                    VkCopyQueue::CurrentVulkanCopyRecordedSubmit>(
                    &_packet
                )) {
            auto& copy_queue =
                static_cast<VkCopyQueue&>(
                    RenderDevice::Get().GetCopyQueue()
            );
            return copy_queue.SubmitRecordedForRuntime(
                std::move(*packet), _pre_completion
            );
        }
    } else if (
        _queue == EQueueType::Graphics ||
        _queue == EQueueType::Compute
    ) {
        if (auto* packet =
                std::get_if<
                    VkCommandQueue::CurrentVulkanRecordedSubmit>(
                    &_packet
                )) {
            auto& queue = static_cast<VkCommandQueue&>(
                RenderDevice::Get().GetCommandQueue(_queue)
            );
            return queue.SubmitRecordedForRuntime(
                std::move(*packet), _pre_completion
            );
        }
    }

    const VulkanRuntimeSubmissionResult rejected{
        .outcome = {
            EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
        },
    };
    if (_pre_completion != nullptr &&
        _pre_completion->callback != nullptr) {
        _pre_completion->callback(
            _pre_completion->context, rejected
        );
    }
    RejectRecordedSourceForRuntime(
        _queue, std::move(_packet), VK_ERROR_UNKNOWN, false
    );
    return rejected;
}

void VulkanSubmissionExecutor::RejectRecordedSourceForRuntime(
    EQueueType             _queue,
    RecordedSourcePacket&& _packet,
    VkResult               _result,
    bool                   _recoverable
) noexcept {
    if (_result == VK_SUCCESS) {
        _result = VK_ERROR_UNKNOWN;
    }
    if (auto* packet =
            std::get_if<VkCopyQueue::CurrentVulkanCopyRecordedSubmit>(
                &_packet
            )) {
        assert(
            _queue == EQueueType::Copy &&
            "Copy recorded packet queue tag mismatch"
        );
        auto& copy_queue =
            static_cast<VkCopyQueue&>(
                RenderDevice::Get().GetCopyQueue()
            );
        copy_queue.RejectRecordedForRuntime(
            std::move(*packet), _result, _recoverable
        );
        return;
    }
    if (auto* packet =
            std::get_if<VkCommandQueue::CurrentVulkanRecordedSubmit>(
                &_packet
            )) {
        EQueueType packet_queue = packet->context.queue_type;
        if (packet_queue != EQueueType::Graphics &&
            packet_queue != EQueueType::Compute) {
            packet_queue = _queue;
        }
        assert(
            packet_queue == _queue &&
            "command recorded packet queue tag mismatch"
        );
        auto& queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(packet_queue)
        );
        queue.RejectRecordedForRuntime(
            std::move(*packet), _result, _recoverable
        );
    }
}

void VulkanSubmissionExecutor::RejectSourceForRuntime(
    EQueueType _queue,
    CmdSubmit&& _submit,
    VkResult    _result,
    bool        _recoverable
) noexcept {
    if (_queue == EQueueType::Copy) {
        auto& copy_queue =
            static_cast<VkCopyQueue&>(
                RenderDevice::Get().GetCopyQueue()
            );
        copy_queue.RejectForRuntime(
            std::move(_submit), _result, _recoverable
        );
        return;
    }
    if (_queue == EQueueType::Graphics ||
        _queue == EQueueType::Compute) {
        auto& queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(_queue)
        );
        queue.RejectForRuntime(
            std::move(_submit), _result, _recoverable
        );
        return;
    }

    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] cannot terminalize unsupported queue type={}",
            static_cast<uint32>(_queue)
        );
    } catch (...) {
    }
}

VkResult VulkanSubmissionExecutor::GetQueueFaultResult(
    EQueueType _queue
) const noexcept {
    if (_queue == EQueueType::Copy) {
        auto& copy_queue =
            static_cast<VkCopyQueue&>(
                RenderDevice::Get().GetCopyQueue()
            );
        return copy_queue.device.IsFaulted() ?
                   copy_queue.device.GetFirstFaultResult() :
                   VK_ERROR_UNKNOWN;
    }
    if (_queue == EQueueType::Graphics ||
        _queue == EQueueType::Compute) {
        auto& queue = static_cast<VkCommandQueue&>(
            RenderDevice::Get().GetCommandQueue(_queue)
        );
        return queue.vk_device.IsFaulted() ?
                   queue.vk_device.GetFirstFaultResult() :
                   VK_ERROR_UNKNOWN;
    }
    return VK_ERROR_UNKNOWN;
}

void VulkanSubmissionExecutor::ExecutePipelineSource(
    const std::shared_ptr<PipelineBatchState>& _batch,
    size_t                                     _source_index
) noexcept {
    struct WorkCompletion final {
        std::shared_ptr<PipelineBatchState> batch;
        ~WorkCompletion() {
            batch->FinishWork();
        }
    } work_completion{_batch};

    if (_source_index >= _batch->slots.size()) {
        LatchHardFailure(
            StreamPosition{_batch->sequence, _source_index},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        return;
    }

    PipelineSourceSlot& slot = _batch->slots[_source_index];
    auto reject_packet =
        [&](VkResult _result,
            bool _recoverable,
            std::string_view _reason) noexcept {
        if (_result == VK_SUCCESS) {
            _result = VK_ERROR_UNKNOWN;
        }
        if (_batch->rejection_publication) {
            _batch->rejection_publication->PublishSuffix(
                _source_index,
                static_cast<int32>(_result),
                _recoverable,
                _reason
            );
        }
        if (HasRecordedSourcePacket(slot.recorded_packet)) {
            RejectRecordedSourceForRuntime(
                slot.queue,
                std::move(slot.recorded_packet),
                _result,
                _recoverable
            );
            slot.recorded_packet.emplace<std::monostate>();
        }
    };

    int32 hard_result = static_cast<int32>(VK_ERROR_UNKNOWN);
    const PipelineFailure batch_failure = _batch->ReadFailure();
    if (IsHardFailureAtOrBefore(
            StreamPosition{_batch->sequence, _source_index},
            &hard_result
        )) {
        reject_packet(
            static_cast<VkResult>(hard_result),
            false,
            "Vulkan pipeline source was rejected after a hard failure"
        );
        return;
    }
    if (_source_index >= batch_failure.reject_from) {
        reject_packet(
            static_cast<VkResult>(batch_failure.result),
            batch_failure.recoverable,
            batch_failure.recoverable ?
                "Vulkan pipeline source was rejected by a dependency" :
                "Vulkan pipeline source was rejected after a hard failure"
        );
        return;
    }

    if (!HasRecordedSourcePacket(slot.recorded_packet)) {
        if (_batch->rejection_publication) {
            _batch->rejection_publication->PublishSuffix(
                _source_index,
                static_cast<int32>(VK_ERROR_UNKNOWN),
                false,
                "Vulkan pipeline source lost its recorded packet"
            );
        }
        LatchHardFailure(
            StreamPosition{_batch->sequence, _source_index + 1},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        _batch->PublishFailure(
            _source_index + 1,
            static_cast<int32>(VK_ERROR_UNKNOWN),
            false
        );
        ResetStreamScope();
        return;
    }
    CmdSubmit* submit = GetRecordedSourceSubmit(
        slot.recorded_packet
    );
    if (submit == nullptr) {
        reject_packet(
            VK_ERROR_UNKNOWN,
            false,
            "Vulkan pipeline source lost its submit payload"
        );
        _batch->PublishFailure(
            _source_index + 1,
            static_cast<int32>(VK_ERROR_UNKNOWN),
            false
        );
        LatchHardFailure(
            StreamPosition{_batch->sequence, _source_index + 1},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        ResetStreamScope();
        return;
    }

    try {
        PrepareStreamSubmit(*submit, slot.queue);
    } catch (...) {
        ReportRequestFailure(
            ERequestKind::Submit,
            VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
            std::current_exception()
        );
        reject_packet(
            VK_ERROR_UNKNOWN,
            false,
            "Vulkan pipeline source preparation failed"
        );
        _batch->PublishFailure(
            _source_index + 1,
            static_cast<int32>(VK_ERROR_UNKNOWN),
            false
        );
        LatchHardFailure(
            StreamPosition{_batch->sequence, _source_index + 1},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        ResetStreamScope();
        return;
    }

    RuntimePreCompletionPublication pre_completion_publication{
        .rejection_publication = _batch->rejection_publication,
        .reject_from           = _source_index,
    };
    const VulkanRuntimePreCompletionHook pre_completion{
        .context  = &pre_completion_publication,
        .callback =
            &VulkanSubmissionExecutor::
                PublishRuntimeFailureBeforeCompletion,
    };
    VulkanRuntimeSubmissionResult submit_result =
        SubmitRecordedSourceForRuntime(
            slot.queue,
            std::move(slot.recorded_packet),
            &pre_completion
        );
    slot.recorded_packet.emplace<std::monostate>();

    if (submit_result.WasSubmitted()) {
        assert(submit_result.completion.has_value());
        NotifySourceSubmitted(
            _batch->sequence,
            static_cast<uint32>(_source_index),
            slot.original_source_index,
            slot.source_segment_index,
            slot.source_segment_count,
            slot.queue,
            slot.async_queue_scope,
            slot.cross_native_predecessor_wait
        );
        if (slot.queue == EQueueType::Copy) {
            last_copy_timeline = std::max(
                last_copy_timeline,
                submit_result.completion->value
            );
        }
        UpdateStreamFrontier(
            slot.queue,
            *submit_result.completion,
            slot.async_queue_scope == 0
        );
        return;
    }

    const int32 failure_result =
        submit_result.outcome.result == VK_SUCCESS ?
            static_cast<int32>(VK_ERROR_UNKNOWN) :
            static_cast<int32>(submit_result.outcome.result);
    const bool recoverable = submit_result.IsRecoverableRejection();
    if (_batch->rejection_publication) {
        _batch->rejection_publication->PublishSuffix(
            _source_index,
            failure_result,
            recoverable,
            recoverable ?
                "Vulkan pipeline source submission was rejected by a dependency" :
                "Vulkan pipeline source submission failed"
        );
    }
    _batch->PublishFailure(
        _source_index + 1, failure_result, recoverable
    );
    ResetStreamScope();
    if (!recoverable) {
        LatchHardFailure(
            StreamPosition{_batch->sequence, _source_index + 1},
            failure_result
        );
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
    bool                        _recoverable,
    size_t                      _first_source,
    size_t*                     _next_unconsumed_source
) {
    const size_t first_source = std::min(_first_source, _batch.submits.size());
    const VkResult result =
        _result == static_cast<int32>(VK_SUCCESS) ?
            VK_ERROR_UNKNOWN :
            static_cast<VkResult>(_result);

    // Terminal publication is atomic at the runtime-batch frontier: every
    // signal and Query reaches its final classification before the first
    // per-queue Completion packet can release callbacks.
    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        CmdSubmit& submit = _batch.submits[source_index].submit;
        if (_recoverable) {
            submit.RejectPendingSignals();
            continue;
        }
        for (const SignalEvent& signal : submit.signal_events) {
            auto* fence =
                reinterpret_cast<VulkanFence*>(signal.timeline_handle);
            if (fence == nullptr) {
                continue;
            }
            try {
                fence->Fail(result);
            } catch (...) {
                fence->Reject(signal.value);
            }
        }
        // Queue rejection must not add an exact-value rejection after a hard
        // failure has already been published for the whole fence.
        submit.signal_events.clear();
    }

    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        CmdSubmit& submit = _batch.submits[source_index].submit;
        // Batch admission may already have installed the winning notification
        // ticket. Rejection must publish through that owner instead of
        // replacing it with the fallback shared by previously unsnapshotted
        // entries.
        const QueryPublishBatch source_query_batch =
            submit.query_publish_batch.Valid() ?
                submit.query_publish_batch :
                query_batch;
        submit.PublishPendingQueryErrors(_reason, source_query_batch);
    }

    // A Present receipt is also user-visible batch state. Resolve it only
    // after every submit signal and Query in the rejected suffix is terminal,
    // so a receipt waiter cannot observe a partially published batch.
    ResolveRejectedPresent(_batch.present);

    for (size_t source_index = first_source;
         source_index < _batch.submits.size();
         ++source_index) {
        RHIBackendSubmissionBatchEntry& entry = _batch.submits[source_index];
        if (entry.queue == EQueueType::Copy) {
            auto& queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            queue.RejectForRuntime(
                std::move(entry.submit), result, _recoverable
            );
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
        queue.RejectForRuntime(
            std::move(entry.submit), result, _recoverable
        );
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
    int32 latched_failure_result = static_cast<int32>(VK_ERROR_UNKNOWN);
    if (IsHardFailureAtOrBefore(
            StreamPosition{_batch.sequence, 0},
            &latched_failure_result
        )) {
        DrainPipelineBatches();
        RejectBatch(
            std::move(_batch),
            latched_failure_result,
            "submission runtime is latched after a hard failure",
            false,
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
        NotifyBatchPreflightRejected(_batch.sequence, false);
        // Validation runs before the ordinary admission decision so a later
        // malformed batch can be discovered while an earlier pipeline batch
        // is still owned by Submission. Commit that accepted prefix before
        // its queue receives this batch's rejection markers.
        DrainPipelineBatches();
        LatchHardFailure(
            StreamPosition{_batch.sequence, 0},
            static_cast<int32>(VK_ERROR_UNKNOWN)
        );
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(VK_ERROR_UNKNOWN),
            preflight.reason,
            true,
            0,
            &_exception_state.first_unconsumed_source
        );
        return;
    }

    const RHIQueueTopology                   queue_topology = RenderDevice::Get().GetQueueTopology();
    const VulkanSegmentMaterializationResult materialization =
        MaterializeMultiSegmentBatch(_batch, queue_topology);
    if (materialization.changed) {
        const BatchPreflightResult executable_preflight = ValidateBatch(_batch);
        if (!executable_preflight.valid) {
            LOG_ERROR(
                "[RHIExecutor][Vulkan][MultiSegment] batch={} "
                "executable preflight rejected reason={} topology_error={} "
                "source={} command={}",
                _batch.sequence,
                executable_preflight.reason,
                TopologyErrorName(_batch.topology.error),
                executable_preflight.source_index,
                executable_preflight.command_index
            );
            NotifyBatchPreflightRejected(_batch.sequence, true);
            DrainPipelineBatches();
            LatchHardFailure(
                StreamPosition{_batch.sequence, 0},
                static_cast<int32>(VK_ERROR_UNKNOWN)
            );
            RejectBatch(
                std::move(_batch),
                static_cast<int32>(VK_ERROR_UNKNOWN),
                executable_preflight.reason,
                true,
                0,
                &_exception_state.first_unconsumed_source
            );
            return;
        }
        if (!logged_multi_segment_topology) {
            logged_multi_segment_topology = true;
            LOG_INFO(
                "[RHIExecutor][Vulkan][MultiSegment] "
                "original_sources={} executable_segments={} "
                "cross_native_waits={} completion_aggregate=true",
                materialization.original_source_count,
                _batch.submits.size(),
                materialization.cross_native_wait_count
            );
        }
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

    bool batch_crosses_aliased_native_lane = false;
    if (runtime_queues_share_native_lane) {
        for (size_t lhs = 0;
             lhs < _batch.submits.size() &&
             !batch_crosses_aliased_native_lane;
             ++lhs) {
            const EQueueType lhs_queue = _batch.submits[lhs].queue;
            const RHIQueueBinding lhs_binding =
                queue_topology.Resolve(lhs_queue);
            for (size_t rhs = lhs + 1;
                 rhs < _batch.submits.size();
                 ++rhs) {
                const EQueueType rhs_queue =
                    _batch.submits[rhs].queue;
                if (lhs_queue == rhs_queue) {
                    continue;
                }
                const RHIQueueBinding rhs_binding =
                    queue_topology.Resolve(rhs_queue);
                if (lhs_binding.available &&
                    rhs_binding.available &&
                    lhs_binding.native_queue_id ==
                        rhs_binding.native_queue_id) {
                    batch_crosses_aliased_native_lane = true;
                    break;
                }
            }
        }
    }
    const bool pipeline_mode =
        !_batch.present.has_value() && !_batch.submits.empty() &&
        !batch_crosses_aliased_native_lane &&
        std::all_of(
            _batch.submits.begin(),
            _batch.submits.end(),
            [](const RHIBackendSubmissionBatchEntry& entry) {
                return IsParallelTranslateCandidate(entry) &&
                       !entry.submit.b_sync &&
                       !entry.submit.b_tick_profiling &&
                       !entry.submit.b_delete_resources &&
                       !entry.submit.EmitsProfilingQueries();
            }
        );

    if (pipeline_mode) {
        WaitForPipelineCapacity();
    } else {
        // Present, legacy/direct state inference, serial control, and a batch
        // crossing distinct logical wrappers of one native lane remain
        // explicit pipeline boundaries. Once the tail marker completes, this
        // coordinator may safely reuse the synchronous path and its existing
        // failure semantics.
        DrainPipelineBatches();
    }

    if (IsHardFailureAtOrBefore(
            StreamPosition{_batch.sequence, 0},
            &latched_failure_result
        )) {
        // Windowed admission can leave an already-accepted batch behind the
        // capacity item just retired above. Its Submission work must publish
        // terminal markers before this later batch is rejected directly.
        DrainPipelineBatches();
        RejectBatch(
            std::move(_batch),
            latched_failure_result,
            "submission runtime latched while waiting for pipeline admission",
            false,
            0,
            &_exception_state.first_unconsumed_source
        );
        return;
    }

    // Capture strong, copyable terminal handles before any CmdSubmit moves
    // into a Translate worker. A pipeline failure can then publish every raw
    // or already-recorded suffix Future without waiting for per-queue
    // Completion packets that may sit behind a blocking callback.
    auto rejection_publication =
        std::make_shared<BatchRejectionPublication>(_batch);

    struct PipelineSealGuard final {
        std::shared_ptr<PipelineBatchState> state{};

        void Seal() noexcept {
            if (!state) {
                return;
            }
            std::shared_ptr<PipelineBatchState> sealed_state =
                std::move(state);
            sealed_state->Seal();
        }

        ~PipelineSealGuard() {
            Seal();
        }
    };

    std::shared_ptr<PipelineBatchState> pipeline_state{};
    PipelineSealGuard                 pipeline_seal{};
    if (pipeline_mode) {
        auto completion = std::make_shared<Completion>();
        pipeline_state  = std::make_shared<PipelineBatchState>(
            _batch.sequence,
            _batch.submits.size(),
            completion,
            rejection_publication
        );
        pipeline_seal.state = pipeline_state;
        in_flight_batches.emplace_back(std::move(completion));
        if (!logged_cross_batch_pipeline) {
            logged_cross_batch_pipeline = true;
            LOG_INFO(
                "[RHIExecutor][Vulkan][SubmissionPipeline] "
                "mode=bounded-cross-batch batch_window={} "
                "per_native_lane_recorded_limit=1 "
                "ordered_submission=stable",
                batch_window
            );
        }
    }

    auto seal_and_drain_pipeline = [&]() noexcept {
        if (!pipeline_state) {
            return;
        }
        // The current state is itself present in in_flight_batches. Seal it
        // before waiting so an empty or partially handed-off failing batch
        // can publish its terminal completion instead of self-deadlocking.
        pipeline_seal.Seal();
        DrainPipelineBatches();
    };
    auto reject_remaining = [&](size_t _begin, VkResult _result, std::string_view _reason) {
        if (_result == VK_SUCCESS) {
            _result = VK_ERROR_UNKNOWN;
        }
        rejection_publication->PublishSuffix(
            _begin, static_cast<int32>(_result), false, _reason
        );
        // prepare_source may have admitted a later ready native lane before
        // the stable submission cursor rejects an earlier source. Preserve
        // gpu_frontier for successfully submitted work, but force the next
        // batch (even one reusing the same graph scope id) to freeze a fresh
        // entry frontier and republish its first-lane waits.
        if (pipeline_state) {
            pipeline_state->PublishFailure(
                _begin, static_cast<int32>(_result), false
            );
            seal_and_drain_pipeline();
        } else {
            ResetStreamScope();
        }
        LatchHardFailure(
            StreamPosition{_batch.sequence, _begin},
            static_cast<int32>(_result)
        );
        _exception_state.first_unconsumed_source =
            std::min(_begin, _batch.submits.size());
        RejectBatch(
            std::move(_batch),
            static_cast<int32>(_result),
            _reason,
            false,
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
            rejection_publication->PublishSuffix(
                _begin, static_cast<int32>(_result), true, _reason
            );
            if (pipeline_state) {
                pipeline_state->PublishFailure(
                    _begin, static_cast<int32>(_result), true
                );
                seal_and_drain_pipeline();
            } else {
                ResetStreamScope();
            }
            _exception_state.first_unconsumed_source =
                std::min(_begin, _batch.submits.size());
            RejectBatch(
                std::move(_batch),
                static_cast<int32>(_result),
                _reason,
                true,
                _exception_state.first_unconsumed_source,
                &_exception_state.first_unconsumed_source
            );
            _exception_state.first_unconsumed_source = _batch.submits.size();
        };

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
    auto has_foreign_same_native_frontier =
        [&](EQueueType target_queue, const auto& frontier) {
            for (size_t index = 0; index < frontier.size(); ++index) {
                if (!frontier[index].has_value()) {
                    continue;
                }
                const auto source_queue = static_cast<EQueueType>(index);
                if (source_queue != target_queue &&
                    same_native_queue(source_queue, target_queue)) {
                    return true;
                }
            }
            return false;
        };
    auto update_frontier =
        [&](EQueueType queue, WaitEvent completion, bool collapse) {
            UpdateStreamFrontier(queue, completion, collapse);
        };
    auto prepare_source = [&](size_t source_index) {
        RHIBackendSubmissionBatchEntry& entry = _batch.submits[source_index];
        const RHISourceSubmitPlan& source_plan =
            _batch.topology.source_plans[source_index];
        if (source_plan.segment_plan_count == 0) {
            return false;
        }

        assert(source_plan.segment_plan_count == 1);
        const RHISubmissionSegmentPlan& segment_plan =
            _batch.topology.segments[source_plan.segment_plan_begin];
        entry.submit.segments = {RHISubmitSegment{
            .queue = segment_plan.segment.queue,
            .begin = 0,
            .end   = entry.submit.cmds.size(),
        }};

        if (pipeline_mode) {
            return true;
        }
        PrepareStreamSubmit(entry.submit, entry.queue);
        return true;
    };
    auto is_parallel_translate_candidate = [&](size_t source_index) {
        return source_index < _batch.submits.size() &&
               _batch.topology.source_plans[source_index].segment_plan_count == 1 &&
               IsParallelTranslateCandidate(_batch.submits[source_index]);
    };
    auto process_parallel_translate_group = [&](size_t group_begin, size_t group_end) {
        using namespace VulkanTranslateWaveDetail;

        assert(group_begin < group_end);
        assert(group_end <= _batch.submits.size());

        const size_t                group_size = group_end - group_begin;
        Array<Array<SubmissionKey>> dependency_storage(group_size);
        Array<TranslateWaveNode>    schedule_nodes{};
        schedule_nodes.reserve(group_size);

        const size_t first_segment = _batch.topology.source_plans[group_begin].segment_plan_begin;
        const size_t last_segment  = _batch.topology.source_plans[group_end - 1].segment_plan_begin;
        for (size_t local_index = 0; local_index < group_size; ++local_index) {
            const size_t               source_index = group_begin + local_index;
            const RHISourceSubmitPlan& source_plan  = _batch.topology.source_plans[source_index];
            assert(source_plan.segment_plan_count == 1);
            const RHISubmissionSegmentPlan& segment_plan =
                _batch.topology.segments[source_plan.segment_plan_begin];

            Array<SubmissionKey>& dependencies = dependency_storage[local_index];
            dependencies.reserve(segment_plan.dependencies.size());
            for (const SubmissionKey& dependency : segment_plan.dependencies) {
                // A dependency outside this contiguous explicit-RDG
                // group belongs to an already terminalized prefix.
                if (dependency.submit_idx >= first_segment && dependency.submit_idx <= last_segment) {
                    dependencies.emplace_back(dependency);
                }
            }

            schedule_nodes.emplace_back(TranslateWaveNode{
                .key             = segment_plan.key,
                .native_queue_id = queue_topology.Resolve(_batch.submits[source_index].queue).native_queue_id,
                .async_translate = true,
                .dependencies    = dependencies,
            });
        }

        TranslateWaveScheduler scheduler{};
        if (!scheduler.Build(schedule_nodes)) {
            const size_t error_node = scheduler.GetErrorNodeIndex();
            LOG_ERROR(
                "[RHIExecutor][Vulkan][ParallelTranslate] "
                "schedule rejected batch={} source={} error={} "
                "dependency={}",
                _batch.sequence,
                error_node == TranslateWaveScheduler::NoIndex ? group_begin : group_begin + error_node,
                static_cast<uint32>(scheduler.GetBuildError()),
                scheduler.GetErrorDependencyIndex()
            );
            reject_remaining(group_begin, VK_ERROR_UNKNOWN, "parallel Translate schedule rejected");
            return false;
        }

        struct TranslateGroupSlot {
            size_t               source_index{0};
            SubmissionKey        key{};
            EQueueType           queue_type{EQueueType::Ignore};
            uint64               async_queue_scope{0};
            bool                 prepared{false};
            bool                 attempted{false};
            bool                 terminalized{false};
            RecordedSourcePacket recorded{};
        };

        Array<TranslateGroupSlot> slots{};
        slots.reserve(group_size);
        for (size_t local_index = 0; local_index < group_size; ++local_index) {
            const size_t                    source_index = group_begin + local_index;
            const RHISourceSubmitPlan&      source_plan  = _batch.topology.source_plans[source_index];
            const RHISubmissionSegmentPlan& segment_plan =
                _batch.topology.segments[source_plan.segment_plan_begin];
            slots.emplace_back(TranslateGroupSlot{
                .source_index      = source_index,
                .key               = segment_plan.key,
                .queue_type        = _batch.submits[source_index].queue,
                .async_queue_scope = _batch.submits[source_index].submit.async_queue_scope,
            });
        }

        // Group-wide terminalization is deliberately declared after the
        // slots so its destructor runs first. It closes every translated
        // packet and every untouched raw source before the outer request
        // exception barrier can reject the later batch suffix.
        struct TranslateGroupTerminalizer {
            RHIBackendSubmissionBatch* batch{nullptr};
            Array<TranslateGroupSlot>* slots{nullptr};
            size_t                     group_end{0};
            size_t*                    first_unconsumed_source{nullptr};
            VulkanSubmissionExecutor*  owner{nullptr};
            std::shared_ptr<BatchRejectionPublication> rejection_publication{};
            std::shared_ptr<PipelineBatchState>* pipeline_state{nullptr};
            PipelineSealGuard*         pipeline_seal{nullptr};
            bool                       armed{true};

            TranslateGroupTerminalizer(
                RHIBackendSubmissionBatch* _batch,
                Array<TranslateGroupSlot>* _slots,
                size_t                     _group_end,
                size_t*                    _first_unconsumed_source,
                VulkanSubmissionExecutor*  _owner,
                std::shared_ptr<BatchRejectionPublication> _rejection_publication,
                std::shared_ptr<PipelineBatchState>* _pipeline_state,
                PipelineSealGuard*         _pipeline_seal
            ) noexcept :
                batch(_batch),
                slots(_slots),
                group_end(_group_end),
                first_unconsumed_source(_first_unconsumed_source),
                owner(_owner),
                rejection_publication(std::move(_rejection_publication)),
                pipeline_state(_pipeline_state),
                pipeline_seal(_pipeline_seal) {}

            TranslateGroupTerminalizer(const TranslateGroupTerminalizer&)            = delete;
            TranslateGroupTerminalizer& operator=(const TranslateGroupTerminalizer&) = delete;

            ~TranslateGroupTerminalizer() noexcept {
                TerminalizeAll(VK_ERROR_UNKNOWN);
            }

            void TerminalizeAll(VkResult result) noexcept {
                if (!armed) {
                    return;
                }
                if (result == VK_SUCCESS) {
                    result = VK_ERROR_UNKNOWN;
                }
                if (pipeline_state != nullptr && *pipeline_state) {
                    // Direct queue rejection is observable through callbacks
                    // and signals. Preserve every already-handed-off prefix
                    // before terminalizing the current group, including this
                    // guard's exception-unwind path.
                    pipeline_seal->Seal();
                    owner->DrainPipelineBatches();
                }
                size_t rejection_begin = group_end;
                for (const TranslateGroupSlot& slot : *slots) {
                    if (!slot.terminalized) {
                        rejection_begin =
                            std::min(rejection_begin, slot.source_index);
                    }
                }
                if (rejection_begin < group_end &&
                    rejection_publication) {
                    // This guard also runs during exception unwinding, where
                    // fail_group() may never execute. Publish terminal state
                    // before any direct queue rejection can enqueue a
                    // Completion packet.
                    rejection_publication->PublishSuffix(
                        rejection_begin,
                        static_cast<int32>(result),
                        false,
                        "parallel Translate group failed before Completion notification"
                    );
                }
                for (TranslateGroupSlot& slot : *slots) {
                    if (slot.terminalized) {
                        continue;
                    }
                    RHIBackendSubmissionBatchEntry& entry = batch->submits[slot.source_index];
                    if (owner->HasRecordedSourcePacket(
                            slot.recorded
                        )) {
                        owner->RejectRecordedSourceForRuntime(
                            slot.queue_type,
                            std::move(slot.recorded),
                            result,
                            false
                        );
                        slot.recorded.emplace<std::monostate>();
                    } else if (!slot.attempted) {
                        owner->RejectSourceForRuntime(
                            slot.queue_type,
                            std::move(entry.submit),
                            result,
                            false
                        );
                    }
                    // Runtime Translate normally returns a terminal packet.
                    // If an attempted Translate returned no packet, its
                    // payload has already been consumed and cannot be
                    // terminalized a second time here.
                    slot.terminalized = true;
                }
                if (first_unconsumed_source != nullptr) {
                    *first_unconsumed_source = std::max(*first_unconsumed_source, group_end);
                }
                armed = false;
            }

            void Disarm() noexcept {
                if (first_unconsumed_source != nullptr) {
                    *first_unconsumed_source = std::max(*first_unconsumed_source, group_end);
                }
                armed = false;
            }
        } terminalizer{
            &_batch,
            &slots,
            group_end,
            &_exception_state.first_unconsumed_source,
            this,
            rejection_publication,
            &pipeline_state,
            &pipeline_seal,
        };

        auto slot_for_key = [&](const SubmissionKey& key) -> TranslateGroupSlot& {
            assert(key.op_seq == _batch.sequence);
            assert(key.submit_idx >= first_segment);
            const size_t local_index = key.submit_idx - first_segment;
            assert(local_index < slots.size());
            assert(slots[local_index].key == key);
            return slots[local_index];
        };
        auto translate_slot =
            [&](TranslateGroupSlot& slot, CmdSubmit&& submit) noexcept {
                const VulkanExecutableSourceMetadata& metadata =
                    materialization.sources[slot.source_index];
                const uint32 native_queue_id =
                    queue_topology.Resolve(
                        slot.queue_type
                    ).native_queue_id;
                slot.attempted = true;
                NotifySourceTranslation(
                    _batch.sequence,
                    static_cast<uint32>(slot.source_index),
                    metadata.original_source_index,
                    metadata.source_segment_index,
                    metadata.source_segment_count,
                    slot.queue_type,
                    native_queue_id,
                    slot.async_queue_scope,
                    EVulkanSourceTranslationPhase::Begin
                );
                slot.recorded =
                    TranslateSourceForRuntime(
                        slot.queue_type, std::move(submit)
                    );
                NotifySourceTranslation(
                    _batch.sequence,
                    static_cast<uint32>(slot.source_index),
                    metadata.original_source_index,
                    metadata.source_segment_index,
                    metadata.source_segment_count,
                    slot.queue_type,
                    native_queue_id,
                    slot.async_queue_scope,
                    HasRecordedSourcePacket(slot.recorded) ?
                        EVulkanSourceTranslationPhase::Recorded :
                        EVulkanSourceTranslationPhase::Failed
                );
            };
        auto fail_group = [&](size_t           failure_source,
                              VkResult         failure_result,
                              bool             recoverable_failure,
                              std::string_view failure_reason,
                              std::optional<size_t> rejection_source =
                                  std::nullopt) {
            if (failure_result == VK_SUCCESS) {
                failure_result = VK_ERROR_UNKNOWN;
            }
            scheduler.Cancel();
            size_t rejection_begin = group_end;
            for (const TranslateGroupSlot& slot : slots) {
                if (!slot.terminalized) {
                    rejection_begin =
                        std::min(rejection_begin, slot.source_index);
                }
            }
            // Include the failed source even when Translate could not return
            // a terminal packet. Its Query state must be published before any
            // earlier same-queue callback can run.
            rejection_begin = std::min(
                rejection_begin,
                rejection_source.value_or(failure_source)
            );
            rejection_publication->PublishSuffix(
                rejection_begin,
                static_cast<int32>(failure_result),
                recoverable_failure,
                failure_reason
            );
            terminalizer.TerminalizeAll(failure_result);
            LOG_ERROR(
                "[RHIExecutor][Vulkan][ParallelTranslate] "
                "batch={} failure_source={} reason={} result={} "
                "recoverable={}",
                _batch.sequence,
                failure_source,
                failure_reason,
                static_cast<int32>(failure_result),
                recoverable_failure
            );
            if (recoverable_failure) {
                reject_remaining_recoverable(group_end, failure_result, failure_reason);
            } else {
                reject_remaining(group_end, failure_result, failure_reason);
            }
            return false;
        };

        size_t next_release_local = 0;
        for (;;) {
            std::vector<SubmissionKey> wave = scheduler.NextWave();
            if (wave.empty()) {
                if (scheduler.IsComplete()) {
                    assert(next_release_local == slots.size());
                    terminalizer.Disarm();
                    return true;
                }
                return fail_group(
                    group_begin + next_release_local,
                    VK_ERROR_UNKNOWN,
                    false,
                    "parallel Translate schedule stalled"
                );
            }

            Array<TranslateGroupSlot*> wave_slots{};
            wave_slots.reserve(wave.size());
            for (const SubmissionKey& key : wave) {
                TranslateGroupSlot& slot = slot_for_key(key);
                assert(!slot.prepared);
                if (!prepare_source(slot.source_index)) {
                    return fail_group(
                        slot.source_index,
                        VK_ERROR_UNKNOWN,
                        false,
                        "parallel Translate source lost its segment"
                    );
                }
                slot.prepared = true;
                wave_slots.emplace_back(&slot);
            }

            bool dispatch_failed = false;
            if (wave_slots.size() > 1 && TaskGraph::IsInitialized()) {
                // LambdaTask::Dispatch can allocate before it queues a
                // task. Once it reaches TaskGraph::QueueTask, the engine's
                // lock-free path does not throw. A latch therefore gives
                // us a non-allocating, noexcept join: on a dispatch
                // exception, count down the current/unstarted suffix and
                // still wait for every already-queued worker.
                std::latch completed(static_cast<std::ptrdiff_t>(wave_slots.size()));
                size_t     dispatched_count = 0;
                for (; dispatched_count < wave_slots.size(); ++dispatched_count) {
                    TranslateGroupSlot* slot = wave_slots[dispatched_count];
                    try {
                        RHIBackendSubmissionBatchEntry* entry = &_batch.submits[slot->source_index];
                        (void)LambdaTask::Dispatch(
                            [slot, entry, &completed, &translate_slot] {
                                RHIThreadRoleScope translate_worker(ERHIThreadRole::Translate);
                                translate_slot(
                                    *slot, std::move(entry->submit)
                                );
                                completed.count_down();
                            },
                            EThread::AnyThread_NormalPri
                        );
                    } catch (...) {
                        dispatch_failed = true;
                        completed.count_down(static_cast<std::ptrdiff_t>(wave_slots.size() - dispatched_count)
                        );
                        break;
                    }
                }
                completed.wait();
            } else {
                for (TranslateGroupSlot* slot : wave_slots) {
                    RHIBackendSubmissionBatchEntry& entry = _batch.submits[slot->source_index];
                    translate_slot(
                        *slot, std::move(entry.submit)
                    );
                }
            }

            std::optional<size_t> translation_failure_source{};
            VkResult              translation_failure_result = VK_ERROR_UNKNOWN;
            std::string_view      translation_failure_reason{"parallel Translate task dispatch failure"};
            for (TranslateGroupSlot* slot : wave_slots) {
                if (!slot->attempted) {
                    if (!translation_failure_source) {
                        translation_failure_source = slot->source_index;
                        translation_failure_reason = "parallel Translate task dispatch failure";
                    }
                    continue;
                }

                used_queues[static_cast<size_t>(_batch.submits[slot->source_index].queue)] = true;
                if (!scheduler.MarkTranslated(slot->key) && !translation_failure_source) {
                    translation_failure_source = slot->source_index;
                    translation_failure_reason = "parallel Translate scheduler state failure";
                }
                if (!HasRecordedSourcePacket(slot->recorded)) {
                    // Runtime Translate normally returns a terminal packet
                    // for native-record failures. A missing packet is an
                    // unexpected handoff failure and has no Completion packet
                    // left for the group terminalizer to retire.
                    slot->terminalized = true;
                    if (!translation_failure_source) {
                        translation_failure_source = slot->source_index;
                        translation_failure_result =
                            GetQueueFaultResult(slot->queue_type);
                        translation_failure_reason = "parallel command translation failure";
                    }
                }
            }

            if (dispatch_failed || translation_failure_source) {
                return fail_group(
                    translation_failure_source.value_or(group_begin + next_release_local),
                    translation_failure_result,
                    false,
                    translation_failure_reason
                );
            }

            if (!logged_parallel_translate_wave && wave_slots.size() > 1) {
                logged_parallel_translate_wave = true;
                LOG_INFO(
                    "[RHIExecutor][Vulkan][ParallelTranslate] "
                    "mode=ready-native-lanes workers=TaskGraph "
                    "native_lanes={} ordered_submit_owner=serial "
                    "batch_window={}",
                    wave_slots.size(),
                    batch_window
                );
            }

            // Translation readiness is a native-lane ready set, while
            // submission remains a distinct stable cursor. A later
            // independent lane may already hold a recorded packet here;
            // it is released only after every earlier source is ready.
            while (next_release_local < slots.size()) {
                TranslateGroupSlot& slot  = slots[next_release_local];
                const auto          state = scheduler.GetState(slot.key);
                if (!state || *state != ETranslateWaveNodeState::Translated) {
                    break;
                }
                assert(HasRecordedSourcePacket(slot.recorded));
                assert(!slot.terminalized);

                RHIBackendSubmissionBatchEntry& entry = _batch.submits[slot.source_index];

                if (pipeline_state) {
                    PipelineSourceSlot& pipeline_slot =
                        pipeline_state->slots[slot.source_index];
                    const VulkanExecutableSourceMetadata& source_metadata =
                        materialization.sources[slot.source_index];
                    pipeline_slot.queue                         = slot.queue_type;
                    pipeline_slot.async_queue_scope             = slot.async_queue_scope;
                    pipeline_slot.original_source_index         =
                        source_metadata.original_source_index;
                    pipeline_slot.source_segment_index          =
                        source_metadata.source_segment_index;
                    pipeline_slot.source_segment_count          =
                        source_metadata.source_segment_count;
                    pipeline_slot.cross_native_predecessor_wait =
                        source_metadata.cross_native_predecessor_wait;
                    pipeline_slot.recorded_packet =
                        std::move(slot.recorded);
                    slot.recorded.emplace<std::monostate>();

                    pipeline_state->AddWork();
                    bool handoff_succeeded = false;
                    try {
                        handoff_succeeded = EnqueueSubmissionWork(SubmissionWork{
                            .execute = std::packaged_task<void()>(
                                [this,
                                 state = pipeline_state,
                                 source_index = slot.source_index] {
                                    ExecutePipelineSource(state, source_index);
                                }
                            ),
                        });
                    } catch (...) {
                        ReportRequestFailure(
                            ERequestKind::Submit,
                            VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                            std::current_exception()
                        );
                    }
                    if (!handoff_succeeded) {
                        pipeline_state->FinishWork();
                        if (HasRecordedSourcePacket(
                                pipeline_slot.recorded_packet
                            )) {
                            slot.recorded =
                                std::move(
                                    pipeline_slot.recorded_packet
                                );
                            pipeline_slot.recorded_packet.emplace<
                                std::monostate>();
                        }
                        return fail_group(
                            slot.source_index,
                            VK_ERROR_UNKNOWN,
                            false,
                            "parallel Translate pipeline handoff failure"
                        );
                    }

                    slot.terminalized = true;
                    _exception_state.first_unconsumed_source =
                        slot.source_index + 1;
                    if (!scheduler.MarkReleased(slot.key)) {
                        return fail_group(
                            slot.source_index,
                            VK_ERROR_UNKNOWN,
                            false,
                            "parallel Translate scheduler handoff release failure",
                            slot.source_index + 1
                        );
                    }
                    ++next_release_local;
                    continue;
                }

                VulkanRuntimeSubmissionResult submit_result{};
                try {
                    submit_result =
                        ExecuteOnSubmissionThread([this,
                                                   packet         = &slot.recorded,
                                                   batch_sequence = _batch.sequence,
                                                   source_index   = static_cast<uint32>(slot.source_index),
                                                   source_metadata =
                                                        materialization.sources[slot.source_index],
                                                   queue_type     = slot.queue_type,
                                                   async_scope    = slot.async_queue_scope,
                                                   rejection_publication]() mutable {
                            RuntimePreCompletionPublication
                                pre_completion_publication{
                                    .rejection_publication =
                                        rejection_publication,
                                    .reject_from = source_index,
                                };
                            const VulkanRuntimePreCompletionHook
                                pre_completion{
                                    .context =
                                        &pre_completion_publication,
                                    .callback =
                                        &VulkanSubmissionExecutor::
                                            PublishRuntimeFailureBeforeCompletion,
                                };
                            VulkanRuntimeSubmissionResult result =
                                SubmitRecordedSourceForRuntime(
                                    queue_type,
                                    std::move(*packet),
                                    &pre_completion
                                );
                            packet->emplace<std::monostate>();
                            if (result.WasSubmitted()) {
                                NotifySourceSubmitted(
                                    batch_sequence,
                                    source_index,
                                    source_metadata.original_source_index,
                                    source_metadata.source_segment_index,
                                    source_metadata.source_segment_count,
                                    queue_type,
                                    async_scope,
                                    source_metadata.cross_native_predecessor_wait
                                );
                            }
                            return result;
                        });
                } catch (...) {
                    ReportRequestFailure(
                        ERequestKind::Submit,
                        VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                        std::current_exception()
                    );
                    rejection_publication->PublishSuffix(
                        slot.source_index,
                        static_cast<int32>(VK_ERROR_UNKNOWN),
                        false,
                        "parallel Submission handoff failed before Completion notification"
                    );
                    RejectRecordedSourceForRuntime(
                        slot.queue_type,
                        std::move(slot.recorded),
                        VK_ERROR_UNKNOWN,
                        false
                    );
                    slot.recorded.emplace<std::monostate>();
                    submit_result.outcome = {EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
                }
                slot.terminalized                        = true;
                _exception_state.first_unconsumed_source = slot.source_index + 1;
                if (!scheduler.MarkReleased(slot.key)) {
                    return fail_group(
                        slot.source_index,
                        VK_ERROR_UNKNOWN,
                        false,
                        "parallel Translate scheduler release failure",
                        submit_result.WasSubmitted() ?
                            slot.source_index + 1 :
                            slot.source_index
                    );
                }
                ++next_release_local;

                if (submit_result.WasSubmitted()) {
                    assert(submit_result.completion.has_value());
                    if (entry.queue == EQueueType::Copy) {
                        last_copy_timeline = std::max(
                            last_copy_timeline,
                            submit_result.completion->value
                        );
                    }
                    update_frontier(entry.queue, *submit_result.completion, false);
                    continue;
                }

                const VkResult         failure_result      = submit_result.outcome.result == VK_SUCCESS ?
                                                                 VK_ERROR_UNKNOWN :
                                                                 submit_result.outcome.result;
                const bool             recoverable_failure = submit_result.IsRecoverableRejection();
                const std::string_view failure_reason =
                    recoverable_failure ? "parallel command submission dependency rejection" :
                                          "parallel command submission hard failure";
                return fail_group(slot.source_index, failure_result, recoverable_failure, failure_reason);
            }
        }
    };

    // Keep every source in the request-owned batch until its queue call has
    // returned. If reorder, descriptor-lease, allocator, or native recording
    // setup throws, Run's request barrier can still terminalize the current
    // source callbacks/signals and every later source. Moving all sources into
    // a temporary expansion array here would lose those obligations on unwind.
    for (size_t source_index = 0; source_index < _batch.submits.size(); ++source_index) {
        if (is_parallel_translate_candidate(source_index)) {
            const uint64 async_scope = _batch.submits[source_index].submit.async_queue_scope;
            size_t       group_end   = source_index + 1;
            while (group_end < _batch.submits.size() && is_parallel_translate_candidate(group_end) &&
                   _batch.submits[group_end].submit.async_queue_scope == async_scope) {
                ++group_end;
            }
            if (pipeline_mode || group_end - source_index > 1) {
                if (!process_parallel_translate_group(source_index, group_end)) {
                    return;
                }
                source_index = group_end - 1;
                continue;
            }
        }

        RHIBackendSubmissionBatchEntry& entry = _batch.submits[source_index];
        if (!prepare_source(source_index)) {
            _exception_state.first_unconsumed_source = source_index + 1;
            continue;
        }

        const uint64 async_scope = entry.submit.async_queue_scope;
        const bool serial_control =
            IsSerialControlSource(
                entry.submit.translate_execution_class
            );
        if (entry.queue == EQueueType::Copy) {
            auto& copy_queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            std::optional<VkCopyQueue::CurrentVulkanCopyRecordedSubmit>
                recorded =
                    copy_queue.TranslateForRuntime(std::move(entry.submit));
            used_queues[static_cast<size_t>(EQueueType::Copy)] = true;
            if (!recorded) {
                _exception_state.first_unconsumed_source =
                    source_index + 1;
                const VkResult result = copy_queue.device.IsFaulted() ?
                                            copy_queue.device.GetFirstFaultResult() :
                                            VK_ERROR_UNKNOWN;
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] batch={} Copy translation failed result={}",
                    _batch.sequence,
                    static_cast<int32>(result)
                );
                rejection_publication->PublishSuffix(
                    source_index,
                    static_cast<int32>(result),
                    false,
                    "Copy translation failure"
                );
                reject_remaining(
                    source_index + 1,
                    result,
                    "Copy translation failure"
                );
                return;
            }

            VulkanRuntimeSubmissionResult submit_result{};
            try {
                submit_result = ExecuteOnSubmissionThread(
                    [
                        &copy_queue,
                        packet = &*recorded,
                        batch_sequence = _batch.sequence,
                        source_index = static_cast<uint32>(source_index),
                        source_metadata = materialization.sources[source_index],
                        async_scope,
                        serial_control,
                        rejection_publication
                    ]() mutable {
                        RuntimePreCompletionPublication
                            pre_completion_publication{
                                .rejection_publication =
                                    rejection_publication,
                                .reject_from = source_index,
                            };
                        const VulkanRuntimePreCompletionHook
                            pre_completion{
                                .context = &pre_completion_publication,
                                .callback =
                                    &VulkanSubmissionExecutor::
                                        PublishRuntimeFailureBeforeCompletion,
                            };
                        auto submit =
                            [&copy_queue,
                             packet,
                             &pre_completion]() mutable {
                            return copy_queue.SubmitRecordedForRuntime(
                                std::move(*packet), &pre_completion
                            );
                        };
                        VulkanRuntimeSubmissionResult result =
                            serial_control ?
                                ExecuteObservedSubmissionBoundary(
                                    SubmissionBoundaryIdentity{
                                        .batch_sequence = batch_sequence,
                                        .operation_index = source_index,
                                        .queue = EQueueType::Copy,
                                        .kind =
                                            EVulkanSubmissionBoundaryKind::
                                                SerialControl,
                                    },
                                    submit
                                ) :
                                submit();
                        if (result.WasSubmitted()) {
                            NotifySourceSubmitted(
                                batch_sequence,
                                source_index,
                                source_metadata.original_source_index,
                                source_metadata.source_segment_index,
                                source_metadata.source_segment_count,
                                EQueueType::Copy,
                                async_scope,
                                source_metadata.cross_native_predecessor_wait
                            );
                        }
                        return result;
                    }
                );
            } catch (...) {
                ReportRequestFailure(
                    ERequestKind::Submit,
                    VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                    std::current_exception()
                );
                rejection_publication->PublishSuffix(
                    source_index,
                    static_cast<int32>(VK_ERROR_UNKNOWN),
                    false,
                    "Copy Submission handoff failed before Completion notification"
                );
                copy_queue.RejectRecordedForRuntime(
                    std::move(*recorded), VK_ERROR_UNKNOWN, false
                );
                submit_result.outcome = {
                    EVulkanOperationStatus::Rejected,
                    VK_ERROR_UNKNOWN
                };
            }
            // SubmitRecordedForRuntime has now either submitted or terminalized this
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
            } else if (submit_result.IsRecoverableRejection()) {
                // A failed prerequisite did not reach vkQueueSubmit. Drain the
                // current source on Completion, reject the rest of this
                // topology batch, and keep the runtime available for a later
                // independent batch.
                rejection_publication->PublishSuffix(
                    source_index,
                    static_cast<int32>(submit_result.outcome.result),
                    true,
                    "Copy submission dependency rejection"
                );
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
                rejection_publication->PublishSuffix(
                    source_index,
                    static_cast<int32>(submit_result.outcome.result),
                    false,
                    "Copy submission hard failure"
                );
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
                rejection_publication->PublishSuffix(
                    source_index,
                    static_cast<int32>(result),
                    false,
                    "command translation failure"
                );
                reject_remaining(
                    source_index + 1, result, "command translation failure"
                );
                return;
            }
            VulkanRuntimeSubmissionResult submit_result{};
            try {
                submit_result = ExecuteOnSubmissionThread(
                    [
                        &queue,
                        packet = &*recorded,
                        batch_sequence = _batch.sequence,
                        source_index = static_cast<uint32>(source_index),
                        source_metadata = materialization.sources[source_index],
                        queue_type = entry.queue,
                        async_scope,
                        serial_control,
                        rejection_publication
                    ]() mutable {
                        RuntimePreCompletionPublication
                            pre_completion_publication{
                                .rejection_publication =
                                    rejection_publication,
                                .reject_from = source_index,
                            };
                        const VulkanRuntimePreCompletionHook
                            pre_completion{
                                .context = &pre_completion_publication,
                                .callback =
                                    &VulkanSubmissionExecutor::
                                        PublishRuntimeFailureBeforeCompletion,
                            };
                        auto submit =
                            [&queue,
                             packet,
                             &pre_completion]() mutable {
                            return queue.SubmitRecordedForRuntime(
                                std::move(*packet), &pre_completion
                            );
                        };
                        VulkanRuntimeSubmissionResult result =
                            serial_control ?
                                ExecuteObservedSubmissionBoundary(
                                    SubmissionBoundaryIdentity{
                                        .batch_sequence = batch_sequence,
                                        .operation_index = source_index,
                                        .queue = queue_type,
                                        .kind =
                                            EVulkanSubmissionBoundaryKind::
                                                SerialControl,
                                    },
                                    submit
                                ) :
                                submit();
                        if (result.WasSubmitted()) {
                            NotifySourceSubmitted(
                                batch_sequence,
                                source_index,
                                source_metadata.original_source_index,
                                source_metadata.source_segment_index,
                                source_metadata.source_segment_count,
                                queue_type,
                                async_scope,
                                source_metadata.cross_native_predecessor_wait
                            );
                        }
                        return result;
                    }
                );
            } catch (...) {
                ReportRequestFailure(
                    ERequestKind::Submit,
                    VulkanSubmissionDetail::EWorkerRequestFailurePhase::Process,
                    std::current_exception()
                );
                rejection_publication->PublishSuffix(
                    source_index,
                    static_cast<int32>(VK_ERROR_UNKNOWN),
                    false,
                    "command Submission handoff failed before Completion notification"
                );
                queue.RejectRecordedForRuntime(
                    std::move(*recorded), VK_ERROR_UNKNOWN, false
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
                rejection_publication->PublishSuffix(
                    source_index,
                    static_cast<int32>(submit_result.outcome.result),
                    true,
                    "command submission dependency rejection"
                );
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
                rejection_publication->PublishSuffix(
                    source_index,
                    static_cast<int32>(submit_result.outcome.result),
                    false,
                    "command submission hard failure"
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
        // A foreign logical queue sharing Graphics' native queue needs no GPU
        // semaphore wait, but it has an independent Completion owner. Emit an
        // empty Graphics marker so no-GPU-tail Retry/Recreate retirement stays
        // behind that predecessor instead of retiring resources immediately.
        const bool alias_completion_marker =
            has_foreign_same_native_frontier(
                EQueueType::Graphics,
                gpu_frontier
            );
        if (!bridge.wait_events.empty() || alias_completion_marker) {
            const uint32 bridge_dependency_wait_count =
                static_cast<uint32>(bridge.wait_events.size());
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
                    [
                        &graphics_queue,
                        packet = &*recorded,
                        batch_sequence = _batch.sequence,
                        operation_index =
                            static_cast<uint32>(_batch.submits.size()),
                        bridge_dependency_wait_count
                    ]() mutable {
                        return ExecuteObservedSubmissionBoundary(
                            SubmissionBoundaryIdentity{
                                .batch_sequence  = batch_sequence,
                                .operation_index = operation_index,
                                .queue            = EQueueType::Graphics,
                                .kind =
                                    EVulkanSubmissionBoundaryKind::
                                        PresentBridge,
                                .dependency_wait_count =
                                    bridge_dependency_wait_count,
                            },
                            [&graphics_queue, packet]() mutable {
                                return graphics_queue.SubmitRecordedForRuntime(
                                    std::move(*packet)
                                );
                            }
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
                    std::move(*recorded), VK_ERROR_UNKNOWN, false
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
            [
                &graphics_queue,
                present = &present,
                batch_sequence = _batch.sequence,
                operation_index =
                    static_cast<uint32>(_batch.submits.size() + 1)
            ]() mutable {
                const PresentReceiptRef receipt = present->receipt;
                return ExecuteObservedSubmissionBoundary(
                    SubmissionBoundaryIdentity{
                        .batch_sequence  = batch_sequence,
                        .operation_index = operation_index,
                        .queue            = EQueueType::Graphics,
                        .kind =
                            EVulkanSubmissionBoundaryKind::Present,
                    },
                    [&graphics_queue, present]() mutable {
                        return graphics_queue.PresentForRuntime(
                            std::move(present->swapchain),
                            present->source,
                            // Keep the request's receipt copy until the
                            // Submission owner returns so the exception
                            // barrier can resolve it.
                            present->receipt
                        );
                    },
                    receipt
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
            LatchHardFailure(
                StreamPosition{_batch.sequence, _batch.submits.size()},
                static_cast<int32>(
                    present_result.outcome.result == VK_SUCCESS ?
                        VK_ERROR_UNKNOWN :
                        present_result.outcome.result
                )
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
