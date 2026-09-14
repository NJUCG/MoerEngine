#include "rendergraph/RenderGraph.h"

#include "profile/ProfileScope.h"
#include "rhi/RHIThreadOwnership.h"
#include "taskgraph/TaskGraph.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace Moer::Render {

namespace {

[[nodiscard]] const char* RecordingClassName(
    RenderGraph::PassExecutionClass execution
) noexcept {
    switch (execution) {
        case RenderGraph::PassExecutionClass::MainThread:
            return "main-thread";
        case RenderGraph::PassExecutionClass::CpuPrepare:
            return "cpu-prepare";
        case RenderGraph::PassExecutionClass::ExternalControl:
            return "external-control";
        case RenderGraph::PassExecutionClass::SerialRecord:
            return "serial-record";
        case RenderGraph::PassExecutionClass::ParallelRecordEligible:
            return "parallel-record";
    }
    return "unknown";
}

[[nodiscard]] RHIQueueBinding GraphicsQueueBinding(
    const RenderGraph::QueueBinding& binding
) noexcept {
    return RHIQueueBinding{
        .queue           = EQueueType::Graphics,
        .native_queue_id = binding.native_queue_id,
        .family_id       = binding.family_id,
        .available       = binding.available,
    };
}

struct MergedRecordingCompletion {
    explicit MergedRecordingCompletion(size_t job_count) : remaining(job_count) {}

    void Finish(std::string error_message = {}) noexcept {
        bool notify = false;
        {
            std::lock_guard lock(mutex);
            if (error.empty() && !error_message.empty()) {
                error = std::move(error_message);
            }
            if (remaining != 0) {
                --remaining;
            }
            notify = remaining == 0;
        }
        if (notify) {
            cv.notify_all();
        }
    }

    [[nodiscard]] bool Wait(std::string& out_error) {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return remaining == 0; });
        out_error = error;
        return error.empty();
    }

private:
    std::mutex              mutex{};
    std::condition_variable cv{};
    size_t                  remaining = 0;
    std::string             error{};
};

struct MergedRecordingJob {
    std::string                 pass_name{};
    RenderGraph::RecordCallback record{};
    SharedPtr<CommandList>      command_list{};
    bool                        gpu_profile_requested = false;
    bool                        gpu_profile_bound     = false;
    ERHITranslateExecutionClass translate_execution_class{
        ERHITranslateExecutionClass::Parallel
    };
};

} // namespace

RenderGraph::ExecutedPassInfo RenderGraph::MakeExecutedPassInfo(
    PassHandle pass_handle
) const {
    const auto& pass = passes[pass_handle.index];
    return ExecutedPassInfo{
        .handle          = pass_handle,
        .name            = pass.name,
        .domain          = pass.domain,
        .side_effect     = pass.side_effect,
        .execution_class = pass.execution_class,
        .translate_execution_class = pass.translate_execution_class,
    };
}

RenderGraph::GpuProfileBindOutcome RenderGraph::BindGpuProfileSource(
    const GpuProfilingOptions& options,
    const ExecutedPassInfo&    pass_info,
    CommandList&               command_list,
    RHIQueueBinding            queue_binding,
    uint64                     source_order
) {
    if (!options.try_bind_source) {
        return GpuProfileBindOutcome::Dropped;
    }
    if (!command_list.IsEmpty() || command_list.HasGpuScopeRecorder() ||
        command_list.IsLegacyGpuProfilingSuppressedForGeneration()) {
        compile_error =
            "GPU profiling source binding requires an empty, unbound, "
            "unsuppressed CommandList for pass '" +
            std::string(pass_info.name) + "'";
        return GpuProfileBindOutcome::Failed;
    }

    const uint64 seal_generation = command_list.GetSealGeneration();
    const bool explicit_resource_state =
        command_list.HasExplicitResourceStateOwnership();
    const ERHITranslateExecutionClass translate_execution =
        command_list.GetTranslateExecutionClass();
    const bool legacy_profiling_suppressed =
        command_list.IsLegacyGpuProfilingSuppressedForGeneration();

    bool bound = false;
    try {
        RHIThreadRoleScope configuration_owner(ERHIThreadRole::RecordWorker);
        bound = options.try_bind_source(
            pass_info,
            command_list,
            queue_binding,
            source_order
        );
    } catch (const std::exception& exception) {
        compile_error =
            "GPU profiling source binding failed for pass '" +
            std::string(pass_info.name) + "': " + exception.what();
        return GpuProfileBindOutcome::Failed;
    } catch (...) {
        compile_error =
            "GPU profiling source binding failed for pass '" +
            std::string(pass_info.name) + "'";
        return GpuProfileBindOutcome::Failed;
    }

    if (!command_list.IsEmpty() ||
        command_list.GetSealGeneration() != seal_generation ||
        command_list.HasExplicitResourceStateOwnership() !=
            explicit_resource_state ||
        command_list.GetTranslateExecutionClass() != translate_execution ||
        command_list.IsLegacyGpuProfilingSuppressedForGeneration() !=
            legacy_profiling_suppressed ||
        command_list.HasGpuScopeRecorder() != bound) {
        compile_error =
            "GPU profiling source binding illegally mutated CommandList state "
            "for pass '" +
            std::string(pass_info.name) + "'";
        return GpuProfileBindOutcome::Failed;
    }
    if (bound) {
        return GpuProfileBindOutcome::Bound;
    }

    try {
        command_list.SuppressLegacyGpuProfilingForGeneration();
    } catch (const std::exception& exception) {
        compile_error =
            "GPU profiling source drop suppression failed for pass '" +
            std::string(pass_info.name) + "': " + exception.what();
        return GpuProfileBindOutcome::Failed;
    } catch (...) {
        compile_error =
            "GPU profiling source drop suppression failed for pass '" +
            std::string(pass_info.name) + "'";
        return GpuProfileBindOutcome::Failed;
    }
    return GpuProfileBindOutcome::Dropped;
}

class RenderGraph::MergedRecordingExecutor {
public:
    MergedRecordingExecutor(
        RenderGraph&               graph,
        CommandList&               destination,
        bool                       parallel_recording_enabled,
        const GpuProfilingOptions& gpu_profiling
    ) :
        graph(graph),
        destination(destination),
        parallel_recording_enabled(parallel_recording_enabled),
        gpu_profiling(gpu_profiling) {
    }

    [[nodiscard]] bool Execute() {
        if (!Validate()) {
            return false;
        }

        MOER_PROFILE_SCOPE("RenderGraph.ExecuteRecordingMerged");
        graph.executed = true;
        try {
            recorded_lists.resize(
                graph.compiled_plan.recording_batches.size()
            );
        } catch (const std::exception& exception) {
            return Fail(
                std::string("failed to allocate recording slots: ") +
                exception.what()
            );
        } catch (...) {
            return Fail("failed to allocate recording slots");
        }

        for (const CompiledRecordingGroup& group :
             graph.compiled_plan.recording_groups) {
            if (!RecordGroup(group)) {
                return false;
            }
        }
        return MergeRecordedLists();
    }

private:
    [[nodiscard]] bool Fail(std::string message) {
        graph.compile_error = std::move(message);
        return false;
    }

    [[nodiscard]] bool Validate() {
        if (!graph.compiled) {
            return Fail(
                "ExecuteRecordingMerged called before a successful Compile"
            );
        }
        if (graph.executed) {
            return Fail(
                "a per-frame RenderGraph can only be executed once"
            );
        }
        if (destination.GetQueueType() != EQueueType::Graphics) {
            return Fail(
                "ExecuteRecordingMerged requires a Graphics CommandList"
            );
        }
        if (destination.HasExplicitResourceStateOwnership()) {
            return Fail(
                "ExecuteRecordingMerged requires a backend-tracked "
                "destination CommandList"
            );
        }

        const bool has_allocation_backed_transient = std::any_of(
            graph.compiled_plan.resources.begin(),
            graph.compiled_plan.resources.end(),
            [](const CompiledResource& resource) {
                return !resource.imported &&
                       resource.first_use != PassHandle::InvalidIndex &&
                       resource.transient_slot != PassHandle::InvalidIndex;
            }
        );
        if (has_allocation_backed_transient) {
            return Fail(
                "allocation-backed transient resources require active "
                "ExecuteRecording"
            );
        }

        for (const CompiledRecordingBatch& batch :
             graph.compiled_plan.recording_batches) {
            if (!ValidateBatch(batch)) {
                return false;
            }
        }
        return ValidateGroups();
    }

    [[nodiscard]] bool ValidateBatch(
        const CompiledRecordingBatch& batch
    ) {
        if (batch.passes.size() != 1) {
            return Fail(
                "merged recording requires one pass per compiled recording "
                "batch"
            );
        }
        const PassHandle pass_handle = batch.passes.front();
        const auto&      pass = graph.passes[pass_handle.index];
        if (batch.execution != PassExecutionClass::SerialRecord &&
            batch.execution != PassExecutionClass::ParallelRecordEligible) {
            return Fail(
                "merged recording only supports record-class passes; pass '" +
                pass.name + "' uses " + RecordingClassName(batch.execution)
            );
        }
        if (!pass.record || pass.execute) {
            return Fail(
                "merged recording pass has an invalid callback shape: " +
                pass.name
            );
        }
        if (pass.domain.queue != QueueRole::Graphics) {
            return Fail(
                "merged recording only supports Graphics passes; pass '" +
                pass.name + "' declared another queue"
            );
        }
        if (gpu_profiling.try_bind_source &&
            gpu_profiling.source_order_base >
                std::numeric_limits<uint64>::max() -
                    static_cast<uint64>(batch.id)) {
            return Fail("GPU profiling source order overflow");
        }
        return true;
    }

    [[nodiscard]] bool ValidateGroups() {
        size_t expected_first_batch = 0;
        for (const CompiledRecordingGroup& group :
             graph.compiled_plan.recording_groups) {
            if (group.first_batch != expected_first_batch ||
                group.batch_count == 0 ||
                group.first_batch + group.batch_count >
                    graph.compiled_plan.recording_batches.size()) {
                return Fail("compiled recording groups are inconsistent");
            }
            expected_first_batch += group.batch_count;
        }
        if (expected_first_batch !=
            graph.compiled_plan.recording_batches.size()) {
            return Fail(
                "compiled recording groups do not cover every pass"
            );
        }
        return true;
    }

    [[nodiscard]] bool CreateJobs(
        const CompiledRecordingGroup& group,
        Array<MergedRecordingJob>&    jobs
    ) {
        try {
            jobs.reserve(group.batch_count);
            for (uint32_t offset = 0; offset < group.batch_count; ++offset) {
                const size_t batch_index = group.first_batch + offset;
                const auto&  batch =
                    graph.compiled_plan.recording_batches[batch_index];
                const PassHandle pass_handle = batch.passes.front();
                const auto&      pass = graph.passes[pass_handle.index];

                auto pass_command_list =
                    MakeShared<CommandList>(EQueueType::Graphics);
                pass_command_list->SetTranslateExecutionClass(
                    batch.translate_execution_class
                );

                bool gpu_profile_bound = false;
                if (gpu_profiling.try_bind_source) {
                    const GpuProfileBindOutcome bind_outcome =
                        graph.BindGpuProfileSource(
                            gpu_profiling,
                            graph.MakeExecutedPassInfo(pass_handle),
                            *pass_command_list,
                            GraphicsQueueBinding(batch.queue),
                            gpu_profiling.source_order_base +
                                static_cast<uint64>(batch.id)
                        );
                    if (bind_outcome == GpuProfileBindOutcome::Failed) {
                        return false;
                    }
                    gpu_profile_bound =
                        bind_outcome == GpuProfileBindOutcome::Bound;
                }

                recorded_lists[batch_index] = pass_command_list;
                jobs.emplace_back(MergedRecordingJob{
                    .pass_name             = pass.name,
                    .record                = pass.record,
                    .command_list          = std::move(pass_command_list),
                    .gpu_profile_requested =
                        static_cast<bool>(gpu_profiling.try_bind_source),
                    .gpu_profile_bound = gpu_profile_bound,
                    .translate_execution_class =
                        batch.translate_execution_class,
                });
            }
        } catch (const std::exception& exception) {
            return Fail(
                std::string("failed to create merged recording batch: ") +
                exception.what()
            );
        } catch (...) {
            return Fail("failed to create merged recording batch");
        }
        return true;
    }

    static void RunJob(
        MergedRecordingJob                         job,
        const SharedPtr<MergedRecordingCompletion>& completion
    ) noexcept {
        auto fail = [&](std::string_view detail) noexcept {
            try {
                std::string message =
                    "record pass '" + job.pass_name + "' failed";
                if (!detail.empty()) {
                    message += ": ";
                    message += detail;
                }
                completion->Finish(std::move(message));
            } catch (...) {
                completion->Finish("recording batch failed");
            }
        };

        try {
            RHIThreadRoleScope record_owner(ERHIThreadRole::RecordWorker);
            ProfileDump::ScopedCpuProfile profile_scope(
                "RenderGraph.Record",
                job.pass_name
            );

            const uint64 seal_generation =
                job.command_list->GetSealGeneration();
            if (job.gpu_profile_requested) {
                ScopedGpuMarker pass_marker(
                    *job.command_list,
                    job.pass_name,
                    GpuMarkerPalette::Pass(),
                    job.gpu_profile_bound ?
                        EGpuMarkerMode::Timestamp :
                        EGpuMarkerMode::Label
                );
                job.record(*job.command_list);
            } else {
                job.record(*job.command_list);
            }

            if (job.command_list->GetSealGeneration() != seal_generation) {
                throw std::logic_error(
                    "record callback sealed its CommandList"
                );
            }
            if (job.command_list->HasExplicitResourceStateOwnership()) {
                throw std::logic_error(
                    "record callback changed resource-state ownership"
                );
            }
            if (job.command_list->GetTranslateExecutionClass() !=
                job.translate_execution_class) {
                throw std::logic_error(
                    "record callback changed its translation class"
                );
            }

            auto lifetime =
                std::make_shared<RecordCallback>(std::move(job.record));
            job.command_list->AddCallback([lifetime] {});
            job.command_list->AddSuccessCallback([lifetime] {});
            completion->Finish();
        } catch (const std::exception& exception) {
            fail(exception.what());
        } catch (...) {
            fail({});
        }
    }

    [[nodiscard]] bool RecordGroup(
        const CompiledRecordingGroup& group
    ) {
        Array<MergedRecordingJob> jobs{};
        if (!CreateJobs(group, jobs)) {
            return false;
        }

        SharedPtr<MergedRecordingCompletion> completion{};
        try {
            completion =
                std::make_shared<MergedRecordingCompletion>(jobs.size());
        } catch (const std::exception& exception) {
            return Fail(
                std::string("failed to create recording completion: ") +
                exception.what()
            );
        } catch (...) {
            return Fail("failed to create recording completion");
        }

        const bool dispatch_parallel =
            group.execution == PassExecutionClass::ParallelRecordEligible &&
            parallel_recording_enabled && TaskGraph::IsInitialized() &&
            jobs.size() > 1;
        GraphEventArray record_events{};
        if (!dispatch_parallel) {
            for (auto& job : jobs) {
                RunJob(std::move(job), completion);
            }
        } else {
            record_events.reserve(jobs.size());
            size_t dispatched_count = 0;
            try {
                for (; dispatched_count < jobs.size(); ++dispatched_count) {
                    record_events.emplace_back(LambdaTask::Dispatch(
                        [job = std::move(jobs[dispatched_count]),
                         completion]() mutable {
                            RunJob(std::move(job), completion);
                        }
                    ));
                }
            } catch (const std::exception& exception) {
                const size_t undispatched_count =
                    jobs.size() - dispatched_count;
                for (size_t index = 0; index < undispatched_count; ++index) {
                    completion->Finish(
                        std::string("failed to dispatch recording task: ") +
                        exception.what()
                    );
                }
            } catch (...) {
                const size_t undispatched_count =
                    jobs.size() - dispatched_count;
                for (size_t index = 0; index < undispatched_count; ++index) {
                    completion->Finish(
                        "failed to dispatch recording task"
                    );
                }
            }
        }

        std::string group_error{};
        if (!completion->Wait(group_error)) {
            return Fail(std::move(group_error));
        }
        return true;
    }

    [[nodiscard]] bool MergeRecordedLists() {
        try {
            for (auto& recorded_list : recorded_lists) {
                if (!recorded_list) {
                    throw std::logic_error(
                        "recording batch left an empty CommandList slot"
                    );
                }
                destination.AppendRecorded(std::move(*recorded_list));
            }
        } catch (const std::exception& exception) {
            return Fail(
                std::string("failed to merge frontend CommandLists: ") +
                exception.what()
            );
        } catch (...) {
            return Fail("failed to merge frontend CommandLists");
        }
        return true;
    }

    RenderGraph&               graph;
    CommandList&               destination;
    bool                       parallel_recording_enabled = true;
    const GpuProfilingOptions& gpu_profiling;
    Array<SharedPtr<CommandList>> recorded_lists{};
};

bool RenderGraph::ExecuteRecordingMerged(
    CommandList&               command_list,
    bool                       parallel_recording_enabled,
    const GpuProfilingOptions& gpu_profiling
) {
    return MergedRecordingExecutor(
        *this,
        command_list,
        parallel_recording_enabled,
        gpu_profiling
    ).Execute();
}

} // namespace Moer::Render
