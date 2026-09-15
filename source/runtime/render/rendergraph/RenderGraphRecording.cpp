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

// Group-local join state. Every job, including one that failed to dispatch,
// completes one slot; the first error is reported after the group becomes terminal.
struct FrontendGroupCompletion {
    explicit FrontendGroupCompletion(size_t job_count) : remaining(job_count) {}

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

struct FrontendRecordJob {
    std::string                 pass_name{};
    RenderGraph::RecordCallback record_callback{};
    SharedPtr<CommandList>      frontend_command_list{};
    bool                        gpu_profile_requested = false;
    bool                        gpu_profile_bound     = false;
    ERHITranslateExecutionClass native_translate_class{
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

// Bind profiling to an empty stream and verify that the hook changed no
// recording contract beyond installing its recorder.
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

class RenderGraph::FrontendRecordMergeExecutor {
public:
    FrontendRecordMergeExecutor(
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

        MOER_PROFILE_SCOPE("RenderGraph.RecordAndMergeFrontendCommands");
        graph.executed = true;
        // Allocate stable result slots before any worker starts recording.
        try {
            frontend_streams_by_unit.resize(
                graph.compiled_plan.frontend_record_units.size()
            );
        } catch (const std::exception& exception) {
            return Fail(
                std::string("failed to allocate recording slots: ") +
                exception.what()
            );
        } catch (...) {
            return Fail("failed to allocate recording slots");
        }

        // Respect compiler-defined CPU boundaries; commit to the destination
        // only after every group has recorded successfully.
        for (const CompiledFrontendDispatchGroup& group :
             graph.compiled_plan.frontend_dispatch_groups) {
            if (!RecordFrontendDispatchGroup(group)) {
                return false;
            }
        }
        return AppendFrontendStreamsInCompiledOrder();
    }

private:
    [[nodiscard]] bool Fail(std::string message) {
        graph.compile_error = std::move(message);
        return false;
    }

    [[nodiscard]] bool Validate() {
        if (!graph.compiled) {
            return Fail(
                "RecordAndMergeFrontendCommands called before a successful Compile"
            );
        }
        if (graph.executed) {
            return Fail(
                "a per-frame RenderGraph can only be executed once"
            );
        }
        if (destination.GetQueueType() != EQueueType::Graphics) {
            return Fail(
                "RecordAndMergeFrontendCommands requires a Graphics CommandList"
            );
        }
        if (destination.HasExplicitResourceStateOwnership()) {
            return Fail(
                "RecordAndMergeFrontendCommands requires a backend-tracked "
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
                "ExecuteFrontendRecordingPlan"
            );
        }

        for (const CompiledFrontendRecordUnit& unit :
             graph.compiled_plan.frontend_record_units) {
            if (!ValidateRecordUnit(unit)) {
                return false;
            }
        }
        return ValidateDispatchGroups();
    }

    [[nodiscard]] bool ValidateRecordUnit(
        const CompiledFrontendRecordUnit& unit
    ) {
        const PassHandle pass_handle = unit.pass;
        const auto&      pass = graph.passes[pass_handle.index];
        if (unit.record_execution_class != PassExecutionClass::SerialRecord &&
            unit.record_execution_class !=
                PassExecutionClass::ParallelRecordEligible) {
            return Fail(
                "merged recording only supports record-class passes; pass '" +
                pass.name + "' uses " +
                    RecordingClassName(unit.record_execution_class)
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
                    static_cast<uint64>(unit.source_index)) {
            return Fail("GPU profiling source order overflow");
        }
        return true;
    }

    [[nodiscard]] bool ValidateDispatchGroups() {
        size_t expected_first_unit = 0;
        for (const CompiledFrontendDispatchGroup& group :
             graph.compiled_plan.frontend_dispatch_groups) {
            if (group.first_unit != expected_first_unit ||
                group.unit_count == 0 ||
                group.first_unit + group.unit_count >
                    graph.compiled_plan.frontend_record_units.size()) {
                return Fail("compiled frontend dispatch groups are inconsistent");
            }
            expected_first_unit += group.unit_count;
        }
        if (expected_first_unit !=
            graph.compiled_plan.frontend_record_units.size()) {
            return Fail(
                "compiled frontend dispatch groups do not cover every pass"
            );
        }
        return true;
    }

    [[nodiscard]] bool CreateFrontendRecordJobs(
        const CompiledFrontendDispatchGroup& group,
        Array<FrontendRecordJob>&             jobs
    ) {
        // Materialize one job and output slot per pass; callbacks do not run here.
        try {
            jobs.reserve(group.unit_count);
            for (uint32_t offset = 0; offset < group.unit_count; ++offset) {
                const size_t unit_index = group.first_unit + offset;
                const auto&  unit =
                    graph.compiled_plan.frontend_record_units[unit_index];
                const PassHandle pass_handle = unit.pass;
                const auto&      pass = graph.passes[pass_handle.index];

                auto pass_command_list =
                    MakeShared<CommandList>(EQueueType::Graphics);
                pass_command_list->SetTranslateExecutionClass(
                    unit.native_translate_class
                );

                bool gpu_profile_bound = false;
                if (gpu_profiling.try_bind_source) {
                    const GpuProfileBindOutcome bind_outcome =
                        graph.BindGpuProfileSource(
                            gpu_profiling,
                            graph.MakeExecutedPassInfo(pass_handle),
                            *pass_command_list,
                            GraphicsQueueBinding(unit.target_queue),
                            gpu_profiling.source_order_base +
                                static_cast<uint64>(unit.source_index)
                        );
                    if (bind_outcome == GpuProfileBindOutcome::Failed) {
                        return false;
                    }
                    gpu_profile_bound =
                        bind_outcome == GpuProfileBindOutcome::Bound;
                }

                frontend_streams_by_unit[unit_index] = pass_command_list;
                jobs.emplace_back(FrontendRecordJob{
                    .pass_name       = pass.name,
                    .record_callback = pass.record,
                    .frontend_command_list = std::move(pass_command_list),
                    .gpu_profile_requested =
                        static_cast<bool>(gpu_profiling.try_bind_source),
                    .gpu_profile_bound = gpu_profile_bound,
                    .native_translate_class = unit.native_translate_class,
                });
            }
        } catch (const std::exception& exception) {
            return Fail(
                std::string("failed to create frontend record jobs: ") +
                exception.what()
            );
        } catch (...) {
            return Fail("failed to create frontend record jobs");
        }
        return true;
    }

    static void RecordFrontendPass(
        FrontendRecordJob                         job,
        const SharedPtr<FrontendGroupCompletion>& completion
    ) noexcept {
        // Record exactly one pass into its exclusively owned frontend CommandList.
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
                completion->Finish("frontend record job failed");
            }
        };

        try {
            RHIThreadRoleScope record_owner(ERHIThreadRole::RecordWorker);
            ProfileDump::ScopedCpuProfile profile_scope(
                "RenderGraph.Record",
                job.pass_name
            );

            const uint64 seal_generation =
                job.frontend_command_list->GetSealGeneration();
            if (job.gpu_profile_requested) {
                ScopedGpuMarker pass_marker(
                    *job.frontend_command_list,
                    job.pass_name,
                    GpuMarkerPalette::Pass(),
                    job.gpu_profile_bound ?
                        EGpuMarkerMode::Timestamp :
                        EGpuMarkerMode::Label
                );
                job.record_callback(*job.frontend_command_list);
            } else {
                job.record_callback(*job.frontend_command_list);
            }

            if (job.frontend_command_list->GetSealGeneration() !=
                seal_generation) {
                throw std::logic_error(
                    "record callback sealed its CommandList"
                );
            }
            if (job.frontend_command_list->HasExplicitResourceStateOwnership()) {
                throw std::logic_error(
                    "record callback changed resource-state ownership"
                );
            }
            if (job.frontend_command_list->GetTranslateExecutionClass() !=
                job.native_translate_class) {
                throw std::logic_error(
                    "record callback changed its translation class"
                );
            }

            auto lifetime = std::make_shared<RecordCallback>(
                std::move(job.record_callback)
            );
            job.frontend_command_list->AddCallback([lifetime] {});
            job.frontend_command_list->AddSuccessCallback([lifetime] {});
            completion->Finish();
        } catch (const std::exception& exception) {
            fail(exception.what());
        } catch (...) {
            fail({});
        }
    }

    /**
     * Records one compiler-defined group into independent frontend CommandLists.
     * Eligible passes may run concurrently; their results are retained in
     * compiled order for the later merge. This does not translate or submit GPU work.
     */
    [[nodiscard]] bool RecordFrontendDispatchGroup(
        const CompiledFrontendDispatchGroup& group
    ) {
        // Create one independently owned frontend CommandList per pass.
        Array<FrontendRecordJob> jobs{};
        if (!CreateFrontendRecordJobs(group, jobs)) {
            return false;
        }

        SharedPtr<FrontendGroupCompletion> group_completion{};
        try {
            group_completion =
                std::make_shared<FrontendGroupCompletion>(jobs.size());
        } catch (const std::exception& exception) {
            return Fail(
                std::string("failed to create recording completion: ") +
                exception.what()
            );
        } catch (...) {
            return Fail("failed to create recording completion");
        }

        const bool can_dispatch_parallel =
            group.record_execution_class ==
                PassExecutionClass::ParallelRecordEligible &&
            parallel_recording_enabled && TaskGraph::IsInitialized() &&
            jobs.size() > 1;
        GraphEventArray record_task_events{};
        // Avoid task-dispatch overhead when the group cannot expose concurrency.
        if (!can_dispatch_parallel) {
            for (auto& job : jobs) {
                RecordFrontendPass(std::move(job), group_completion);
            }
        } else {
            record_task_events.reserve(jobs.size());
            size_t dispatched_count = 0;
            try {
                for (; dispatched_count < jobs.size(); ++dispatched_count) {
                    record_task_events.emplace_back(LambdaTask::Dispatch(
                        [job = std::move(jobs[dispatched_count]),
                         group_completion]() mutable {
                            RecordFrontendPass(
                                std::move(job), group_completion
                            );
                        }
                    ));
                }
            } catch (const std::exception& exception) {
                const size_t undispatched_count =
                    jobs.size() - dispatched_count;
                for (size_t index = 0; index < undispatched_count; ++index) {
                    group_completion->Finish(
                        std::string("failed to dispatch recording task: ") +
                        exception.what()
                    );
                }
            } catch (...) {
                const size_t undispatched_count =
                    jobs.size() - dispatched_count;
                for (size_t index = 0; index < undispatched_count; ++index) {
                    group_completion->Finish(
                        "failed to dispatch recording task"
                    );
                }
            }
        }

        std::string group_error{};
        // Join at the compiler-defined CPU recording boundary before continuing.
        if (!group_completion->Wait(group_error)) {
            return Fail(std::move(group_error));
        }
        return true;
    }

    [[nodiscard]] bool AppendFrontendStreamsInCompiledOrder() {
        // All recording succeeded; append streams once in compiled unit order.
        try {
            for (auto& frontend_stream : frontend_streams_by_unit) {
                if (!frontend_stream) {
                    throw std::logic_error(
                        "frontend record unit left an empty CommandList slot"
                    );
                }
                destination.AppendRecorded(std::move(*frontend_stream));
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
    Array<SharedPtr<CommandList>> frontend_streams_by_unit{};
};

bool RenderGraph::RecordAndMergeFrontendCommands(
    CommandList&               command_list,
    bool                       parallel_recording_enabled,
    const GpuProfilingOptions& gpu_profiling
) {
    return FrontendRecordMergeExecutor(
        *this,
        command_list,
        parallel_recording_enabled,
        gpu_profiling
    ).Execute();
}

} // namespace Moer::Render
