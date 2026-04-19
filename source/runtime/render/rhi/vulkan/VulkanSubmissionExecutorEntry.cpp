#include "VulkanSubmissionExecutorPrivate.h"

namespace Moer::Render {

namespace {

struct PendingPreprocessBatch {
    Array<ExecutorOp> ops{};
    uint64            op_seq_base{0};
    uint64            trace_frame{0};
};

struct PendingTranslateBatch {
    TranslatePipelineBatch pipeline_batch{};
    uint64                 trace_frame{0};
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
                    pipeline_batch.submit_ops.empty()) {
                    return;
                }

                auto translate_request            = std::make_shared<PendingTranslateBatch>();
                translate_request->pipeline_batch = std::move(pipeline_batch);
                translate_request->trace_frame    = request->trace_frame;

                translate_pipe.Enqueue(
                    [this, translate_request]() {
                        ScopedRHITraceFrame trace_scope(translate_request->trace_frame);
                        TRACE_SCOPE_CAT("VulkanSubmissionExecutor.TranslatePipe", "RHI");

                        translate_pipeline.Dispatch(
                            std::move(translate_request->pipeline_batch),
                            submission_runtime
                        );
                    },
                    {},
                    EThread::AnyThread_NormalPri
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
            return;
        }

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
    TaskPipe                 translate_pipe{};
    std::atomic_bool         b_enable{true};
};

static std::mutex g_executor_state_mutex{};
static std::unique_ptr<VulkanSubmissionExecutorState> g_executor_state{};
static std::atomic_uint64_t g_executor_op_seq_base{0};

static VulkanSubmissionExecutorState& GetExecutorState() {
    std::lock_guard<std::mutex> lock(g_executor_state_mutex);
    if (!g_executor_state) {
        g_executor_state = std::make_unique<VulkanSubmissionExecutorState>();
    }
    return *g_executor_state;
}

static VulkanSubmissionExecutorState* TryGetExecutorState() {
    std::lock_guard<std::mutex> lock(g_executor_state_mutex);
    return g_executor_state.get();
}

} // namespace

void VulkanSubmissionExecutor::Enqueue(VulkanSubmissionBatch&& batch) {
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

    const uint64 op_seq_base = g_executor_op_seq_base.fetch_add(
        EstimatePlatformOpCount(ops),
        std::memory_order_relaxed
    );

    GetExecutorState().Enqueue(std::move(ops), op_seq_base, trace_frame);
}

GraphEventRef VulkanSubmissionExecutor::Sync(ERHISyncDepth depth) {
    if (auto* state = TryGetExecutorState(); state != nullptr) {
        return state->Sync(depth);
    }
    return CreateCompletedExecutorEvent();
}

void VulkanSubmissionExecutor::Flush(ERHIFlushDepth depth) {
    if (auto* state = TryGetExecutorState(); state != nullptr) {
        state->Flush(depth);
    }
}

void VulkanSubmissionExecutor::Shutdown() {
    std::unique_ptr<VulkanSubmissionExecutorState> state{};
    {
        std::lock_guard<std::mutex> lock(g_executor_state_mutex);
        state = std::move(g_executor_state);
    }
    if (state) {
        state->Shutdown();
    }
    VulkanTranslateTask::ResetSchedulerState();
}

} // namespace Moer::Render
