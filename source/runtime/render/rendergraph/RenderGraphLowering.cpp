#include "rendergraph/RenderGraphLowering.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <tuple>

namespace Moer::Render {

namespace {

using AccessMode    = RenderGraph::AccessMode;
using BufferState   = RenderGraph::BufferState;
using ExecutionDomain = RenderGraph::ExecutionDomain;
using PipelineType  = RenderGraph::PipelineType;
using ResourceKind  = RenderGraph::ResourceKind;
using ResourceState = RenderGraph::ResourceState;
using TextureAspect = RenderGraph::TextureAspect;
using TextureState  = RenderGraph::TextureState;

[[nodiscard]] constexpr bool HasRead(AccessMode access) {
    return access == AccessMode::Read || access == AccessMode::ReadWrite;
}

[[nodiscard]] constexpr bool HasWrite(AccessMode access) {
    return access == AccessMode::Write || access == AccessMode::ReadWrite;
}

[[nodiscard]] constexpr bool IsKnownAccess(AccessMode access) {
    return access != AccessMode::Unknown;
}

template<typename Enum>
[[nodiscard]] constexpr Enum Or(Enum lhs, Enum rhs) {
    return static_cast<Enum>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs)
    );
}

[[nodiscard]] bool ShaderStages(
    PipelineType             pipeline,
    ERHIPipelineStageFlags& stages,
    std::string&             error
) {
    switch (pipeline) {
        case PipelineType::None:
            stages = ERHIPipelineStageFlags::PS_ALL_COMMANDS;
            return true;
        case PipelineType::Graphics:
            stages = ERHIPipelineStageFlags::PS_ALL_GRAPHICS;
            return true;
        case PipelineType::Compute:
            stages = ERHIPipelineStageFlags::PS_COMPUTE_SHADER;
            return true;
        case PipelineType::RayTracing:
            stages = ERHIPipelineStageFlags::PS_RAY_TRACING_SHADER;
            return true;
        case PipelineType::Copy:
            error = "a shader resource cannot use the Copy pipeline domain";
            return false;
    }
    error = "unrecognized pipeline domain";
    return false;
}

[[nodiscard]] bool TextureLayoutFor(
    TextureState    state,
    ETextureLayout& layout,
    std::string&    error
) {
    switch (state) {
        case TextureState::Undefined:
            layout = ETextureLayout::TEXTURE_LAYOUT_UNDEFINED;
            return true;
        case TextureState::TransferSource:
            layout = ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC;
            return true;
        case TextureState::PresentationSource:
            layout = ETextureLayout::TEXTURE_LAYOUT_COMMON;
            return true;
        case TextureState::TransferDestination:
            layout = ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST;
            return true;
        case TextureState::ShaderResource:
            layout = ETextureLayout::TEXTURE_LAYOUT_COMMON;
            return true;
        case TextureState::Sampled:
            layout = ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return true;
        case TextureState::RenderTarget:
            layout = ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT;
            return true;
        case TextureState::DepthStencilRead:
            // The Vulkan command preprocessor records every depth attachment
            // (including load-only attachments) in ATTACHMENT_OPTIMAL. Keep
            // the explicit RDG state aligned with that physical body state.
            // Depth textures consumed by shaders use Sampled instead.
            layout = ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE;
            return true;
        case TextureState::DepthStencilWrite:
            layout = ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE;
            return true;
        case TextureState::UnorderedAccess:
            layout = ETextureLayout::TEXTURE_LAYOUT_COMMON;
            return true;
        case TextureState::Automatic:
            error = "Automatic texture state has no physical layout";
            return false;
        case TextureState::Present:
            error = "Present layout requires an external synchronization endpoint";
            return false;
    }
    error = "unrecognized texture state";
    return false;
}

[[nodiscard]] bool MapScope(
    const ResourceState&             state,
    AccessMode                       access,
    const ExecutionDomain&           domain,
    RenderGraphLowering::Scope&      scope,
    std::string&                     error
) {
    if (!IsKnownAccess(access)) {
        error = "Unknown access cannot be lowered";
        return false;
    }
    if (state.kind == ResourceKind::Token) {
        error = "token state cannot produce a physical barrier";
        return false;
    }
    if (state.IsAutomatic()) {
        error = "Automatic state cannot be lowered";
        return false;
    }

    scope = {};
    if (state.kind == ResourceKind::Texture) {
        scope.has_texture_layout = true;
        if (!TextureLayoutFor(state.texture, scope.layout, error)) {
            return false;
        }
        switch (state.texture) {
            case TextureState::Undefined:
                if (access != AccessMode::None) {
                    error = "Undefined texture state must use None access";
                    return false;
                }
                scope.stages = ERHIPipelineStageFlags::PS_NONE;
                scope.access = ERHIAccessFlags::UNDEFINED;
                return true;
            case TextureState::TransferSource:
                scope.stages = ERHIPipelineStageFlags::PS_TRANSFER;
                scope.access = ERHIAccessFlags::TRANSFER_READ;
                return true;
            case TextureState::PresentationSource:
                scope.stages = ERHIPipelineStageFlags::PS_TRANSFER;
                scope.access = ERHIAccessFlags::TRANSFER_READ;
                return true;
            case TextureState::TransferDestination:
                scope.stages = ERHIPipelineStageFlags::PS_TRANSFER;
                scope.access = ERHIAccessFlags::TRANSFER_WRITE;
                return true;
            case TextureState::ShaderResource:
            case TextureState::Sampled:
                if (!ShaderStages(domain.pipeline, scope.stages, error)) {
                    return false;
                }
                // VkCmdPreprocessor uses a generic shader-read access for both
                // SRV descriptor forms; their layouts remain distinct.
                scope.access = ERHIAccessFlags::SHADER_READ;
                return true;
            case TextureState::RenderTarget:
                scope.stages = ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT;
                if (HasRead(access)) {
                    scope.access = Or(scope.access, ERHIAccessFlags::COLOR_ATTACHMENT_READ);
                }
                if (HasWrite(access)) {
                    scope.access = Or(scope.access, ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);
                }
                return true;
            case TextureState::DepthStencilRead:
            case TextureState::DepthStencilWrite:
                scope.stages = Or(
                    ERHIPipelineStageFlags::PS_EARLY_FRAGMENT_TESTS,
                    ERHIPipelineStageFlags::PS_LATE_FRAGMENT_TESTS
                );
                if (HasRead(access)) {
                    scope.access = Or(scope.access, ERHIAccessFlags::DEPTH_STENCIL_READ);
                }
                if (HasWrite(access)) {
                    scope.access = Or(scope.access, ERHIAccessFlags::DEPTH_STENCIL_WRITE);
                }
                return true;
            case TextureState::UnorderedAccess:
                if (!ShaderStages(domain.pipeline, scope.stages, error)) {
                    return false;
                }
                if (HasRead(access)) {
                    scope.access = Or(scope.access, ERHIAccessFlags::SHADER_READ);
                }
                if (HasWrite(access)) {
                    scope.access = Or(scope.access, ERHIAccessFlags::SHADER_WRITE);
                }
                return true;
            case TextureState::Automatic:
            case TextureState::Present:
                break;
        }
    } else {
        scope.has_texture_layout = false;
        switch (state.buffer) {
            case BufferState::Undefined:
                if (access != AccessMode::None) {
                    error = "Undefined buffer state must use None access";
                    return false;
                }
                scope.stages = ERHIPipelineStageFlags::PS_NONE;
                scope.access = ERHIAccessFlags::UNDEFINED;
                return true;
            case BufferState::TransferSource:
                scope.stages = ERHIPipelineStageFlags::PS_TRANSFER;
                scope.access = ERHIAccessFlags::TRANSFER_READ;
                return true;
            case BufferState::TransferDestination:
                scope.stages = ERHIPipelineStageFlags::PS_TRANSFER;
                scope.access = ERHIAccessFlags::TRANSFER_WRITE;
                return true;
            case BufferState::VertexBuffer:
                scope.stages = ERHIPipelineStageFlags::PS_VERTEX_INPUT;
                scope.access = ERHIAccessFlags::VERTEX_ATTRIBUTE_READ;
                return true;
            case BufferState::IndexBuffer:
                scope.stages = ERHIPipelineStageFlags::PS_VERTEX_INPUT;
                scope.access = ERHIAccessFlags::INDEX_READ;
                return true;
            case BufferState::IndirectArgument:
                scope.stages = ERHIPipelineStageFlags::PS_DRAW_INDIRECT;
                scope.access = ERHIAccessFlags::INDIRECT_COMMAND_READ;
                return true;
            case BufferState::ShaderResource:
                if (!ShaderStages(domain.pipeline, scope.stages, error)) {
                    return false;
                }
                // VkCmdPreprocessor records CBV/SRV buffers with the generic
                // shader-read access bit.
                scope.access = ERHIAccessFlags::SHADER_READ;
                return true;
            case BufferState::UnorderedAccess:
                if (!ShaderStages(domain.pipeline, scope.stages, error)) {
                    return false;
                }
                if (HasRead(access)) {
                    scope.access = Or(scope.access, ERHIAccessFlags::SHADER_READ);
                }
                if (HasWrite(access)) {
                    scope.access = Or(scope.access, ERHIAccessFlags::SHADER_WRITE);
                }
                return true;
            case BufferState::AccelerationStructureBuildInput:
                scope.stages = ERHIPipelineStageFlags::PS_ACCELERATION_STRUCTURE_BUILD;
                scope.access = ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT;
                return true;
            case BufferState::AccelerationStructureRead:
                if (!ShaderStages(domain.pipeline, scope.stages, error)) {
                    return false;
                }
                scope.access = ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT;
                return true;
            case BufferState::AccelerationStructureWrite:
                scope.stages = ERHIPipelineStageFlags::PS_ACCELERATION_STRUCTURE_BUILD;
                scope.access = ERHIAccessFlags::ACCELERATION_STRUCTURE_WRITE_BIT;
                return true;
            case BufferState::Automatic:
                break;
        }
    }
    error = "unsupported resource state";
    return false;
}

[[nodiscard]] ETextureAspectFlags LowerAspects(TextureAspect aspects) {
    ETextureAspectFlags result = ETextureAspectFlags::NONE;
    const auto mask = static_cast<uint8_t>(aspects);
    if ((mask & static_cast<uint8_t>(TextureAspect::Color)) != 0) {
        result = Or(result, ETextureAspectFlags::COLOR);
    }
    if ((mask & static_cast<uint8_t>(TextureAspect::Depth)) != 0) {
        result = Or(result, ETextureAspectFlags::DEPTH_SLICE);
    }
    if ((mask & static_cast<uint8_t>(TextureAspect::Stencil)) != 0) {
        result = Or(result, ETextureAspectFlags::STENCIL_SLICE);
    }
    return result;
}

[[nodiscard]] TextureAspect RaiseAspects(ETextureAspectFlags aspects) {
    TextureAspect result = TextureAspect::None;
    const auto mask = static_cast<uint32_t>(aspects);
    if ((mask & static_cast<uint32_t>(ETextureAspectFlags::COLOR)) != 0) {
        result = static_cast<TextureAspect>(
            static_cast<uint8_t>(result) | static_cast<uint8_t>(TextureAspect::Color)
        );
    }
    if ((mask & static_cast<uint32_t>(ETextureAspectFlags::DEPTH_SLICE)) != 0) {
        result = static_cast<TextureAspect>(
            static_cast<uint8_t>(result) | static_cast<uint8_t>(TextureAspect::Depth)
        );
    }
    if ((mask & static_cast<uint32_t>(ETextureAspectFlags::STENCIL_SLICE)) != 0) {
        result = static_cast<TextureAspect>(
            static_cast<uint8_t>(result) | static_cast<uint8_t>(TextureAspect::Stencil)
        );
    }
    return result;
}

void AppendRange(std::ostringstream& stream, const RenderGraph::ResourceRange& range) {
    if (range.kind == ResourceKind::Texture) {
        stream << "aspect=" << static_cast<uint32_t>(range.texture.aspects)
               << ",mip=" << range.texture.mip_first << "+" << range.texture.mip_count
               << ",layer=" << range.texture.layer_first << "+" << range.texture.layer_count;
    } else {
        stream << "offset=" << range.buffer.offset << ",size=" << range.buffer.size;
    }
}

void AppendInstruction(
    std::ostringstream&                                  stream,
    const RenderGraphLowering::LoweredInstruction& instruction
) {
    stream << "kind=" << static_cast<uint32_t>(instruction.instruction_kind)
           << " barrier=" << instruction.barrier_index
           << " access_index=" << instruction.access_index
           << " correlation=" << instruction.correlation_id
           << " resource=" << instruction.resource.index << " ";
    AppendRange(stream, instruction.range);
    stream << " src_stage=" << static_cast<uint32_t>(instruction.source.stages)
           << " src_access=" << static_cast<uint32_t>(instruction.source.access)
           << " src_layout=" << static_cast<uint32_t>(instruction.source.layout)
           << " dst_stage=" << static_cast<uint32_t>(instruction.destination.stages)
           << " dst_access=" << static_cast<uint32_t>(instruction.destination.access)
           << " dst_layout=" << static_cast<uint32_t>(instruction.destination.layout)
           << " discard=" << instruction.discard_previous_contents
           << " import=" << instruction.import_boundary
           << " export=" << instruction.export_boundary
           << " transient_alias=" << instruction.transient_alias
           << " queue_acquire=" << instruction.queue_acquire
           << " transfer_src_role="
           << static_cast<uint32_t>(instruction.transfer_source.role)
           << " transfer_src_native="
           << instruction.transfer_source.native_queue_id
           << " transfer_src_family="
           << instruction.transfer_source.family_id
           << " transfer_dst_role="
           << static_cast<uint32_t>(instruction.transfer_destination.role)
           << " transfer_dst_native="
           << instruction.transfer_destination.native_queue_id
           << " transfer_dst_family="
           << instruction.transfer_destination.family_id;
}

} // namespace

void RenderGraphLowering::LoweredPlan::Clear() {
    prologue.clear();
    passes.clear();
    queue_syncs.clear();
}

const RenderGraphLowering::PassInstructions*
RenderGraphLowering::LoweredPlan::FindPass(RenderGraph::PassHandle pass) const {
    const auto found = std::find_if(
        passes.begin(),
        passes.end(),
        [&](const PassInstructions& candidate) { return candidate.pass == pass; }
    );
    return found == passes.end() ? nullptr : &*found;
}

std::span<const RenderGraphLowering::LoweredInstruction>
RenderGraphLowering::LoweredPlan::Before(RenderGraph::PassHandle pass) const {
    const auto* instructions = FindPass(pass);
    return instructions == nullptr ? std::span<const LoweredInstruction>{} :
                                     std::span<const LoweredInstruction>{instructions->before};
}

std::span<const RenderGraphLowering::LoweredInstruction>
RenderGraphLowering::LoweredPlan::After(RenderGraph::PassHandle pass) const {
    const auto* instructions = FindPass(pass);
    return instructions == nullptr ? std::span<const LoweredInstruction>{} :
                                     std::span<const LoweredInstruction>{instructions->after};
}

std::span<const RenderGraphLowering::PhysicalBinding>
RenderGraphLowering::LoweredPlan::Keepalive(RenderGraph::PassHandle pass) const {
    const auto* instructions = FindPass(pass);
    return instructions == nullptr ? std::span<const PhysicalBinding>{} :
                                     std::span<const PhysicalBinding>{instructions->keepalive};
}

std::string RenderGraphLowering::LoweredPlan::Dump() const {
    std::ostringstream stream;
    stream << "lowered prologue=" << prologue.size() << " passes=" << passes.size()
           << " queue_syncs=" << queue_syncs.size() << "\n";
    for (const auto& sync : queue_syncs) {
        stream << "sync=" << sync.correlation_id
               << " signal_pass=" << sync.signal_pass.index
               << " wait_pass=" << sync.wait_pass.index
               << " signal_native=" << sync.signal_queue.native_queue_id
               << " wait_native=" << sync.wait_queue.native_queue_id
               << " signal_batch=" << sync.signal_batch
               << " wait_batch=" << sync.wait_batch
               << " synthetic_ownership_join="
               << sync.synthetic_ownership_join
               << " ownership_joins=[";
        for (uint32_t index = 0; index < sync.ownership_join_barriers.size(); ++index) {
            if (index != 0) {
                stream << ",";
            }
            stream << sync.ownership_join_barriers[index];
        }
        stream << "] ownership_transfers=[";
        for (uint32_t index = 0;
             index < sync.ownership_transfer_barriers.size();
             ++index) {
            if (index != 0) {
                stream << ",";
            }
            stream << sync.ownership_transfer_barriers[index];
        }
        stream << "]\n";
    }
    for (const auto& instruction : prologue) {
        stream << "prologue ";
        AppendInstruction(stream, instruction);
        stream << "\n";
    }
    for (const auto& pass : passes) {
        stream << "pass=" << pass.pass.index << " keepalive=[";
        for (uint32_t index = 0; index < pass.keepalive.size(); ++index) {
            if (index != 0) {
                stream << ",";
            }
            stream << pass.keepalive[index].resource.index;
        }
        stream << "] before=" << pass.before.size() << " after=" << pass.after.size() << "\n";
        for (const auto& instruction : pass.before) {
            stream << "  before ";
            AppendInstruction(stream, instruction);
            stream << "\n";
        }
        for (const auto& instruction : pass.after) {
            stream << "  after ";
            AppendInstruction(stream, instruction);
            stream << "\n";
        }
    }
    return stream.str();
}

bool RenderGraphLowering::Lower(
    const RenderGraph& graph,
    LoweredPlan&       output,
    std::string&       error
) {
    output.Clear();
    error.clear();
    auto fail = [&](std::string message) {
        output.Clear();
        error = "RenderGraph lowering: " + std::move(message);
        return false;
    };

    if (!graph.compiled) {
        return fail("graph must be compiled before lowering");
    }
    const auto& compiled = graph.compiled_plan;
    if (!compiled.state_plan_complete) {
        return fail("compiled state plan is incomplete");
    }
    if (compiled.resources.size() != graph.resources.size()) {
        return fail("compiled resource table is incomplete");
    }

    auto supported_queue = [](RenderGraph::QueueRole role) {
        return role == RenderGraph::QueueRole::Graphics ||
               role == RenderGraph::QueueRole::Compute ||
               role == RenderGraph::QueueRole::Copy;
    };
    std::vector<bool>     gpu_pass(graph.passes.size(), false);
    std::vector<uint32_t> pass_queue_batch(
        graph.passes.size(), RenderGraph::PassHandle::InvalidIndex
    );
    for (const auto& pass : graph.passes) {
        if (pass.execution_class == RenderGraph::PassExecutionClass::ExternalControl) {
            return fail("ExternalControl pass is outside the managed lowering domain: '" +
                        pass.name + "'");
        }
    }
    std::vector<bool> execution_member(graph.passes.size(), false);
    if (compiled.execution_order.size() != graph.passes.size()) {
        return fail("execution order does not cover every declared pass");
    }
    for (const auto pass : compiled.execution_order) {
        if (!graph.IsValidPass(pass) || execution_member[pass.index]) {
            return fail("execution order contains an invalid or duplicate pass");
        }
        execution_member[pass.index] = true;
    }
    if (compiled.recording_batches.size() != compiled.execution_order.size()) {
        return fail("recording schedule does not cover the execution order");
    }
    for (uint32_t batch_index = 0;
         batch_index < compiled.recording_batches.size();
         ++batch_index) {
        const auto& batch = compiled.recording_batches[batch_index];
        const auto  pass  = compiled.execution_order[batch_index];
        const auto& declaration = graph.passes[pass.index];
        if (batch.id != batch_index || batch.passes.size() != 1 ||
            batch.passes.front() != pass ||
            batch.queue != graph.queue_topology.Resolve(declaration.domain.queue) ||
            batch.execution != declaration.execution_class ||
            batch.workload != declaration.workload) {
            return fail(
                "recording schedule disagrees with stable execution order"
            );
        }
    }
    for (uint32_t batch_index = 0; batch_index < compiled.queue_batches.size();
         ++batch_index) {
        const auto& batch = compiled.queue_batches[batch_index];
        if (batch.external_control) {
            return fail("external-control queue batch is outside the managed lowering domain");
        }
        if (batch.id != batch_index || !batch.queue.available ||
            !supported_queue(batch.queue.role)) {
            return fail(
                "queue batch has an unsupported role or unstable identifier"
            );
        }
        if (batch.queue != graph.queue_topology.Resolve(batch.queue.role)) {
            return fail("queue batch binding disagrees with the graph topology");
        }
        if (batch.passes.empty()) {
            return fail("queue batch contains no GPU pass");
        }
        for (const auto pass : batch.passes) {
            if (!graph.IsValidPass(pass) || !execution_member[pass.index]) {
                return fail("queue batch references a pass outside execution order");
            }
            if (gpu_pass[pass.index]) {
                return fail("GPU pass belongs to more than one queue batch");
            }
            gpu_pass[pass.index] = true;
            pass_queue_batch[pass.index] = batch_index;
            if (graph.passes[pass.index].domain.queue != batch.queue.role) {
                return fail("GPU pass '" + graph.passes[pass.index].name +
                            "' disagrees with its queue batch role");
            }
        }
    }
    for (const auto pass : compiled.execution_order) {
        const bool should_be_gpu =
            graph.passes[pass.index].execution_class !=
            RenderGraph::PassExecutionClass::CpuPrepare;
        if (gpu_pass[pass.index] != should_be_gpu) {
            return fail(
                should_be_gpu ?
                    "execution-order GPU pass has no queue batch" :
                    "CpuPrepare pass unexpectedly belongs to a queue batch"
            );
        }
    }
    for (uint32_t batch_index = 0; batch_index < compiled.queue_batches.size();
         ++batch_index) {
        const auto& batch = compiled.queue_batches[batch_index];
        std::vector<bool> seen_signals(compiled.queue_syncs.size(), false);
        for (const uint32_t sync_index : batch.signal_syncs) {
            if (sync_index >= compiled.queue_syncs.size() ||
                seen_signals[sync_index] ||
                compiled.queue_syncs[sync_index].signal_batch != batch_index) {
                return fail("queue batch has an invalid or duplicate signal sync");
            }
            seen_signals[sync_index] = true;
        }
        std::vector<bool> seen_waits(compiled.queue_syncs.size(), false);
        for (const uint32_t sync_index : batch.wait_syncs) {
            if (sync_index >= compiled.queue_syncs.size() ||
                seen_waits[sync_index] ||
                compiled.queue_syncs[sync_index].wait_batch != batch_index) {
                return fail("queue batch has an invalid or duplicate wait sync");
            }
            seen_waits[sync_index] = true;
        }
    }
    std::vector<QueueSyncInstruction> lowered_queue_syncs{};
    lowered_queue_syncs.reserve(compiled.queue_syncs.size());
    for (uint32_t sync_index = 0; sync_index < compiled.queue_syncs.size();
         ++sync_index) {
        const auto& sync = compiled.queue_syncs[sync_index];
        if (sync.id != sync_index ||
            sync.signal_batch >= compiled.queue_batches.size() ||
            sync.wait_batch >= compiled.queue_batches.size() ||
            !graph.IsValidPass(sync.signal_pass) ||
            !graph.IsValidPass(sync.wait_pass) ||
            !supported_queue(sync.signal_queue.role) ||
            !supported_queue(sync.wait_queue.role)) {
            return fail("queue synchronization record is malformed");
        }
        const bool duplicate_pair = std::any_of(
            compiled.queue_syncs.begin(),
            compiled.queue_syncs.begin() + sync_index,
            [&](const RenderGraph::CompiledQueueSync& candidate) {
                return candidate.signal_batch == sync.signal_batch &&
                       candidate.wait_batch == sync.wait_batch;
            }
        );
        if (duplicate_pair || sync.signal_batch >= sync.wait_batch) {
            return fail(
                duplicate_pair ?
                    "queue synchronization batch pair is duplicated" :
                    "queue synchronization is not forward ordered"
            );
        }
        const auto& signal_batch = compiled.queue_batches[sync.signal_batch];
        const auto& wait_batch   = compiled.queue_batches[sync.wait_batch];
        if (signal_batch.passes.empty() || wait_batch.passes.empty() ||
            sync.signal_pass != signal_batch.passes.back() ||
            sync.wait_pass != wait_batch.passes.front() ||
            sync.signal_queue != signal_batch.queue ||
            sync.wait_queue != wait_batch.queue ||
            pass_queue_batch[sync.signal_pass.index] != sync.signal_batch ||
            pass_queue_batch[sync.wait_pass.index] != sync.wait_batch) {
            return fail("queue synchronization endpoints disagree with their batches");
        }
        if (std::count(
                signal_batch.signal_syncs.begin(),
                signal_batch.signal_syncs.end(),
                sync_index
            ) != 1 ||
            std::count(
                wait_batch.wait_syncs.begin(),
                wait_batch.wait_syncs.end(),
                sync_index
            ) != 1) {
            return fail("queue synchronization is not owned by both endpoint batches");
        }
        const bool crosses_native =
            sync.signal_queue.native_queue_id != sync.wait_queue.native_queue_id;
        if (sync.gpu_wait_required != crosses_native) {
            return fail("queue synchronization GPU-wait policy disagrees with topology");
        }
        if (sync.gpu_wait_required && sync.dependency_edges.empty()) {
            return fail(
                "cross-native queue synchronization has no dependency edge"
            );
        }
        std::vector<bool> seen_edges(compiled.edges.size(), false);
        for (const uint32_t edge_index : sync.dependency_edges) {
            if (edge_index >= compiled.edges.size() || seen_edges[edge_index]) {
                return fail(
                    "queue synchronization has an invalid or duplicate dependency edge"
                );
            }
            seen_edges[edge_index] = true;
            const auto& edge = compiled.edges[edge_index];
            if (!graph.IsValidPass(edge.src) || !graph.IsValidPass(edge.dst) ||
                pass_queue_batch[edge.src.index] != sync.signal_batch ||
                pass_queue_batch[edge.dst.index] != sync.wait_batch) {
                return fail(
                    "queue synchronization dependency edge disagrees with its batches"
                );
            }
        }
        std::vector<bool> seen_barriers(compiled.barriers.size(), false);
        for (const uint32_t barrier_index : sync.barriers) {
            if (barrier_index >= compiled.barriers.size() ||
                seen_barriers[barrier_index]) {
                return fail(
                    "queue synchronization has an invalid or duplicate barrier"
                );
            }
            seen_barriers[barrier_index] = true;
            const auto& barrier = compiled.barriers[barrier_index];
            const bool matching_destination =
                graph.IsValidPass(barrier.dst_pass) &&
                pass_queue_batch[barrier.dst_pass.index] == sync.wait_batch;
            const bool matching_source = std::any_of(
                barrier.sources.begin(),
                barrier.sources.end(),
                [&](const RenderGraph::CompiledBarrierSource& source) {
                    return graph.IsValidPass(source.pass) &&
                           pass_queue_batch[source.pass.index] ==
                               sync.signal_batch;
                }
            );
            if (!matching_destination || !matching_source) {
                return fail(
                    "queue synchronization barrier disagrees with its batches"
                );
            }
        }
        if (sync.gpu_wait_required) {
            lowered_queue_syncs.push_back(QueueSyncInstruction{
                .correlation_id = sync.id,
                .signal_pass    = sync.signal_pass,
                .wait_pass      = sync.wait_pass,
                .signal_queue   = sync.signal_queue,
                .wait_queue     = sync.wait_queue,
                .signal_batch   = sync.signal_batch,
                .wait_batch     = sync.wait_batch,
            });
        }
    }
    for (uint32_t edge_index = 0; edge_index < compiled.edges.size();
         ++edge_index) {
        const auto& edge = compiled.edges[edge_index];
        if (!graph.IsValidPass(edge.src) || !graph.IsValidPass(edge.dst)) {
            return fail("compiled dependency edge references an invalid pass");
        }
        const uint32_t signal_batch = pass_queue_batch[edge.src.index];
        const uint32_t wait_batch   = pass_queue_batch[edge.dst.index];
        if (signal_batch == RenderGraph::PassHandle::InvalidIndex ||
            wait_batch == RenderGraph::PassHandle::InvalidIndex ||
            signal_batch == wait_batch) {
            continue;
        }
        const auto& signal_queue = compiled.queue_batches[signal_batch].queue;
        const auto& wait_queue   = compiled.queue_batches[wait_batch].queue;
        if (signal_queue.native_queue_id == wait_queue.native_queue_id) {
            continue;
        }
        const auto sync = std::find_if(
            compiled.queue_syncs.begin(),
            compiled.queue_syncs.end(),
            [&](const RenderGraph::CompiledQueueSync& candidate) {
                return candidate.signal_batch == signal_batch &&
                       candidate.wait_batch == wait_batch &&
                       candidate.gpu_wait_required &&
                       std::find(
                           candidate.dependency_edges.begin(),
                           candidate.dependency_edges.end(),
                           edge_index
                       ) != candidate.dependency_edges.end();
            }
        );
        if (sync == compiled.queue_syncs.end()) {
            return fail(
                "cross-native dependency edge is not correlated with an "
                "executable GPU sync"
            );
        }
    }

    std::vector<bool> resource_has_access(graph.resources.size(), false);
    for (const auto& access : compiled.accesses) {
        if (!graph.IsValidResource(access.resource) || !graph.IsValidPass(access.pass)) {
            return fail("compiled access references an invalid handle");
        }
        if (access.resource.index >= graph.resources.size()) {
            return fail("compiled access resource index is out of range");
        }
        const auto& resource = graph.resources[access.resource.index];
        if (resource.kind == ResourceKind::Token) {
            continue;
        }
        resource_has_access[access.resource.index] = true;
        if (access.state.IsAutomatic()) {
            return fail("physical access uses Automatic state on resource '" + resource.name + "'");
        }
        if (!IsKnownAccess(access.mode)) {
            return fail("physical access uses Unknown access on resource '" + resource.name + "'");
        }
        if (!supported_queue(access.domain.queue)) {
            return fail("physical access uses an unsupported logical queue on resource '" +
                        resource.name + "'");
        }
        if (access.state.kind == ResourceKind::Texture &&
            access.state.texture == TextureState::Present) {
            return fail("Present state requires an external synchronization endpoint on resource '" +
                        resource.name + "'");
        }
        if (access.state.kind == ResourceKind::Texture &&
            access.state.texture == TextureState::PresentationSource) {
            return fail(
                "PresentationSource is an export-boundary-only state on resource '" +
                resource.name + "'"
            );
        }
    }

    auto validate_boundary = [&](const RenderGraph::ResourceDeclaration& resource,
                                 const RenderGraph::StateDeclaration&    state,
                                 bool                                    initial) {
        if (state.state.IsAutomatic()) {
            error = "Automatic boundary state on resource '" + resource.name + "'";
            return false;
        }
        if (!IsKnownAccess(state.boundary_access)) {
            error = "Unknown boundary access on resource '" + resource.name + "'";
            return false;
        }
        if (state.state.kind == ResourceKind::Texture &&
            state.state.texture == TextureState::Present) {
            error = "Present boundary requires an external synchronization endpoint on resource '" +
                    resource.name + "'";
            return false;
        }
        if (state.state.kind == ResourceKind::Texture &&
            state.state.texture == TextureState::PresentationSource) {
            if (initial) {
                error =
                    "PresentationSource is an export-boundary-only state on resource '" +
                    resource.name + "'";
                return false;
            }
            if (state.queue != RenderGraph::QueueRole::Graphics) {
                error =
                    "PresentationSource export requires the Graphics queue on resource '" +
                    resource.name + "'";
                return false;
            }
        }
        if (!initial && state.state.IsUndefined()) {
            error = "Undefined state cannot be an export destination on resource '" +
                    resource.name + "'";
            return false;
        }
        if (initial && state.state.IsUndefined()) {
            if (state.queue != RenderGraph::QueueRole::None ||
                state.boundary_access != AccessMode::None) {
                error = "Undefined import boundary must use queue None and access None on resource '" +
                        resource.name + "'";
                return false;
            }
            return true;
        }
        if (!supported_queue(state.queue)) {
            error = "boundary uses an unsupported queue on resource '" +
                    resource.name + "'";
            return false;
        }
        return true;
    };

    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        if (resource.kind == ResourceKind::Token) {
            continue;
        }
        if (!resource_has_access[resource_index] &&
            resource.first_use == RenderGraph::PassHandle::InvalidIndex) {
            continue;
        }
        if (!resource.typed_desc) {
            return fail("physical resource requires a typed descriptor: '" + resource.name + "'");
        }
        if (!resource.imported) {
            const bool has_allocation_desc =
                resource.kind == ResourceKind::Texture ?
                    resource.transient_texture_desc.has_value() :
                    resource.transient_buffer_desc.has_value();
            if (!has_allocation_desc) {
                return fail(
                    "transient physical resource lacks an allocation descriptor: '" +
                    resource.name + "'"
                );
            }
        }
        if (resource.kind == ResourceKind::Texture) {
            if (!resource.physical_texture.IsValid() ||
                resource.physical_identity != resource.physical_texture.Get()) {
                return fail("texture lacks a strong physical binding: '" + resource.name + "'");
            }
            const auto actual_aspects = RaiseAspects(resource.physical_texture->GetAspectFlags());
            if (resource.texture_desc.mip_count != resource.physical_texture->GetNumMips() ||
                resource.texture_desc.layer_count != resource.physical_texture->GetNumArray() ||
                resource.texture_desc.aspects != actual_aspects) {
                return fail("texture descriptor does not match its physical binding: '" +
                            resource.name + "'");
            }
        } else {
            if (!resource.physical_buffer.IsValid() ||
                resource.physical_identity != resource.physical_buffer.Get()) {
                return fail("buffer lacks a strong physical binding: '" + resource.name + "'");
            }
            if (resource.buffer_desc.byte_size != resource.physical_buffer->GetByteSize()) {
                return fail("buffer descriptor does not match its physical binding: '" +
                            resource.name + "'");
            }
        }

        if (!resource_has_access[resource_index]) {
            continue;
        }
        if (resource.imported) {
            if (resource.initial_states.empty()) {
                return fail(
                    "physical resource lacks an explicit initial state: '" +
                    resource.name + "'"
                );
            }
            if (!resource.exported || resource.final_states.empty()) {
                return fail(
                    "physical resource lacks an explicit final state: '" +
                    resource.name + "'"
                );
            }
            for (const auto& state : resource.initial_states) {
                if (!validate_boundary(resource, state, true)) {
                    return fail(std::move(error));
                }
            }
        } else if (!resource.initial_states.empty()) {
            return fail(
                "allocation-backed transient resource must begin in its implicit "
                "Undefined state: '" +
                resource.name + "'"
            );
        }
        if (resource.exported && resource.final_states.empty()) {
            return fail(
                "exported transient resource lacks an explicit final state: '" +
                resource.name + "'"
            );
        }
        for (const auto& state : resource.final_states) {
            if (!validate_boundary(resource, state, false)) {
                return fail(std::move(error));
            }
        }
    }

    std::vector<uint32_t> alias_barrier_owners(compiled.barriers.size(), 0);
    for (const auto& alias : compiled.alias_boundaries) {
        if (!graph.IsValidResource(alias.predecessor_resource) ||
            !graph.IsValidResource(alias.successor_resource) ||
            !graph.IsValidPass(alias.primary_src_pass) ||
            !graph.IsValidPass(alias.dst_pass) ||
            alias.source_frontier.empty() ||
            alias.barrier_index >= compiled.barriers.size()) {
            return fail("transient alias boundary contains an invalid handle or index");
        }
        const auto& predecessor =
            graph.resources[alias.predecessor_resource.index];
        const auto& successor =
            graph.resources[alias.successor_resource.index];
        const auto& predecessor_plan =
            compiled.resources[alias.predecessor_resource.index];
        const auto& successor_plan =
            compiled.resources[alias.successor_resource.index];
        if (predecessor.imported || successor.imported ||
            predecessor.exported || successor.exported ||
            predecessor.physical_identity == nullptr ||
            predecessor.physical_identity != successor.physical_identity ||
            predecessor_plan.transient_slot != alias.transient_slot ||
            successor_plan.transient_slot != alias.transient_slot ||
            predecessor_plan.last_use >= successor_plan.first_use) {
            return fail(
                "transient alias boundary does not bind one compatible non-overlapping "
                "physical allocation"
            );
        }

        const auto& barrier = compiled.barriers[alias.barrier_index];
        ++alias_barrier_owners[alias.barrier_index];
        if (!barrier.transient_alias ||
            barrier.resource != alias.successor_resource ||
            barrier.src_pass != alias.primary_src_pass ||
            barrier.dst_pass != alias.dst_pass ||
            barrier.sources.size() != alias.source_frontier.size() ||
            !barrier.memory_dependency || !barrier.execution_dependency ||
            barrier.queue_dependency || barrier.queue_ownership ||
            barrier.discard_previous_contents || barrier.import_boundary ||
            barrier.export_boundary || barrier.source_state_unknown) {
            return fail("transient alias boundary disagrees with its compiled barrier");
        }
        for (uint32_t source_index = 0;
             source_index < alias.source_frontier.size();
             ++source_index) {
            if (barrier.sources[source_index].pass !=
                alias.source_frontier[source_index]) {
                return fail(
                    "transient alias barrier dropped or reordered a source frontier"
                );
            }
        }
        if (alias.source_frontier.back() != alias.primary_src_pass) {
            return fail(
                "transient alias primary source is not its latest frontier source"
            );
        }
    }
    for (uint32_t barrier_index = 0;
         barrier_index < compiled.barriers.size();
         ++barrier_index) {
        const uint32_t owner_count = alias_barrier_owners[barrier_index];
        if ((compiled.barriers[barrier_index].transient_alias &&
             owner_count != 1) ||
            (!compiled.barriers[barrier_index].transient_alias &&
             owner_count != 0)) {
            return fail(
                "transient alias barrier does not have exactly one boundary owner"
            );
        }
    }

    std::vector<bool> prologue(compiled.barriers.size(), false);
    std::vector<bool> epilogue(compiled.barriers.size(), false);
    std::vector<uint32_t> before_occurrences(compiled.barriers.size(), 0);
    for (const uint32_t barrier_index : compiled.prologue_barriers) {
        if (barrier_index >= compiled.barriers.size() || prologue[barrier_index]) {
            return fail("prologue contains an invalid or duplicate barrier index");
        }
        prologue[barrier_index] = true;
    }
    for (const uint32_t barrier_index : compiled.epilogue_barriers) {
        if (barrier_index >= compiled.barriers.size() || epilogue[barrier_index]) {
            return fail("epilogue contains an invalid or duplicate barrier index");
        }
        epilogue[barrier_index] = true;
    }
    for (const auto& pass_barriers : compiled.pass_barriers) {
        if (!graph.IsValidPass(pass_barriers.pass)) {
            return fail("pass barrier list references an invalid pass");
        }
        for (const uint32_t barrier_index : pass_barriers.before) {
            if (barrier_index >= compiled.barriers.size()) {
                return fail("pass barrier list contains an invalid barrier index");
            }
            ++before_occurrences[barrier_index];
        }
    }

    LoweredPlan candidate{};
    candidate.queue_syncs = lowered_queue_syncs;
    candidate.passes.reserve(compiled.execution_order.size());
    for (const auto pass : compiled.execution_order) {
        if (!graph.IsValidPass(pass)) {
            return fail("execution order references an invalid pass");
        }
        if (gpu_pass[pass.index]) {
            candidate.passes.push_back(PassInstructions{.pass = pass});
        }
    }

    auto find_mutable_pass = [&](RenderGraph::PassHandle pass) -> PassInstructions* {
        const auto found = std::find_if(
            candidate.passes.begin(),
            candidate.passes.end(),
            [&](const PassInstructions& entry) { return entry.pass == pass; }
        );
        return found == candidate.passes.end() ? nullptr : &*found;
    };

    auto binding_for = [&](RenderGraph::ResourceHandle handle) {
        const auto& resource = graph.resources[handle.index];
        PhysicalBinding binding{
            .resource = handle,
            .kind     = resource.kind,
        };
        if (resource.kind == ResourceKind::Texture) {
            binding.texture = resource.physical_texture;
        } else if (resource.kind == ResourceKind::Buffer) {
            binding.buffer = resource.physical_buffer;
        }
        return binding;
    };

    for (auto& pass_entry : candidate.passes) {
        std::vector<RenderGraph::ResourceHandle> resources{};
        for (const auto& access : compiled.accesses) {
            if (access.pass == pass_entry.pass &&
                graph.resources[access.resource.index].kind != ResourceKind::Token) {
                resources.push_back(access.resource);
            }
        }
        for (const auto resource : graph.passes[pass_entry.pass.index].references) {
            if (graph.resources[resource.index].kind != ResourceKind::Token) {
                resources.push_back(resource);
            }
        }
        std::sort(
            resources.begin(),
            resources.end(),
            [](RenderGraph::ResourceHandle lhs, RenderGraph::ResourceHandle rhs) {
                return lhs.index < rhs.index;
            }
        );
        resources.erase(
            std::unique(
                resources.begin(),
                resources.end(),
                [](RenderGraph::ResourceHandle lhs, RenderGraph::ResourceHandle rhs) {
                    return lhs.index == rhs.index;
                }
            ),
            resources.end()
        );
        for (const auto resource : resources) {
            pass_entry.keepalive.push_back(binding_for(resource));
        }
    }

    std::vector<uint32_t> execution_position(
        graph.passes.size(),
        RenderGraph::PassHandle::InvalidIndex
    );
    for (uint32_t position = 0; position < compiled.execution_order.size(); ++position) {
        execution_position[compiled.execution_order[position].index] = position;
    }

    auto lower_instruction = [&](uint32_t barrier_index, LoweredInstruction& instruction) {
        const auto& barrier = compiled.barriers[barrier_index];
        bool queue_acquire = false;
        if (!graph.IsValidResource(barrier.resource)) {
            error = "barrier references an invalid resource";
            return false;
        }
        const auto& resource = graph.resources[barrier.resource.index];
        if (resource.kind == ResourceKind::Token) {
            error = "token barrier cannot be physically lowered";
            return false;
        }
        if (barrier.queue_dependency &&
            (!barrier.src_pass.IsValid() || !barrier.dst_pass.IsValid())) {
            error =
                "cross-queue external boundary lacks a managed synchronization endpoint";
            return false;
        }
        if (barrier.queue_ownership &&
            (!barrier.queue_dependency || barrier.import_boundary ||
             barrier.export_boundary || barrier.transient_alias ||
             barrier.sources.empty() || !barrier.src_pass.IsValid() ||
             !barrier.dst_pass.IsValid())) {
            error =
                "queue-family ownership requires one internal managed source "
                "frontier and destination";
            return false;
        }
        if (barrier.queue_dependency) {
            for (const auto& source : barrier.sources) {
                if (!graph.IsValidPass(source.pass)) {
                    error = "queue barrier contains an invalid source pass";
                    return false;
                }
                const uint32_t signal_batch =
                    pass_queue_batch[source.pass.index];
                const uint32_t wait_batch =
                    pass_queue_batch[barrier.dst_pass.index];
                if (signal_batch == RenderGraph::PassHandle::InvalidIndex ||
                    wait_batch == RenderGraph::PassHandle::InvalidIndex) {
                    error = "queue barrier source or destination has no queue batch";
                    return false;
                }
                if (compiled.queue_batches[signal_batch].queue.native_queue_id ==
                    compiled.queue_batches[wait_batch].queue.native_queue_id) {
                    continue;
                }
                queue_acquire = true;
                const auto sync = std::find_if(
                    compiled.queue_syncs.begin(),
                    compiled.queue_syncs.end(),
                    [&](const RenderGraph::CompiledQueueSync& candidate) {
                        return candidate.signal_batch == signal_batch &&
                               candidate.wait_batch == wait_batch &&
                               candidate.gpu_wait_required &&
                               std::find(
                                   candidate.barriers.begin(),
                                   candidate.barriers.end(),
                                   barrier_index
                               ) != candidate.barriers.end();
                    }
                );
                if (sync == compiled.queue_syncs.end()) {
                    error =
                        "queue barrier is not correlated with an executable GPU sync";
                    return false;
                }
            }
        }
        if (barrier.source_state_unknown) {
            error = "barrier source state is unknown";
            return false;
        }
        if (barrier.before_state.IsAutomatic() || barrier.after_state.IsAutomatic()) {
            error = "barrier contains Automatic state";
            return false;
        }
        if (!IsKnownAccess(barrier.before_access) || !IsKnownAccess(barrier.after_access)) {
            error = "barrier contains Unknown access";
            return false;
        }
        if (barrier.before_state.kind == ResourceKind::Texture &&
            (barrier.before_state.texture == TextureState::Present ||
             barrier.after_state.texture == TextureState::Present)) {
            error = "Present barrier requires an external synchronization endpoint";
            return false;
        }
        if (barrier.dst_pass.IsValid()) {
            if (!graph.IsValidPass(barrier.dst_pass) ||
                !supported_queue(barrier.dst_domain.queue)) {
                error = "barrier destination is not a supported GPU pass";
                return false;
            }
        } else if (!barrier.export_boundary ||
                   !supported_queue(barrier.dst_domain.queue)) {
            error = "barrier has no managed supported destination";
            return false;
        }

        instruction = LoweredInstruction{
            .instruction_kind        = InstructionKind::Barrier,
            .correlation_id          = barrier_index,
            .barrier_index           = barrier_index,
            .access_index            = InvalidAccessIndex,
            .resource                = barrier.resource,
            .resource_kind           = resource.kind,
            .range                   = barrier.range,
            .texture_aspects         = resource.kind == ResourceKind::Texture ?
                                           LowerAspects(barrier.range.texture.aspects) :
                                           ETextureAspectFlags::NONE,
            .physical                = binding_for(barrier.resource),
            .src_pass                = barrier.src_pass,
            .dst_pass                = barrier.dst_pass,
            .before_state            = barrier.before_state,
            .after_state             = barrier.after_state,
            .before_access           = barrier.before_access,
            .after_access            = barrier.after_access,
            .state_transition        = barrier.state_transition,
            .memory_dependency       = barrier.memory_dependency,
            .execution_dependency    = barrier.execution_dependency,
            .discard_previous_contents = barrier.discard_previous_contents,
            .import_boundary         = barrier.import_boundary,
            .export_boundary         = barrier.export_boundary,
            .transient_alias         = barrier.transient_alias,
            .queue_acquire           = queue_acquire,
        };

        if (resource.kind == ResourceKind::Texture) {
            if (barrier.range.texture.aspects == TextureAspect::None ||
                barrier.range.texture.mip_count == RenderGraph::RemainingTextureRange ||
                barrier.range.texture.layer_count == RenderGraph::RemainingTextureRange) {
                error = "texture barrier range is not fully normalized";
                return false;
            }
        } else if (barrier.range.buffer.size == RenderGraph::RemainingBufferRange) {
            error = "buffer barrier range is not fully normalized";
            return false;
        }

        if (!barrier.sources.empty()) {
            bool first_source = true;
            for (const auto& source : barrier.sources) {
                if (!graph.IsValidPass(source.pass) ||
                    !supported_queue(source.domain.queue) ||
                    !IsKnownAccess(source.access) || source.state.IsAutomatic()) {
                    error = "barrier fan-in contains an unsupported source";
                    return false;
                }
                Scope source_scope{};
                if (!MapScope(source.state, source.access, source.domain, source_scope, error)) {
                    return false;
                }
                instruction.source_frontier.push_back(source.pass);
                const uint32_t source_batch =
                    pass_queue_batch[source.pass.index];
                const uint32_t destination_batch =
                    barrier.dst_pass.IsValid() ?
                        pass_queue_batch[barrier.dst_pass.index] :
                        RenderGraph::PassHandle::InvalidIndex;
                const bool contributes_local_scope =
                    barrier.queue_ownership || !instruction.queue_acquire ||
                    (source_batch != RenderGraph::PassHandle::InvalidIndex &&
                     destination_batch != RenderGraph::PassHandle::InvalidIndex &&
                     compiled.queue_batches[source_batch]
                             .queue.native_queue_id ==
                         compiled.queue_batches[destination_batch]
                             .queue.native_queue_id);
                if (!contributes_local_scope) {
                    continue;
                }
                if (first_source) {
                    instruction.source = source_scope;
                    first_source = false;
                } else {
                    instruction.source.stages =
                        Or(instruction.source.stages, source_scope.stages);
                    instruction.source.access =
                        Or(instruction.source.access, source_scope.access);
                }
            }
            if (resource.kind == ResourceKind::Texture) {
                if (!TextureLayoutFor(
                        barrier.before_state.texture,
                        instruction.source.layout,
                        error
                    )) {
                    return false;
                }
            }
            instruction.source.has_texture_layout = resource.kind == ResourceKind::Texture;
        } else {
            if (!MapScope(
                    barrier.before_state,
                    barrier.before_access,
                    barrier.src_domain,
                    instruction.source,
                    error
                )) {
                return false;
            }
            if (barrier.src_pass.IsValid()) {
                instruction.source_frontier.push_back(barrier.src_pass);
            }
        }
        if (!MapScope(
                barrier.after_state,
                barrier.after_access,
                barrier.dst_domain,
                instruction.destination,
                error
            )) {
            return false;
        }
        if (instruction.queue_acquire && barrier.sources.empty()) {
            // The semaphore wait supplies the cross-queue memory dependency.
            // This destination-local barrier performs visibility/layout
            // adoption without pretending its source scope executes on the
            // consumer queue.
            instruction.source.stages = ERHIPipelineStageFlags::PS_NONE;
            instruction.source.access = ERHIAccessFlags::UNDEFINED;
        }
        if (instruction.discard_previous_contents && !barrier.before_state.IsUndefined()) {
            error = "discard flag requires an Undefined source state";
            return false;
        }
        return true;
    };

    uint32_t next_synthetic_sync_id =
        static_cast<uint32_t>(compiled.queue_syncs.size());
    auto find_lowered_sync =
        [&](uint32_t signal_batch,
            uint32_t wait_batch) -> QueueSyncInstruction* {
            const auto found = std::find_if(
                candidate.queue_syncs.begin(),
                candidate.queue_syncs.end(),
                [&](const QueueSyncInstruction& sync) {
                    return sync.signal_batch == signal_batch &&
                           sync.wait_batch == wait_batch;
                }
            );
            return found == candidate.queue_syncs.end() ? nullptr : &*found;
        };
    auto append_unique_barrier =
        [](std::vector<uint32_t>& barriers, uint32_t barrier_index) {
            if (std::find(barriers.begin(), barriers.end(), barrier_index) ==
                barriers.end()) {
                barriers.push_back(barrier_index);
            }
        };
    auto ensure_ownership_join =
        [&](uint32_t signal_batch,
            uint32_t wait_batch,
            uint32_t barrier_index) {
            if (signal_batch >= compiled.queue_batches.size() ||
                wait_batch >= compiled.queue_batches.size() ||
                signal_batch >= wait_batch) {
                error =
                    "ownership fan-in join is not a forward queue-batch edge";
                return false;
            }
            const auto& signal = compiled.queue_batches[signal_batch];
            const auto& wait   = compiled.queue_batches[wait_batch];
            if (signal.queue.native_queue_id ==
                wait.queue.native_queue_id) {
                error =
                    "ownership fan-in join redundantly targets one native queue";
                return false;
            }
            if (auto* existing =
                    find_lowered_sync(signal_batch, wait_batch);
                existing != nullptr) {
                append_unique_barrier(
                    existing->ownership_join_barriers,
                    barrier_index
                );
                return true;
            }
            candidate.queue_syncs.push_back(QueueSyncInstruction{
                .correlation_id = next_synthetic_sync_id++,
                .signal_pass    = signal.passes.back(),
                .wait_pass      = wait.passes.front(),
                .signal_queue   = signal.queue,
                .wait_queue     = wait.queue,
                .signal_batch   = signal_batch,
                .wait_batch     = wait_batch,
                .synthetic_ownership_join = true,
                .ownership_join_barriers = {barrier_index},
            });
            return true;
        };

    for (uint32_t barrier_index = 0; barrier_index < compiled.barriers.size(); ++barrier_index) {
        const auto& barrier = compiled.barriers[barrier_index];
        if (prologue[barrier_index] && epilogue[barrier_index]) {
            return fail("one barrier cannot be both prologue and epilogue");
        }
        if (prologue[barrier_index]) {
            if (!barrier.import_boundary || barrier.src_pass.IsValid() ||
                !barrier.dst_pass.IsValid() || before_occurrences[barrier_index] != 1) {
                return fail("prologue barrier has inconsistent placement metadata");
            }
        } else if (epilogue[barrier_index]) {
            if (!barrier.export_boundary || barrier.dst_pass.IsValid() ||
                before_occurrences[barrier_index] != 0) {
                return fail("epilogue barrier has inconsistent placement metadata");
            }
        } else if (!barrier.dst_pass.IsValid() || before_occurrences[barrier_index] != 1) {
            return fail("internal barrier does not have exactly one before-pass placement");
        }

        LoweredInstruction instruction{};
        if (!lower_instruction(barrier_index, instruction)) {
            return fail("barrier " + std::to_string(barrier_index) + ": " + std::move(error));
        }
        if (barrier.queue_ownership) {
            const uint32_t destination_batch =
                pass_queue_batch[barrier.dst_pass.index];
            if (destination_batch == RenderGraph::PassHandle::InvalidIndex ||
                destination_batch >= compiled.queue_batches.size()) {
                return fail(
                    "ownership destination has no managed queue batch"
                );
            }
            const auto& destination_binding =
                compiled.queue_batches[destination_batch].queue;

            struct SourceNativeTail {
                uint32_t native_queue_id = 0;
                uint32_t batch = RenderGraph::PassHandle::InvalidIndex;
            };
            std::vector<SourceNativeTail> source_tails{};
            uint32_t source_family = RenderGraph::PassHandle::InvalidIndex;
            for (const auto& source : barrier.sources) {
                if (!graph.IsValidPass(source.pass)) {
                    return fail(
                        "ownership source frontier contains an invalid pass"
                    );
                }
                const uint32_t source_batch =
                    pass_queue_batch[source.pass.index];
                if (source_batch == RenderGraph::PassHandle::InvalidIndex ||
                    source_batch >= compiled.queue_batches.size() ||
                    source_batch >= destination_batch) {
                    return fail(
                        "ownership source is not a forward managed queue batch"
                    );
                }
                const auto& source_binding =
                    compiled.queue_batches[source_batch].queue;
                if (source_binding.role != source.domain.queue) {
                    return fail(
                        "ownership source domain disagrees with its queue batch"
                    );
                }
                if (source_family == RenderGraph::PassHandle::InvalidIndex) {
                    source_family = source_binding.family_id;
                } else if (source_family != source_binding.family_id) {
                    return fail(
                        "ownership source frontier spans multiple queue families"
                    );
                }
                auto tail = std::find_if(
                    source_tails.begin(),
                    source_tails.end(),
                    [&](const SourceNativeTail& candidate_tail) {
                        return candidate_tail.native_queue_id ==
                               source_binding.native_queue_id;
                    }
                );
                if (tail == source_tails.end()) {
                    source_tails.push_back(SourceNativeTail{
                        .native_queue_id = source_binding.native_queue_id,
                        .batch           = source_batch,
                    });
                } else {
                    tail->batch = std::max(tail->batch, source_batch);
                }
            }
            if (source_tails.empty() ||
                source_family == RenderGraph::PassHandle::InvalidIndex ||
                source_family == destination_binding.family_id) {
                return fail(
                    "ownership transfer does not cross two distinct queue families"
                );
            }

            const auto release_tail = std::max_element(
                source_tails.begin(),
                source_tails.end(),
                [](const SourceNativeTail& lhs,
                   const SourceNativeTail& rhs) {
                    return lhs.batch < rhs.batch;
                }
            );
            const uint32_t release_batch = release_tail->batch;
            const auto& release_binding =
                compiled.queue_batches[release_batch].queue;
            if (release_binding.family_id != source_family ||
                release_binding.native_queue_id ==
                    destination_binding.native_queue_id) {
                return fail(
                    "ownership release owner disagrees with the source family "
                    "or destination native queue"
                );
            }

            for (const SourceNativeTail& tail : source_tails) {
                if (tail.native_queue_id ==
                    release_binding.native_queue_id) {
                    continue;
                }
                if (!ensure_ownership_join(
                        tail.batch,
                        release_batch,
                        barrier_index
                    )) {
                    return fail(
                        "barrier " + std::to_string(barrier_index) +
                        ": " + std::move(error)
                    );
                }
            }

            QueueSyncInstruction* transfer_sync =
                find_lowered_sync(release_batch, destination_batch);
            if (transfer_sync == nullptr ||
                transfer_sync->signal_queue != release_binding ||
                transfer_sync->wait_queue != destination_binding) {
                return fail(
                    "ownership release is not correlated with its "
                    "release-to-acquire GPU sync"
                );
            }
            append_unique_barrier(
                transfer_sync->ownership_transfer_barriers,
                barrier_index
            );

            auto* release_pass = find_mutable_pass(
                compiled.queue_batches[release_batch].passes.back()
            );
            auto* acquire_pass = find_mutable_pass(barrier.dst_pass);
            if (release_pass == nullptr || acquire_pass == nullptr) {
                return fail(
                    "ownership release/acquire endpoint is not a GPU pass"
                );
            }

            LoweredInstruction release = instruction;
            release.instruction_kind = InstructionKind::QueueRelease;
            release.queue_acquire = false;
            release.transfer_source = release_binding;
            release.transfer_destination = destination_binding;

            LoweredInstruction acquire = instruction;
            acquire.instruction_kind = InstructionKind::QueueAcquire;
            acquire.queue_acquire = true;
            acquire.transfer_source = release_binding;
            acquire.transfer_destination = destination_binding;

            const auto keepalive = std::find_if(
                release_pass->keepalive.begin(),
                release_pass->keepalive.end(),
                [&](const PhysicalBinding& binding) {
                    return binding.resource == instruction.physical.resource &&
                           binding.Identity() == instruction.physical.Identity();
                }
            );
            if (keepalive == release_pass->keepalive.end()) {
                release_pass->keepalive.push_back(instruction.physical);
            }
            release_pass->after.push_back(std::move(release));
            acquire_pass->before.push_back(std::move(acquire));
            continue;
        }
        if (prologue[barrier_index]) {
            candidate.prologue.push_back(std::move(instruction));
            continue;
        }
        if (epilogue[barrier_index]) {
            RenderGraph::PassHandle latest{};
            uint32_t latest_position = 0;
            bool found_source = false;
            for (const auto source : instruction.source_frontier) {
                if (!graph.IsValidPass(source) ||
                    execution_position[source.index] == RenderGraph::PassHandle::InvalidIndex) {
                    return fail("epilogue barrier source is not in execution order");
                }
                if (!found_source || execution_position[source.index] > latest_position) {
                    latest = source;
                    latest_position = execution_position[source.index];
                    found_source = true;
                }
            }
            if (!found_source) {
                return fail("epilogue barrier has no managed source pass");
            }
            auto* pass = find_mutable_pass(latest);
            if (pass == nullptr) {
                return fail("epilogue barrier source is not a GPU pass");
            }
            pass->after.push_back(std::move(instruction));
            continue;
        }
        auto* pass = find_mutable_pass(barrier.dst_pass);
        if (pass == nullptr) {
            return fail("barrier destination is not a GPU pass");
        }
        pass->before.push_back(std::move(instruction));
    }

    auto has_boundary = [&](const RenderGraph::CompiledAccess& access, bool initial) {
        const auto& indices = initial ? compiled.prologue_barriers :
                                        compiled.epilogue_barriers;
        return std::any_of(indices.begin(), indices.end(), [&](uint32_t barrier_index) {
            const auto& barrier = compiled.barriers[barrier_index];
            return barrier.resource == access.resource && barrier.range == access.range &&
                   (initial ? barrier.import_boundary : barrier.export_boundary);
        });
    };
    for (const auto& access : compiled.accesses) {
        if (graph.resources[access.resource.index].kind == ResourceKind::Token) {
            continue;
        }
        const auto& declaration = graph.resources[access.resource.index];
        if (declaration.imported && !has_boundary(access, true)) {
            return fail(
                "physical access lacks a matching explicit import boundary on resource '" +
                declaration.name + "'"
            );
        }
        if ((declaration.imported || declaration.exported) &&
            !has_boundary(access, false)) {
            return fail(
                "physical access lacks a matching explicit export boundary on resource '" +
                declaration.name + "'"
            );
        }
    }

    for (uint32_t access_index = 0; access_index < compiled.accesses.size(); ++access_index) {
        const auto& access = compiled.accesses[access_index];
        const auto& resource = graph.resources[access.resource.index];
        if (resource.kind == ResourceKind::Token) {
            continue;
        }
        auto* pass = find_mutable_pass(access.pass);
        if (pass == nullptr) {
            return fail("physical access belongs to a non-GPU pass");
        }
        const bool state_already_adopted = std::any_of(
            pass->before.begin(),
            pass->before.end(),
            [&](const LoweredInstruction& instruction) {
                return instruction.resource == access.resource &&
                       instruction.range == access.range &&
                       instruction.after_state == access.state &&
                       instruction.after_access == access.mode;
            }
        ) || std::any_of(
            candidate.prologue.begin(),
            candidate.prologue.end(),
            [&](const LoweredInstruction& instruction) {
                return instruction.dst_pass == access.pass &&
                       instruction.resource == access.resource &&
                       instruction.range == access.range &&
                       instruction.after_state == access.state &&
                       instruction.after_access == access.mode;
            }
        );
        if (state_already_adopted) {
            continue;
        }

        Scope seed_scope{};
        if (!MapScope(access.state, access.mode, access.domain, seed_scope, error)) {
            return fail("access " + std::to_string(access_index) +
                        " cannot seed explicit tracked state: " + std::move(error));
        }
        pass->before.push_back(LoweredInstruction{
            .instruction_kind        = InstructionKind::StateSeed,
            .correlation_id          = access_index,
            .barrier_index           = InvalidBarrierIndex,
            .access_index            = access_index,
            .resource                = access.resource,
            .resource_kind           = resource.kind,
            .range                   = access.range,
            .texture_aspects         = resource.kind == ResourceKind::Texture ?
                                           LowerAspects(access.range.texture.aspects) :
                                           ETextureAspectFlags::NONE,
            .physical                = binding_for(access.resource),
            .src_pass                = access.pass,
            .dst_pass                = access.pass,
            .source                  = seed_scope,
            .destination             = seed_scope,
            .before_state            = access.state,
            .after_state             = access.state,
            .before_access           = access.mode,
            .after_access            = access.mode,
        });
    }

    for (uint32_t barrier_index = 0;
         barrier_index < compiled.barriers.size();
         ++barrier_index) {
        if (!compiled.barriers[barrier_index].queue_ownership) {
            continue;
        }
        const LoweredInstruction* release = nullptr;
        const LoweredInstruction* acquire = nullptr;
        uint32_t release_count = 0;
        uint32_t acquire_count = 0;
        for (const auto& pass : candidate.passes) {
            for (const auto& instruction : pass.after) {
                if (instruction.barrier_index == barrier_index &&
                    instruction.instruction_kind ==
                        InstructionKind::QueueRelease) {
                    release = &instruction;
                    ++release_count;
                }
            }
            for (const auto& instruction : pass.before) {
                if (instruction.barrier_index == barrier_index &&
                    instruction.instruction_kind ==
                        InstructionKind::QueueAcquire) {
                    acquire = &instruction;
                    ++acquire_count;
                }
            }
        }
        if (release_count != 1 || acquire_count != 1 ||
            release == nullptr || acquire == nullptr) {
            return fail(
                "ownership barrier does not have exactly one release/acquire pair"
            );
        }
        const auto same_scope = [](const Scope& lhs, const Scope& rhs) {
            return lhs.stages == rhs.stages &&
                   lhs.access == rhs.access &&
                   lhs.layout == rhs.layout &&
                   lhs.has_texture_layout == rhs.has_texture_layout;
        };
        if (release->correlation_id != acquire->correlation_id ||
            release->resource != acquire->resource ||
            release->resource_kind != acquire->resource_kind ||
            !(release->range == acquire->range) ||
            release->texture_aspects != acquire->texture_aspects ||
            release->physical.Identity() != acquire->physical.Identity() ||
            release->before_state != acquire->before_state ||
            release->after_state != acquire->after_state ||
            release->before_access != acquire->before_access ||
            release->after_access != acquire->after_access ||
            !same_scope(release->source, acquire->source) ||
            !same_scope(release->destination, acquire->destination) ||
            release->transfer_source != acquire->transfer_source ||
            release->transfer_destination !=
                acquire->transfer_destination ||
            release->transfer_source.family_id ==
                release->transfer_destination.family_id) {
            return fail(
                "ownership release/acquire pair lost canonical state, range, "
                "resource, or endpoints"
            );
        }
        const uint32_t transfer_sync_count = static_cast<uint32_t>(
            std::count_if(
                candidate.queue_syncs.begin(),
                candidate.queue_syncs.end(),
                [&](const QueueSyncInstruction& sync) {
                    return std::find(
                               sync.ownership_transfer_barriers.begin(),
                               sync.ownership_transfer_barriers.end(),
                               barrier_index
                           ) != sync.ownership_transfer_barriers.end();
                }
            )
        );
        if (transfer_sync_count != 1) {
            return fail(
                "ownership release/acquire pair does not own exactly one "
                "transfer sync"
            );
        }
    }

    output = std::move(candidate);
    error.clear();
    return true;
}

} // namespace Moer::Render
