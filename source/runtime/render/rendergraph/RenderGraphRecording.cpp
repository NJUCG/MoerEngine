#include "rendergraph/RenderGraph.h"

#include "rhi/RHIThreadOwnership.h"

#include <exception>
#include <string>

namespace Moer::Render {

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

} // namespace Moer::Render
