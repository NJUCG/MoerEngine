#include "rendergraph/RenderGraph.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rendergraph/RenderGraphRecordingPlan.h"

#include "log/LogSystem.h"
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

struct FrontendRecordingLifetime {
    std::vector<RenderGraph::RecordCallback> callbacks{};
    std::vector<RenderGraphLowering::PhysicalBinding> keepalive{};
};

struct FrontendRecordJob {
    Detail::RecordingRange      range{};
    SharedPtr<CommandList>      frontend_command_list{};
    std::shared_ptr<FrontendRecordingLifetime> lifetime{};
    ERHIResourceStateOwnership state_ownership = ERHIResourceStateOwnership::BackendTracked;
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
    const auto& pass = passes[pass_handle.pass_id];
    return ExecutedPassInfo{
        .handle          = pass_handle,
        .name            = pass.name,
        .domain          = pass.domain,
        .side_effect     = pass.side_effect,
        .record_policy = pass.record_policy,
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
        // Only range-sized streams are retained; no per-pass CommandList slots.
        try {
            frontend_streams.reserve(
                graph.compiled_plan.frontend_record_units.size()
            );
            const auto dispatches = Detail::BuildRecordingDispatches(
                graph.compiled_plan, parallel_recording_enabled
            );
            // Respect CPU recording boundaries; commit only after every range succeeds.
            for (const auto& dispatch : dispatches) {
                if (!RecordFrontendDispatchGroup(dispatch)) {
                    return false;
                }
            }
        } catch (const std::exception& exception) {
            return Fail(
                std::string("failed to prepare frontend recording: ") +
                exception.what()
            );
        } catch (...) {
            return Fail("failed to prepare frontend recording");
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

        const bool has_allocation_backed_transient = std::any_of(
            graph.compiled_plan.resources.begin(),
            graph.compiled_plan.resources.end(),
            [](const CompiledResource& resource) {
                return !resource.imported &&
                       resource.first_use != InvalidIndex &&
                       resource.transient_slot != InvalidIndex;
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
        if (!ValidateDispatchGroups()) { return false; }
        if (destination.GetResourceStateOwnership() == ERHIResourceStateOwnership::Compiled) {
            if (!RenderGraphLowering::Lower(graph, lowered, graph.compile_error)) { return false; }
            if (!lowered.queue_syncs.empty()) { return Fail("merged recording cannot execute queue synchronization"); }
            // Materialize before launching workers so invalid bindings cannot leave a partial frame.
            before.resize(graph.passes.size());
            after.resize(graph.passes.size());
            const auto append = [&](const auto& instruction, auto& barriers) {
                std::optional<BarrierCreateInfo> barrier;
                if (!RenderGraphLowering::MaterializeInstruction(instruction, barrier, graph.compile_error)) {
                    return false;
                }
                if (barrier) { barriers.push_back(std::move(*barrier)); }
                return true;
            };
            for (const auto& instruction : lowered.prologue) {
                if (!append(instruction, before.at(instruction.dst_pass.pass_id))) { return false; }
            }
            for (const auto& pass : lowered.passes) {
                for (const auto& instruction : pass.before) {
                    if (!append(instruction, before.at(pass.pass.pass_id))) { return false; }
                }
                for (const auto& instruction : pass.after) {
                    if (!append(instruction, after.at(pass.pass.pass_id))) { return false; }
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool ValidateRecordUnit(
        const CompiledFrontendRecordUnit& unit
    ) {
        const PassHandle pass_handle = unit.pass;
        const auto&      pass = graph.passes[pass_handle.pass_id];
        if (!pass.record) {
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
        const Detail::RecordingDispatch& dispatch,
        Array<FrontendRecordJob>&         jobs
    ) {
        try {
            const uint32_t worker_count = TaskGraph::IsInitialized() ?
                TaskGraph::GetInterface().GetWorkerThreadCount() : 1;
            const auto ranges = Detail::PartitionRecordingRange(
                graph.compiled_plan.frontend_record_units, dispatch, worker_count
            );
            jobs.reserve(ranges.size());
            for (const auto range : ranges) {
                const auto& first_unit =
                    graph.compiled_plan.frontend_record_units[range.first_unit];
                const auto ownership = destination.GetResourceStateOwnership();
                auto command_list = MakeShared<CommandList>(EQueueType::Graphics);
                command_list->SetTranslateExecutionClass(first_unit.native_translate_class);
                command_list->SetResourceStateOwnership(ownership);

                auto lifetime = std::make_shared<FrontendRecordingLifetime>();
                lifetime->callbacks.reserve(range.unit_count);
                for (uint32_t offset = 0; offset < range.unit_count; ++offset) {
                    const auto pass_handle =
                        graph.compiled_plan.frontend_record_units[range.first_unit + offset].pass;
                    lifetime->callbacks.push_back(graph.passes[pass_handle.pass_id].record);
                    if (ownership == ERHIResourceStateOwnership::Compiled) {
                        const auto resources = lowered.Keepalive(pass_handle);
                        lifetime->keepalive.insert(
                            lifetime->keepalive.end(), resources.begin(), resources.end()
                        );
                    }
                }
                bool gpu_profile_bound = false;
                if (gpu_profiling.try_bind_source) {
                    const GpuProfileBindOutcome bind_outcome = graph.BindGpuProfileSource(
                        gpu_profiling,
                        graph.MakeExecutedPassInfo(first_unit.pass),
                        *command_list,
                        GraphicsQueueBinding(first_unit.target_queue),
                        gpu_profiling.source_order_base + static_cast<uint64>(first_unit.source_index)
                    );
                    if (bind_outcome == GpuProfileBindOutcome::Failed) { return false; }
                    gpu_profile_bound = bind_outcome == GpuProfileBindOutcome::Bound;
                }
                // Bind profiling while empty, then retain the complete range before recording it.
                command_list->KeepAlive(lifetime);

                frontend_streams.emplace_back(command_list);
                jobs.emplace_back(FrontendRecordJob{
                    .range = range,
                    .frontend_command_list = std::move(command_list),
                    .lifetime = std::move(lifetime),
                    .state_ownership = ownership,
                    .gpu_profile_requested = static_cast<bool>(gpu_profiling.try_bind_source),
                    .gpu_profile_bound = gpu_profile_bound,
                    .native_translate_class = first_unit.native_translate_class,
                });
            }
        } catch (const std::exception& exception) {
            return Fail(std::string("failed to create frontend record jobs: ") + exception.what());
        } catch (...) {
            return Fail("failed to create frontend record jobs");
        }
        return true;
    }

    [[nodiscard]] std::string RecordFrontendRange(FrontendRecordJob job) noexcept {
        std::string_view pass_name{};
        try {
            RHIThreadRoleScope record_owner(ERHIThreadRole::RecordWorker);
            CommandList& commands = *job.frontend_command_list;
            for (uint32_t offset = 0; offset < job.range.unit_count; ++offset) {
                const auto handle =
                    graph.compiled_plan.frontend_record_units[job.range.first_unit + offset].pass;
                pass_name = graph.passes[handle.pass_id].name;
                ProfileDump::ScopedCpuProfile profile_scope("RenderGraph.Record", pass_name);
                const uint64 seal_generation = commands.GetSealGeneration();
                const auto record_body = [&] {
                    if (job.state_ownership == ERHIResourceStateOwnership::Compiled &&
                        !before[handle.pass_id].empty()) {
                        commands.Barriers(before[handle.pass_id]);
                    }
                    job.lifetime->callbacks[offset](commands);
                    if (job.state_ownership == ERHIResourceStateOwnership::Compiled &&
                        !after[handle.pass_id].empty()) {
                        commands.Barriers(after[handle.pass_id]);
                    }
                };
                if (job.gpu_profile_requested) {
                    ScopedGpuMarker pass_marker(
                        commands, pass_name, GpuMarkerPalette::Pass(),
                        job.gpu_profile_bound ? EGpuMarkerMode::Timestamp : EGpuMarkerMode::Label
                    );
                    record_body();
                } else {
                    record_body();
                }
                // Validate after every callback, before another pass can write the shared stream.
                if (commands.GetSealGeneration() != seal_generation) {
                    throw std::logic_error("record callback sealed its CommandList");
                }
                if (commands.GetResourceStateOwnership() != job.state_ownership) {
                    throw std::logic_error("record callback changed resource-state ownership");
                }
                if (commands.GetTranslateExecutionClass() != job.native_translate_class) {
                    throw std::logic_error("record callback changed its translation class");
                }
            }
            return {};
        } catch (const std::exception& exception) {
            try {
                return "record pass '" + std::string(pass_name) + "' failed: " + exception.what();
            } catch (...) {
                return "frontend record job failed";
            }
        } catch (...) {
            try {
                return "record pass '" + std::string(pass_name) + "' failed";
            } catch (...) {
                return "frontend record job failed";
            }
        }
    }

    [[nodiscard]] bool RecordFrontendDispatchGroup(const Detail::RecordingDispatch& dispatch) {
        Array<FrontendRecordJob> jobs{};
        if (!CreateFrontendRecordJobs(dispatch, jobs)) { return false; }
        const bool can_dispatch_parallel = dispatch.parallel && jobs.size() > 1;
        // Serial regions share a private stream and need neither a task nor a completion mutex.
        if (!can_dispatch_parallel) {
            for (auto& job : jobs) {
                auto error = RecordFrontendRange(std::move(job));
                if (!error.empty()) { return Fail(std::move(error)); }
            }
            return true;
        }

        auto completion = std::make_shared<FrontendGroupCompletion>(jobs.size());
        GraphEventArray record_task_events{};
        record_task_events.reserve(jobs.size());
        size_t dispatched_count = 0;
        std::exception_ptr dispatch_error;
        try {
            for (; dispatched_count < jobs.size(); ++dispatched_count) {
                record_task_events.emplace_back(LambdaTask::Dispatch(
                    [this, job = std::move(jobs[dispatched_count]), completion]() mutable {
                        // Finish only after recording and job destruction stop touching this executor.
                        auto error = RecordFrontendRange(std::move(job));
                        completion->Finish(std::move(error));
                    }
                ));
            }
        } catch (...) {
            // Save the failure without allocating: formatting it must not
            // unwind this executor while dispatched workers still use it.
            dispatch_error = std::current_exception();
            for (size_t index = dispatched_count; index < jobs.size(); ++index) {
                completion->Finish();
            }
        }

        std::string group_error{};
        // A plain join cannot pump another RenderFrame re-entrantly on the caller.
        if (!completion->Wait(group_error)) { return Fail(std::move(group_error)); }
        if (dispatch_error) {
            try {
                std::rethrow_exception(dispatch_error);
            } catch (const std::exception& exception) {
                return Fail(std::string("failed to dispatch recording task: ") + exception.what());
            } catch (...) {
                return Fail("failed to dispatch recording task");
            }
        }
        static std::once_flag logged_parallel_merge;
        std::call_once(logged_parallel_merge, [&] {
            LOG_INFO(
                "[RenderGraph][CompiledMerge] graph={} dispatch=task-graph worker_jobs={} completed=true",
                graph.name, jobs.size()
            );
        });
        return true;
    }

    [[nodiscard]] bool AppendFrontendStreamsInCompiledOrder() {
        // Assemble privately first: a later merge-contract failure must not
        // leave an earlier range in the caller's destination.
        try {
            if (frontend_streams.empty()) { return true; }
            auto& assembled = *frontend_streams.front();
            for (size_t index = 1; index < frontend_streams.size(); ++index) {
                assembled.AppendRecorded(std::move(*frontend_streams[index]));
            }
            destination.AppendRecorded(std::move(assembled));
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
    Array<SharedPtr<CommandList>> frontend_streams{};
    RenderGraphLowering::LoweredPlan lowered{};
    std::vector<std::vector<BarrierCreateInfo>> before{}, after{};
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
