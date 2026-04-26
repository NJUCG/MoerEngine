#include "VulkanSubmissionExecutorPrivate.h"

namespace Moer::Render {

namespace {

struct PendingPreprocessBatch {
    Array<ExecutorOp> ops{};
    uint64            op_seq_base{0};
    uint64            trace_frame{0};
};

static GraphEventRef CreateCompletedExecutorEvent() {
    GraphEventRef event = GraphEvent::CreateGraphEvent();
    event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return event;
}

class VulkanSubmissionExecutorState {
public:
    VulkanSubmissionExecutorState() :
        interrupt_runtime(),
        submission_runtime(interrupt_runtime) {}

    void Enqueue(Array<ExecutorOp>&& ops, uint64 op_seq_base, uint64 trace_frame) {
        assert(
            b_enable.load(std::memory_order_acquire) &&
            "Enqueue is not allowed after shutdown begins"
        );
        if (!b_enable.load(std::memory_order_acquire) || ops.empty()) {
            return;
        }

        auto request         = std::make_shared<PendingPreprocessBatch>();
        request->ops         = std::move(ops);
        request->op_seq_base = op_seq_base;
        request->trace_frame = trace_frame;

        preprocess_pipe.Enqueue(
            [this, request]() {
                ScopedRHITraceFrame trace_scope(request->trace_frame);
                TRACE_SCOPE_CAT("VulkanSubmissionExecutor.PreprocessPipe", "RHI");

                PreprocessTranslateStore preprocess_store =
                    preprocessor.Process(request->ops, request->op_seq_base);
                TranslatePipelineBatch pipeline_batch = translate_pipeline.Assemble(
                    std::move(request->ops),
                    preprocess_store,
                    request->op_seq_base
                );
                if (pipeline_batch.translate_ops.empty() &&
                    pipeline_batch.submit_ops.empty() &&
                    pipeline_batch.present_ops.empty()) {
                    return;
                }

                TRACE_SCOPE_CAT("VulkanSubmissionExecutor.TranslateSchedule", "RHI");
                translate_pipeline.Dispatch(
                    std::move(pipeline_batch),
                    request->trace_frame,
                    translate_dispatch_pipe,
                    translate_pipe,
                    submission_runtime
                );
            },
            {},
            EThread::AnyThread_NormalPri
        );
    }

    GraphEventRef Sync(ERHISyncDepth depth) {
        if (!b_enable.load(std::memory_order_acquire)) {
            return CreateCompletedExecutorEvent();
        }
        FlushInternal(ERHIFlushDepth::SubmitGPU);
        return submission_runtime.Sync(depth);
    }

    GraphEventRef Sync(Swapchain* swapchain) {
        if (!b_enable.load(std::memory_order_acquire)) {
            return CreateCompletedExecutorEvent();
        }
        FlushInternal(ERHIFlushDepth::SubmitGPU);
        return submission_runtime.Sync(swapchain);
    }

    void Flush(ERHIFlushDepth depth) {
        if (!b_enable.load(std::memory_order_acquire)) {
            return;
        }
        FlushInternal(depth);
    }

    void Shutdown() {
        bool expected_enabled = true;
        if (!b_enable.compare_exchange_strong(expected_enabled, false, std::memory_order_acq_rel)) {
            return;
        }

        FlushInternal(ERHIFlushDepth::SubmitGPU);
        submission_runtime.Shutdown();
        interrupt_runtime.Shutdown();
    }

private:
    void FlushInternal(ERHIFlushDepth depth) {
        WaitForPipe(preprocess_pipe);
        if (depth == ERHIFlushDepth::RHITranslate) {
            WaitForPipe(translate_dispatch_pipe);
            return;
        }

        WaitForPipe(translate_dispatch_pipe);
        WaitForPipe(translate_pipe);
        submission_runtime.Drain();
    }

    static void WaitForPipe(TaskPipe& pipe) {
        if (GraphEventRef boundary = pipe.Close(); boundary) {
            boundary->Wait(EThread::UNKNOWN_THREAD);
        }
    }

private:
    VulkanInterruptRuntime   interrupt_runtime;
    VulkanSubmissionRuntime  submission_runtime;
    SubmissionPreprocessor   preprocessor{};
    TranslatePipelineRuntime translate_pipeline{};
    TaskPipe                 preprocess_pipe{};
    TaskPipe                 translate_dispatch_pipe{};
    TaskPipe                 translate_pipe{};
    std::atomic_bool         b_enable{true};
};

} // namespace

struct VulkanSubmissionExecutor::State : VulkanSubmissionExecutorState {};

VulkanSubmissionExecutor::VulkanSubmissionExecutor() :
    state_(std::make_unique<State>()) {}

VulkanSubmissionExecutor::~VulkanSubmissionExecutor() = default;

void VulkanSubmissionExecutor::Enqueue(RHIBackendSubmissionBatch&& batch) {
    Array<ExecutorOp> ops{};
    ops.reserve(batch.submits.size() + (batch.present.has_value() ? 1u : 0u));
    for (auto& submit_entry : batch.submits) {
        ExecutorSubmitOp submit_op{};
        submit_op.queue = submit_entry.queue;
        submit_op.submits.emplace_back(std::move(submit_entry.submit));
        ops.emplace_back(std::move(submit_op));
    }
    if (batch.present.has_value()) {
        ops.emplace_back(ExecutorPresentOp{
            .swapchain = batch.present->swapchain,
            .target    = batch.present->source,
            .queue     = EQueueType::Graphics
        });
    }

    TRACE_SCOPE_CAT("VulkanSubmissionExecutor.Enqueue", "RHI");
    const uint64 trace_frame = NextRHITraceFrameIndex();
    RHITRACE_LOG(
        basic,
        "[RHITrace][Frame] frame={} op_count={}",
        trace_frame,
        ops.size()
    );
    if (ops.empty()) {
        return;
    }

    const uint64 op_seq_base = executor_op_seq_base_.fetch_add(
        EstimatePlatformOpCount(ops),
        std::memory_order_relaxed
    );

    state_->Enqueue(std::move(ops), op_seq_base, trace_frame);
}

GraphEventRef VulkanSubmissionExecutor::Sync(ERHISyncDepth depth) {
    return state_->Sync(depth);
}

GraphEventRef VulkanSubmissionExecutor::Sync(Swapchain* swapchain) {
    return state_->Sync(swapchain);
}

void VulkanSubmissionExecutor::Flush(ERHIFlushDepth depth) {
    state_->Flush(depth);
}

void VulkanSubmissionExecutor::ShutDown() {
    state_->Shutdown();
}

} // namespace Moer::Render
