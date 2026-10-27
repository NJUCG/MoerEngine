#include "rhi/executor/RHIExecutorInternal.h"

#include "log/LogSystem.h"
#include "misc/RAII.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <stdexcept>
#include <utility>

#if !defined(__APPLE__)
#include "rhi/vulkan/VulkanSubmissionExecutor.h"
#endif

namespace Moer::Render::RHIExecutorDetail {
namespace {

GraphEventRef MakeCompletedBackendEvent() {
    GraphEventRef event = GraphEvent::CreateGraphEvent();
    event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return event;
}

// D3D12 has not adopted the Vulkan submission runtime yet. Keep its old queue
// ownership path isolated instead of weakening the Vulkan topology contract.
class LegacyQueueBackendExecutor final : public RHIBackendExecutor {
public:
    void Enqueue(RHISubmissionBatch&& _batch) override {
        std::optional<EQueueType> last_queue{};
        uint64                    ordered_copy_timeline = 0;
        {
            std::lock_guard lock(state_mutex);
            last_queue            = ordered_tail_queue;
            ordered_copy_timeline = ordered_tail_copy_timeline;
        }

        const RHIQueueTopology queue_topology =
            RenderDevice::Get().GetQueueTopology();
        for (const RHISubmissionRequest& entry : _batch.submits) {
            if (SubmitHasWork(entry.submit) &&
                !queue_topology.Resolve(entry.queue).available) {
                throw std::runtime_error(
                    "legacy RHI backend received work for an unavailable queue"
                );
            }
        }
        if (_batch.present.has_value() &&
            !queue_topology.graphics.available) {
            throw std::runtime_error(
                "legacy RHI backend cannot Present without a graphics queue"
            );
        }

        // The legacy D3D12 backend has no reliable per-submit fault outcome.
        // Fail closed for host completion futures rather than reporting a
        // false Ready. Publish the whole batch before the first queue can
        // retire; each native Completion packet still owns callback release.
        const GpuCompletionPublishBatch completion_batch =
            GpuCompletionBackendAccess::BeginPublishBatch();
        for (RHISubmissionRequest& entry : _batch.submits) {
            if (!entry.submit.gpu_completion_tokens.empty()) {
                entry.submit.PublishPendingGpuCompletionErrors(
                    "D3D12 GPU completion futures are unsupported",
                    completion_batch
                );
            }
        }

        // D3D12 intentionally exposes GPU Query as unsupported. Preserve the
        // requested kind in the terminal diagnostic so an occlusion caller is
        // never reported as a timestamp capability failure.
        const QueryPublishBatch query_batch =
            QueryBackendAccess::BeginPublishBatch();
        for (RHISubmissionRequest& entry : _batch.submits) {
            if (!entry.submit.query_tokens.empty()) {
                const QueryPublishBatch effective_batch =
                    entry.submit.query_publish_batch.Valid() ?
                        entry.submit.query_publish_batch :
                        query_batch;
                entry.submit.query_publish_batch = effective_batch;
                for (const QueryToken& token :
                     entry.submit.query_tokens) {
                    QueryBackendAccess::PublishErrorIfPending(
                        token,
                        token.Kind() == QueryKind::Occlusion ?
                            "D3D12 occlusion queries are unsupported" :
                            "D3D12 timestamp queries are unsupported",
                        effective_batch
                    );
                }
            }
        }

        // Enqueue's exception contract leaves only the unaccepted suffix in
        // `_batch`. Publish the accepted prefix tail even if a later queue
        // operation throws, so subsequent D3D12 batches cannot overtake work
        // which already crossed a native queue boundary.
        auto publish_ordered_tail = OnScopeExit([&]() noexcept {
            try {
                std::lock_guard lock(state_mutex);
                ordered_tail_queue         = last_queue;
                ordered_tail_copy_timeline = ordered_copy_timeline;
            } catch (...) {
            }
        });

        for (RHISubmissionRequest& entry : _batch.submits) {
            if (!SubmitHasWork(entry.submit)) {
                continue;
            }
            if (last_queue.has_value() && *last_queue != entry.queue) {
                WaitForQueue(*last_queue, ordered_copy_timeline);
            }
            switch (entry.queue) {
                case EQueueType::Graphics:
                case EQueueType::Compute:
                    RenderDevice::Get().GetCommandQueue(entry.queue).Execute(std::move(entry.submit));
                    ordered_copy_timeline = 0;
                    break;
                case EQueueType::Copy: {
                    const WaitEvent completion =
                        RenderDevice::Get().GetCopyQueue().Execute(std::move(entry.submit));
                    ordered_copy_timeline = completion.value;
                    std::lock_guard lock(state_mutex);
                    last_copy_timeline = std::max(last_copy_timeline, completion.value);
                    break;
                }
                case EQueueType::Ignore:
                case EQueueType::Num:
                default:
                    throw std::runtime_error(
                        "legacy RHI backend received an invalid queue"
                    );
            }
            // Queue::Execute has either accepted the native work and retained
            // its callbacks, or thrown while leaving this submit available to
            // the outer rejection finalizer. Clear only after a successful
            // return so a later entry failure cannot re-reject this prefix.
            ResetAcceptedSubmit(entry.submit);
            {
                std::lock_guard lock(state_mutex);
                used_queues[static_cast<size_t>(entry.queue)] = true;
            }
            last_queue = entry.queue;
        }

        if (_batch.present.has_value()) {
            if (last_queue.has_value() && *last_queue != EQueueType::Graphics) {
                WaitForQueue(*last_queue, ordered_copy_timeline);
            }
            RHIPresentRequest& present = *_batch.present;
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Present(std::move(present.swapchain), present.source, std::move(present.receipt));
            _batch.present.reset();
            last_queue            = EQueueType::Graphics;
            ordered_copy_timeline = 0;
            std::lock_guard lock(state_mutex);
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
            present_seen                                           = true;
        }
    }

    GraphEventRef Sync(ERHISyncDepth _depth) override {
        StaticArray<bool, static_cast<size_t>(EQueueType::Num)> queues{};
        uint64                                                  copy_timeline = 0;
        {
            std::lock_guard lock(state_mutex);
            queues        = used_queues;
            copy_timeline = last_copy_timeline;
        }

        if (queues[static_cast<size_t>(EQueueType::Graphics)] ||
            _depth == ERHISyncDepth::Present) {
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
        }
        if (queues[static_cast<size_t>(EQueueType::Compute)]) {
            RenderDevice::Get().GetCommandQueue(EQueueType::Compute).Sync();
        }
        if (copy_timeline != 0) {
            RenderDevice::Get().GetCopyQueue().Sync(copy_timeline);
        }
        if (_depth == ERHISyncDepth::Present) {
            RenderDevice::Get().WaitIdle();
        }
        return MakeCompletedBackendEvent();
    }

    void Flush(ERHIFlushDepth) override {
        // Queue::Execute has already crossed into the existing RHI FIFO.
    }

    void ShutDown() override {
        GraphEventRef completion = Sync(ERHISyncDepth::Present);
        if (completion) {
            completion->Wait(EThread::UNKNOWN_THREAD);
        }
    }

private:
    static void ResetAcceptedSubmit(CmdSubmit& _submit) noexcept {
        _submit.lifetime.reset();
        _submit.cmds.clear();
        _submit.callbacks.clear();
        _submit.success_callbacks.clear();
        _submit.gpu_completion_tokens.clear();
        _submit.gpu_completion_publish_batch = {};
        _submit.query_tokens.clear();
        _submit.query_publish_batch = {};
        _submit.cached_args.clear();
        _submit.segments.clear();
        _submit.wait_events.clear();
        _submit.signal_events.clear();
        _submit.b_sync             = false;
        _submit.b_tick_profiling   = false;
        _submit.profiling_phase    = ERHIProfilingPhase::Disabled;
        _submit.b_delete_resources = false;
        _submit.resource_state_ownership =
            ERHIResourceStateOwnership::BackendTracked;
        _submit.translate_execution_class =
            ERHITranslateExecutionClass::Parallel;
        _submit.async_queue_scope = 0;
        _submit.debug_label.clear();
        _submit.debug_label_color = GpuMarkerPalette::Frame();
    }

    static void WaitForQueue(EQueueType _queue, uint64 _copy_timeline) {
        switch (_queue) {
            case EQueueType::Graphics:
            case EQueueType::Compute:
                RenderDevice::Get().GetCommandQueue(_queue).Sync();
                return;
            case EQueueType::Copy:
                if (_copy_timeline != 0) {
                    RenderDevice::Get().GetCopyQueue().Sync(_copy_timeline);
                }
                return;
            case EQueueType::Ignore:
            case EQueueType::Num:
            default:
                assert(false && "Legacy RHI executor has an invalid ordered tail queue");
                return;
        }
    }

    std::mutex                                              state_mutex;
    StaticArray<bool, static_cast<size_t>(EQueueType::Num)> used_queues{};
    uint64                                                  last_copy_timeline{0};
    bool                                                    present_seen{false};
    std::optional<EQueueType>                               ordered_tail_queue{};
    uint64                                                  ordered_tail_copy_timeline{0};
};

} // namespace

std::shared_ptr<RHIBackendExecutor> CreateBackendExecutor(uint32 _submission_batch_window) {
    switch (RenderDevice::Get().GetRHIType()) {
        case ERHIType::Vulkan:
#if !defined(__APPLE__)
            return std::make_shared<VulkanSubmissionExecutor>(_submission_batch_window);
#else
            break;
#endif
        case ERHIType::D3D12:
            LOG_WARNING(
                "[RHIExecutor] D3D12 uses the legacy queue adapter; upper Vulkan topology is unavailable"
            );
            return std::make_shared<LegacyQueueBackendExecutor>();
        case ERHIType::Metal:
            LOG_INFO("[RHIExecutor] Metal bootstrap uses the single-segment legacy queue adapter");
            return std::make_shared<LegacyQueueBackendExecutor>();
    }
    LOG_ERROR("[RHIExecutor] unsupported RHI type");
    assert(false && "Unsupported RHI type");
    return {};
}

} // namespace Moer::Render::RHIExecutorDetail
