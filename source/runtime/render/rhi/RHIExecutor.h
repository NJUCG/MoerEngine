#pragma once

#include "RenderAPI.h"
#include "rhi/RHIExecutorBackend.h"
#include "rhi/RHIExecutorRecordingHandoff.h"

#include <condition_variable>
#include <memory>
#include <mutex>

namespace Moer::Render {

struct RHIRecordingSubmitMetadata {
    std::optional<std::string>                 debug_label{};
    float4                                     debug_label_color{GpuMarkerPalette::Pass()};
    std::optional<ERHIProfilingPhase>          profiling_phase{};
    std::optional<ERHITranslateExecutionClass> translate_execution_class{};
};

// One independently recorded source. completion must be signalled only after
// the producer has stopped mutating command_list. submit_metadata is immutable
// at publication and deliberately contains data rather than an arbitrary
// handoff-thread callback, so finalization cannot recursively Flush/Sync.
// The producer owns the only mutable reference until it calls Signal()/Fail().
// It must not call RHIExecutor::Flush, Sync or ShutDown while recording; those
// are ordering/lifecycle boundaries owned by the submitting thread.
struct RHIRecordingSource {
    SharedPtr<CommandList>       command_list{};
    RHIRecordingGateRef          completion{};
    RHIRecordingSubmitMetadata   submit_metadata{};
};

class RENDER_API RHIExecutor {
public:
    static RHIExecutor& Get();
    static void StartUp();

    void Submit(
        EQueueType             _queue,
        CmdSubmit&&            _submit,
        ERHIExecSubmitFlags    _flags   = ERHIExecSubmitFlags::FlushGPU,
        RHIPresentRequest*     _present = nullptr
    );
    void Submit(
        Array<RHIBackendSubmissionBatchEntry>&& _submits,
        ERHIExecSubmitFlags                      _flags   = ERHIExecSubmitFlags::FlushGPU,
        RHIPresentRequest*                       _present = nullptr
    );
    void Submit(
        Array<CommandList>&&   _command_lists,
        ERHIExecSubmitFlags    _flags   = ERHIExecSubmitFlags::FlushGPU,
        RHIPresentRequest*     _present = nullptr
    );

    // Recording handoff API. Sources remain shared and mutable until their
    // explicit gates complete; the RHI handoff worker then seals them into
    // CmdSubmit payloads in input order. A batch may use one common gate or a
    // distinct gate per source through the RHIRecordingSource overload. If any
    // producer fails, the whole group waits for all producers, runs ordinary
    // CommandList cleanup callbacks once, skips success callbacks, and never
    // reaches a GPU queue. Shutdown cancellation remains non-blocking and does
    // not inspect CommandLists whose producers may still be active.
    void SubmitRecording(
        SharedPtr<CommandList> _command_list,
        RHIRecordingGateRef    _completion,
        ERHIExecSubmitFlags    _flags = ERHIExecSubmitFlags::FlushGPU
    );
    void SubmitRecording(
        Array<SharedPtr<CommandList>>&& _command_lists,
        RHIRecordingGateRef             _batch_completion,
        ERHIExecSubmitFlags             _flags = ERHIExecSubmitFlags::FlushGPU
    );
    void SubmitRecording(
        Array<RHIRecordingSource>&& _sources,
        ERHIExecSubmitFlags         _flags = ERHIExecSubmitFlags::FlushGPU
    );
    void Present(RHIPresentRequest&& _present, bool _flush = true);

    void Flush(ERHIFlushDepth _depth = ERHIFlushDepth::SubmitGPU);
    // Blocking lifecycle boundaries must not be called recursively from an
    // RHI completion callback whose completion they are waiting for.
    void Sync(ERHISyncDepth _depth = ERHISyncDepth::RHI);
    static void ShutDown();

private:
    enum class ELifecycleState : uint8 {
        Stopped,
        Running,
        Stopping,
    };

    std::shared_ptr<RHIBackendExecutor> GetBackendExecutorLocked();
    RHIBackendSubmissionBatch          TakePendingBatchLocked();
    void SubmitReady(
        Array<RHIBackendSubmissionBatchEntry>&& _submits,
        ERHIExecSubmitFlags                      _flags,
        RHIPresentRequest*                       _present
    );
    void FlushReady(ERHIFlushDepth _depth);
    void SyncReady(ERHISyncDepth _depth);

    std::mutex                              submit_mutex;
    std::condition_variable                 lifecycle_cv;
    std::mutex                              dispatch_mutex;
    std::shared_ptr<RHIBackendExecutor>     backend_executor{};
    Array<RHIBackendSubmissionBatchEntry>   pending_submits{};
    std::optional<RHIPresentRequest>         pending_present{};
    uint64                                   next_batch_sequence{1};
    size_t                                   active_sync_calls{0};
    uint64                                   shutdown_generation{0};
    uint64                                   completed_shutdown_generation{0};
    ELifecycleState                          lifecycle_state{ELifecycleState::Stopped};
    RHIExecutorRecordingHandoffQueue         recording_handoff{};
};

} // namespace Moer::Render
