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
           << " export=" << instruction.export_boundary;
}

} // namespace

void RenderGraphLowering::LoweredPlan::Clear() {
    prologue.clear();
    passes.clear();
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
    stream << "lowered prologue=" << prologue.size() << " passes=" << passes.size() << "\n";
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

    std::vector<bool> gpu_pass(graph.passes.size(), false);
    for (const auto& pass : graph.passes) {
        if (pass.execution_class == RenderGraph::PassExecutionClass::ExternalControl) {
            return fail("ExternalControl pass is outside the managed lowering domain: '" +
                        pass.name + "'");
        }
    }
    for (const auto& batch : compiled.queue_batches) {
        if (batch.external_control) {
            return fail("external-control queue batch is outside the managed lowering domain");
        }
        if (batch.queue.role != RenderGraph::QueueRole::Graphics) {
            return fail("only the Graphics logical queue is supported");
        }
        for (const auto pass : batch.passes) {
            if (!graph.IsValidPass(pass)) {
                return fail("queue batch references an invalid pass");
            }
            gpu_pass[pass.index] = true;
            if (graph.passes[pass.index].domain.queue != RenderGraph::QueueRole::Graphics) {
                return fail("GPU pass '" + graph.passes[pass.index].name +
                            "' does not use the Graphics logical queue");
            }
        }
    }
    for (const auto& sync : compiled.queue_syncs) {
        if (sync.gpu_wait_required ||
            sync.signal_queue.role != RenderGraph::QueueRole::Graphics ||
            sync.wait_queue.role != RenderGraph::QueueRole::Graphics) {
            return fail("cross-queue synchronization is not supported");
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
        if (access.domain.queue != RenderGraph::QueueRole::Graphics) {
            return fail("physical access does not use the Graphics logical queue on resource '" +
                        resource.name + "'");
        }
        if (access.state.kind == ResourceKind::Texture &&
            access.state.texture == TextureState::Present) {
            return fail("Present state requires an external synchronization endpoint on resource '" +
                        resource.name + "'");
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
        if (state.queue != RenderGraph::QueueRole::Graphics) {
            error = "only Graphics boundary ownership is supported on resource '" + resource.name + "'";
            return false;
        }
        return true;
    };

    for (uint32_t resource_index = 0; resource_index < graph.resources.size(); ++resource_index) {
        const auto& resource = graph.resources[resource_index];
        if (resource.kind == ResourceKind::Token) {
            continue;
        }
        if (!resource.imported) {
            return fail("transient physical resource is not supported: '" + resource.name + "'");
        }
        if (!resource.typed_desc) {
            return fail("physical resource requires a typed descriptor: '" + resource.name + "'");
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
        if (resource.initial_states.empty()) {
            return fail("physical resource lacks an explicit initial state: '" + resource.name + "'");
        }
        if (!resource.exported || resource.final_states.empty()) {
            return fail("physical resource lacks an explicit final state: '" + resource.name + "'");
        }
        for (const auto& state : resource.initial_states) {
            if (!validate_boundary(resource, state, true)) {
                return fail(std::move(error));
            }
        }
        for (const auto& state : resource.final_states) {
            if (!validate_boundary(resource, state, false)) {
                return fail(std::move(error));
            }
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
        if (!graph.IsValidResource(barrier.resource)) {
            error = "barrier references an invalid resource";
            return false;
        }
        const auto& resource = graph.resources[barrier.resource.index];
        if (resource.kind == ResourceKind::Token) {
            error = "token barrier cannot be physically lowered";
            return false;
        }
        if (barrier.queue_dependency || barrier.queue_ownership) {
            error = "queue dependency or ownership transfer is not supported";
            return false;
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
                barrier.dst_domain.queue != RenderGraph::QueueRole::Graphics) {
                error = "barrier destination is not a valid Graphics pass";
                return false;
            }
        } else if (!barrier.export_boundary ||
                   barrier.dst_domain.queue != RenderGraph::QueueRole::Graphics) {
            error = "barrier has no managed Graphics destination";
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
                    source.domain.queue != RenderGraph::QueueRole::Graphics ||
                    !IsKnownAccess(source.access) || source.state.IsAutomatic()) {
                    error = "barrier fan-in contains an unsupported source";
                    return false;
                }
                Scope source_scope{};
                if (!MapScope(source.state, source.access, source.domain, source_scope, error)) {
                    return false;
                }
                instruction.source_frontier.push_back(source.pass);
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
        if (instruction.discard_previous_contents && !barrier.before_state.IsUndefined()) {
            error = "discard flag requires an Undefined source state";
            return false;
        }
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
        if (!has_boundary(access, true)) {
            return fail("physical access lacks a matching explicit import boundary on resource '" +
                        graph.resources[access.resource.index].name + "'");
        }
        if (!has_boundary(access, false)) {
            return fail("physical access lacks a matching explicit export boundary on resource '" +
                        graph.resources[access.resource.index].name + "'");
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

    output = std::move(candidate);
    error.clear();
    return true;
}

} // namespace Moer::Render
