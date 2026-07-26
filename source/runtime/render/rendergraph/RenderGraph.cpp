#include "rendergraph/RenderGraph.h"

#include "log/LogSystem.h"
#include "rendergraph/RenderGraphCompiler.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rhi/RHI.h"
#include "rhi/RHIExecutor.h"
#include "rhi/RHIThreadOwnership.h"
#include "taskgraph/TaskGraph.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace Moer::Render {

namespace {

std::atomic<uint64_t> s_next_graph_id{1};

[[nodiscard]] bool IsValidTextureDesc(const RenderGraph::TextureDesc& desc) {
    const auto aspect_mask = static_cast<uint8_t>(desc.aspects);
    const auto known_mask  = static_cast<uint8_t>(RenderGraph::TextureAspect::All);
    return desc.mip_count != 0 && desc.layer_count != 0 && aspect_mask != 0 &&
           (aspect_mask & known_mask) == aspect_mask;
}

const char* ToString(RenderGraph::ResourceKind kind) {
    switch (kind) {
        case RenderGraph::ResourceKind::Texture:
            return "texture";
        case RenderGraph::ResourceKind::Buffer:
            return "buffer";
        case RenderGraph::ResourceKind::Token:
            return "token";
    }
    return "unknown";
}

const char* ToString(RenderGraph::AccessMode mode) {
    switch (mode) {
        case RenderGraph::AccessMode::Unknown:
            return "unknown";
        case RenderGraph::AccessMode::None:
            return "none";
        case RenderGraph::AccessMode::Read:
            return "R";
        case RenderGraph::AccessMode::Write:
            return "W";
        case RenderGraph::AccessMode::ReadWrite:
            return "RW";
    }
    return "?";
}

const char* ToString(RenderGraph::EdgeReasonKind kind) {
    switch (kind) {
        case RenderGraph::EdgeReasonKind::Explicit:
            return "explicit";
        case RenderGraph::EdgeReasonKind::ReadAfterWrite:
            return "RAW";
        case RenderGraph::EdgeReasonKind::WriteAfterRead:
            return "WAR";
        case RenderGraph::EdgeReasonKind::WriteAfterWrite:
            return "WAW";
        case RenderGraph::EdgeReasonKind::StateTransition:
            return "state";
        case RenderGraph::EdgeReasonKind::QueueOwnership:
            return "ownership";
        case RenderGraph::EdgeReasonKind::TransientAlias:
            return "transient-alias";
    }
    return "unknown";
}

const char* ToString(RenderGraph::QueueRole queue) {
    switch (queue) {
        case RenderGraph::QueueRole::None:
            return "none";
        case RenderGraph::QueueRole::Graphics:
            return "graphics";
        case RenderGraph::QueueRole::Compute:
            return "compute";
        case RenderGraph::QueueRole::Copy:
            return "copy";
    }
    return "unknown";
}

const char* ToString(RenderGraph::PipelineType pipeline) {
    switch (pipeline) {
        case RenderGraph::PipelineType::None:
            return "none";
        case RenderGraph::PipelineType::Graphics:
            return "graphics";
        case RenderGraph::PipelineType::Compute:
            return "compute";
        case RenderGraph::PipelineType::RayTracing:
            return "raytracing";
        case RenderGraph::PipelineType::Copy:
            return "copy";
    }
    return "unknown";
}

const char* ToString(RenderGraph::PassExecutionClass execution) {
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

const char* ToString(ERHITranslateExecutionClass execution) {
    switch (execution) {
        case ERHITranslateExecutionClass::Parallel:
            return "parallel";
        case ERHITranslateExecutionClass::SerialControl:
            return "serial-control";
    }
    return "unknown";
}

const char* ToString(RenderGraph::TextureState state) {
    switch (state) {
        case RenderGraph::TextureState::Automatic:
            return "automatic";
        case RenderGraph::TextureState::Undefined:
            return "undefined";
        case RenderGraph::TextureState::TransferSource:
            return "transfer-src";
        case RenderGraph::TextureState::TransferDestination:
            return "transfer-dst";
        case RenderGraph::TextureState::ShaderResource:
            return "shader-resource";
        case RenderGraph::TextureState::Sampled:
            return "sampled";
        case RenderGraph::TextureState::RenderTarget:
            return "render-target";
        case RenderGraph::TextureState::DepthStencilRead:
            return "depth-read";
        case RenderGraph::TextureState::DepthStencilWrite:
            return "depth-write";
        case RenderGraph::TextureState::UnorderedAccess:
            return "unordered-access";
        case RenderGraph::TextureState::Present:
            return "present";
    }
    return "unknown";
}

const char* ToString(RenderGraph::BufferState state) {
    switch (state) {
        case RenderGraph::BufferState::Automatic:
            return "automatic";
        case RenderGraph::BufferState::Undefined:
            return "undefined";
        case RenderGraph::BufferState::TransferSource:
            return "transfer-src";
        case RenderGraph::BufferState::TransferDestination:
            return "transfer-dst";
        case RenderGraph::BufferState::VertexBuffer:
            return "vertex-buffer";
        case RenderGraph::BufferState::IndexBuffer:
            return "index-buffer";
        case RenderGraph::BufferState::IndirectArgument:
            return "indirect-argument";
        case RenderGraph::BufferState::ShaderResource:
            return "shader-resource";
        case RenderGraph::BufferState::UnorderedAccess:
            return "unordered-access";
        case RenderGraph::BufferState::AccelerationStructureBuildInput:
            return "as-build-input";
        case RenderGraph::BufferState::AccelerationStructureRead:
            return "as-read";
        case RenderGraph::BufferState::AccelerationStructureWrite:
            return "as-write";
    }
    return "unknown";
}

void AppendState(std::ostringstream& stream, const RenderGraph::ResourceState& state) {
    switch (state.kind) {
        case RenderGraph::ResourceKind::Texture:
            stream << ToString(state.texture);
            break;
        case RenderGraph::ResourceKind::Buffer:
            stream << ToString(state.buffer);
            break;
        case RenderGraph::ResourceKind::Token:
            stream << "logical";
            break;
    }
}

void AppendTextureAspects(std::ostringstream& stream, RenderGraph::TextureAspect aspects) {
    const auto mask   = static_cast<uint8_t>(aspects);
    bool       wrote  = false;
    auto       append = [&](RenderGraph::TextureAspect aspect, const char* name) {
        if ((mask & static_cast<uint8_t>(aspect)) == 0) {
            return;
        }
        if (wrote) {
            stream << '|';
        }
        stream << name;
        wrote = true;
    };
    append(RenderGraph::TextureAspect::Color, "color");
    append(RenderGraph::TextureAspect::Depth, "depth");
    append(RenderGraph::TextureAspect::Stencil, "stencil");
    if (!wrote) {
        stream << "none";
    }
}

void AppendCount(std::ostringstream& stream, uint64_t count, uint64_t remaining) {
    if (count == remaining) {
        stream << "remaining";
    } else {
        stream << count;
    }
}

void AppendRange(std::ostringstream& stream, const RenderGraph::ResourceRange& range) {
    switch (range.kind) {
        case RenderGraph::ResourceKind::Texture:
            stream << "texture[aspect=";
            AppendTextureAspects(stream, range.texture.aspects);
            stream << " mip=" << range.texture.mip_first << '+';
            AppendCount(stream, range.texture.mip_count, RenderGraph::RemainingTextureRange);
            stream << " layer=" << range.texture.layer_first << '+';
            AppendCount(stream, range.texture.layer_count, RenderGraph::RemainingTextureRange);
            stream << ']';
            break;
        case RenderGraph::ResourceKind::Buffer:
            stream << "buffer[offset=" << range.buffer.offset << " size=";
            AppendCount(stream, range.buffer.size, RenderGraph::RemainingBufferRange);
            stream << ']';
            break;
        case RenderGraph::ResourceKind::Token:
            stream << "token[all]";
            break;
    }
}

void AppendVersion(std::ostringstream& stream, uint32_t version) {
    if (version == RenderGraph::InvalidVersion) {
        stream << "none";
    } else {
        stream << 'v' << version;
    }
}

} // namespace

RenderGraph::RenderGraph(std::string_view graph_name) :
    RenderGraph(graph_name, QueueTopology::SingleQueue()) {}

RenderGraph::QueueTopology RenderGraph::QueueTopology::FromRHI() {
    const RHIQueueTopology topology = RenderDevice::Get().GetQueueTopology();
    auto convert = [](QueueRole role, const RHIQueueBinding& binding) {
        return QueueBinding{
            .role            = role,
            .native_queue_id = binding.native_queue_id,
            .family_id       = binding.family_id,
            .available       = binding.available,
        };
    };
    return QueueTopology{
        .graphics = convert(QueueRole::Graphics, topology.graphics),
        .compute  = convert(QueueRole::Compute, topology.compute),
        .copy     = convert(QueueRole::Copy, topology.copy),
    };
}

RenderGraph::RenderGraph(std::string_view graph_name, QueueTopology topology) :
    name(graph_name),
    queue_topology(topology),
    graph_id(s_next_graph_id.fetch_add(1, std::memory_order_relaxed)) {
    assert(graph_id != 0 && "RenderGraph id counter wrapped.");
}

RenderGraph::~RenderGraph() = default;

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Read(ResourceHandle resource, std::string_view range) {
    ResourceRange typed_range = ResourceRange::Token();
    if (graph.IsValidResource(resource)) {
        typed_range = ResourceRange::Whole(graph.resources[resource.index].kind);
    }
    graph.AddAccess(
        pass_index,
        resource,
        AccessMode::Read,
        typed_range,
        ResourceState{.kind = typed_range.kind},
        false,
        false,
        range
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Write(ResourceHandle resource, std::string_view range) {
    ResourceRange typed_range = ResourceRange::Token();
    if (graph.IsValidResource(resource)) {
        typed_range = ResourceRange::Whole(graph.resources[resource.index].kind);
    }
    graph.AddAccess(
        pass_index,
        resource,
        AccessMode::Write,
        typed_range,
        ResourceState{.kind = typed_range.kind},
        false,
        false,
        range
    );
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::ReadWrite(ResourceHandle resource, std::string_view range) {
    ResourceRange typed_range = ResourceRange::Token();
    if (graph.IsValidResource(resource)) {
        typed_range = ResourceRange::Whole(graph.resources[resource.index].kind);
    }
    graph.AddAccess(
        pass_index,
        resource,
        AccessMode::ReadWrite,
        typed_range,
        ResourceState{.kind = typed_range.kind},
        false,
        false,
        range
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Read(TextureHandle resource, TextureRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Read,
        ResourceRange::Texture(range),
        ResourceState::Texture(TextureState::Automatic),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Write(TextureHandle resource, TextureRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Write,
        ResourceRange::Texture(range),
        ResourceState::Texture(TextureState::Automatic),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadWrite(TextureHandle resource, TextureRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::ReadWrite,
        ResourceRange::Texture(range),
        ResourceState::Texture(TextureState::Automatic),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::Read(TextureHandle resource, TextureState state, TextureRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Read,
        ResourceRange::Texture(range),
        ResourceState::Texture(state),
        true,
        true
    );
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::Write(TextureHandle resource, TextureState state, TextureRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Write,
        ResourceRange::Texture(range),
        ResourceState::Texture(state),
        true,
        true
    );
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::ReadWrite(TextureHandle resource, TextureState state, TextureRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::ReadWrite,
        ResourceRange::Texture(range),
        ResourceState::Texture(state),
        true,
        true
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Read(BufferHandle resource, BufferRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Read,
        ResourceRange::Buffer(range),
        ResourceState::Buffer(BufferState::Automatic),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Write(BufferHandle resource, BufferRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Write,
        ResourceRange::Buffer(range),
        ResourceState::Buffer(BufferState::Automatic),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadWrite(BufferHandle resource, BufferRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::ReadWrite,
        ResourceRange::Buffer(range),
        ResourceState::Buffer(BufferState::Automatic),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::Read(BufferHandle resource, BufferState state, BufferRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Read,
        ResourceRange::Buffer(range),
        ResourceState::Buffer(state),
        true,
        true
    );
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::Write(BufferHandle resource, BufferState state, BufferRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Write,
        ResourceRange::Buffer(range),
        ResourceState::Buffer(state),
        true,
        true
    );
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::ReadWrite(BufferHandle resource, BufferState state, BufferRange range) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::ReadWrite,
        ResourceRange::Buffer(range),
        ResourceState::Buffer(state),
        true,
        true
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Read(TokenHandle resource) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Read,
        ResourceRange::Token(),
        ResourceState::Token(),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Write(TokenHandle resource) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::Write,
        ResourceRange::Token(),
        ResourceState::Token(),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadWrite(TokenHandle resource) {
    graph.AddAccess(
        pass_index,
        resource.Untyped(),
        AccessMode::ReadWrite,
        ResourceRange::Token(),
        ResourceState::Token(),
        true,
        false
    );
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Reference(ResourceHandle resource) {
    graph.AddReference(pass_index, resource);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::DependsOn(PassHandle dependency) {
    graph.AddDependency(pass_index, dependency);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::SideEffect() {
    graph.MarkSideEffect(pass_index);
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::ExecuteOn(QueueRole queue, PipelineType pipeline) {
    graph.SetExecutionDomain(pass_index, queue, pipeline);
    return *this;
}

[[nodiscard]] EQueueType ToRHIQueue(RenderGraph::QueueRole queue) {
    switch (queue) {
        case RenderGraph::QueueRole::Graphics:
            return EQueueType::Graphics;
        case RenderGraph::QueueRole::Compute:
            return EQueueType::Compute;
        case RenderGraph::QueueRole::Copy:
            return EQueueType::Copy;
        case RenderGraph::QueueRole::None:
            return EQueueType::Ignore;
    }
    return EQueueType::Ignore;
}

[[nodiscard]] RenderGraph::TextureAspect
RaiseTextureAspects(ETextureAspectFlags aspects) {
    RenderGraph::TextureAspect result = RenderGraph::TextureAspect::None;
    const uint32_t mask = static_cast<uint32_t>(aspects);
    if ((mask & static_cast<uint32_t>(ETextureAspectFlags::COLOR)) != 0) {
        result = result | RenderGraph::TextureAspect::Color;
    }
    if ((mask & static_cast<uint32_t>(ETextureAspectFlags::DEPTH_SLICE)) != 0) {
        result = result | RenderGraph::TextureAspect::Depth;
    }
    if ((mask & static_cast<uint32_t>(ETextureAspectFlags::STENCIL_SLICE)) != 0) {
        result = result | RenderGraph::TextureAspect::Stencil;
    }
    return result;
}

struct MaterializedPassState {
    std::vector<BarrierCreateInfo>                    before{};
    std::vector<BarrierCreateInfo>                    after{};
    std::vector<RenderGraphLowering::PhysicalBinding> keepalive{};
    Array<RHIRecordingFencePoint>                     wait_fences{};
    Array<RHIRecordingFencePoint>                     signal_fences{};
    uint64                                            async_queue_scope{0};

    [[nodiscard]] bool RequiresCompletionLifetime() const {
        return !before.empty() || !after.empty() || !keepalive.empty();
    }
};

[[nodiscard]] BarrierState
ToBarrierState(const RenderGraphLowering::Scope& scope, bool texture) {
    return texture ? BarrierState::Texture(scope.stages, scope.access, scope.layout) :
                     BarrierState::Buffer(scope.stages, scope.access);
}

[[nodiscard]] bool MaterializeInstruction(
    const RenderGraphLowering::LoweredInstruction& instruction,
    BarrierCreateInfo&                             output,
    std::string&                                   error
) {
    auto materialize_transfer = [&]() {
        if (instruction.instruction_kind !=
                RenderGraphLowering::InstructionKind::QueueRelease &&
            instruction.instruction_kind !=
                RenderGraphLowering::InstructionKind::QueueAcquire) {
            output.queue_transfer = {};
            return true;
        }
        const auto src_queue = ToRHIQueue(instruction.transfer_source.role);
        const auto dst_queue =
            ToRHIQueue(instruction.transfer_destination.role);
        if ((src_queue != EQueueType::Graphics &&
             src_queue != EQueueType::Compute &&
             src_queue != EQueueType::Copy) ||
            (dst_queue != EQueueType::Graphics &&
             dst_queue != EQueueType::Compute &&
             dst_queue != EQueueType::Copy) ||
            src_queue == dst_queue ||
            instruction.transfer_source.family_id ==
                instruction.transfer_destination.family_id) {
            error =
                "queue ownership instruction has invalid logical or family endpoints";
            return false;
        }
        output.queue_transfer =
            instruction.instruction_kind ==
                    RenderGraphLowering::InstructionKind::QueueRelease ?
                BarrierQueueTransfer::Release(src_queue, dst_queue) :
                BarrierQueueTransfer::Acquire(src_queue, dst_queue);
        return true;
    };

    if (instruction.resource_kind == RenderGraph::ResourceKind::Texture) {
        const auto& binding = instruction.physical.texture;
        const auto& range   = instruction.range.texture;
        if (!binding.IsValid() || instruction.texture_aspects == ETextureAspectFlags::NONE) {
            error = "texture instruction has no physical binding or aspect";
            return false;
        }
        if (instruction.texture_aspects != binding->GetAspectFlags()) {
            error =
                "partial-aspect texture state is not supported by the active "
                "backend-tracker bridge";
            return false;
        }
        constexpr uint32_t max_view_index = std::numeric_limits<uint8_t>::max();
        if (range.mip_count == 0 || range.layer_count == 0 ||
            range.mip_first > max_view_index || range.mip_count > max_view_index ||
            range.layer_first > max_view_index || range.layer_count > max_view_index ||
            range.mip_first + range.mip_count > binding->GetNumMips() ||
            range.layer_first + range.layer_count > binding->GetNumArray()) {
            error = "texture instruction range cannot be represented by an RHI TextureView";
            return false;
        }

        TextureView view(binding);
        view.mip_level   = static_cast<uint8>(range.mip_first);
        view.num_mips    = static_cast<uint8>(range.mip_count);
        view.array_layer = static_cast<uint8>(range.layer_first);
        view.num_array   = static_cast<uint8>(range.layer_count);
        output = BarrierCreateInfo::Transition(
            view,
            ToBarrierState(instruction.source, true),
            ToBarrierState(instruction.destination, true),
            instruction.texture_aspects
        );
        return materialize_transfer();
    }

    if (instruction.resource_kind == RenderGraph::ResourceKind::Buffer) {
        const auto& binding = instruction.physical.buffer;
        const auto& range   = instruction.range.buffer;
        if (!binding.IsValid() || range.size == 0 || range.offset > binding->GetByteSize() ||
            range.size > binding->GetByteSize() - range.offset) {
            error = "buffer instruction has no physical binding or has an invalid range";
            return false;
        }
        if (range.offset != 0 || range.size != binding->GetByteSize()) {
            error =
                "partial buffer state is not supported by the active "
                "backend-tracker bridge";
            return false;
        }
        output = BarrierCreateInfo::Transition(
            BufferView(binding.Get(), range.offset, range.size, 1),
            ToBarrierState(instruction.source, false),
            ToBarrierState(instruction.destination, false)
        );
        return materialize_transfer();
    }

    error = "token instruction cannot be materialized as an RHI barrier";
    return false;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::MainThread() {
    graph.SetPassExecutionClass(pass_index, PassExecutionClass::MainThread, 1);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::CpuPrepare() {
    graph.SetPassExecutionClass(pass_index, PassExecutionClass::CpuPrepare, 1);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::ExternalControl() {
    graph.SetPassExecutionClass(pass_index, PassExecutionClass::ExternalControl, 1);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::SerialRecord(uint32_t workload) {
    graph.SetPassExecutionClass(pass_index, PassExecutionClass::SerialRecord, workload);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::ParallelRecord(uint32_t workload) {
    graph.SetPassExecutionClass(pass_index, PassExecutionClass::ParallelRecordEligible, workload);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::TranslateSerialControl() {
    graph.SetPassTranslateExecutionClass(
        pass_index,
        ERHITranslateExecutionClass::SerialControl
    );
    return *this;
}

RenderGraph::ResourceHandle
RenderGraph::Import(std::string_view resource_name, ResourceKind kind, const void* physical_identity) {
    return ImportInternal(resource_name, kind, physical_identity, nullptr, nullptr);
}

RenderGraph::TextureHandle
RenderGraph::ImportTexture(std::string_view resource_name, const void* physical_identity, TextureDesc desc) {
    return TextureHandle{
        ImportInternal(resource_name, ResourceKind::Texture, physical_identity, &desc, nullptr)
    };
}

RenderGraph::TextureHandle
RenderGraph::ImportTexture(std::string_view resource_name, TextureRef texture, TextureDesc desc) {
    if (!texture.IsValid()) {
        InvalidateCompile();
        declaration_errors.emplace_back(
            "strong texture import requires a valid resource: " + std::string(resource_name)
        );
        return {};
    }
    const auto handle =
        ImportInternal(resource_name, ResourceKind::Texture, texture.Get(), &desc, nullptr);
    if (handle.IsValid()) {
        auto& resource = resources[handle.index];
        if (resource.physical_texture.IsValid() &&
            resource.physical_texture.Get() != texture.Get()) {
            declaration_errors.emplace_back(
                "strong texture import conflicts with the canonical physical resource: " +
                std::string(resource_name)
            );
            return {};
        }
        resource.physical_texture = std::move(texture);
    }
    return TextureHandle{handle};
}

RenderGraph::BufferHandle
RenderGraph::ImportBuffer(std::string_view resource_name, const void* physical_identity, BufferDesc desc) {
    return BufferHandle{ImportInternal(resource_name, ResourceKind::Buffer, physical_identity, nullptr, &desc)
    };
}

RenderGraph::BufferHandle
RenderGraph::ImportBuffer(std::string_view resource_name, BufferRef buffer, BufferDesc desc) {
    if (!buffer.IsValid()) {
        InvalidateCompile();
        declaration_errors.emplace_back(
            "strong buffer import requires a valid resource: " + std::string(resource_name)
        );
        return {};
    }
    const auto handle =
        ImportInternal(resource_name, ResourceKind::Buffer, buffer.Get(), nullptr, &desc);
    if (handle.IsValid()) {
        auto& resource = resources[handle.index];
        if (resource.physical_buffer.IsValid() &&
            resource.physical_buffer.Get() != buffer.Get()) {
            declaration_errors.emplace_back(
                "strong buffer import conflicts with the canonical physical resource: " +
                std::string(resource_name)
            );
            return {};
        }
        resource.physical_buffer = std::move(buffer);
    }
    return BufferHandle{handle};
}

RenderGraph::TokenHandle
RenderGraph::ImportToken(std::string_view resource_name, const void* physical_identity) {
    return TokenHandle{ImportInternal(resource_name, ResourceKind::Token, physical_identity, nullptr, nullptr)
    };
}

RenderGraph::ResourceHandle RenderGraph::ImportInternal(
    std::string_view   resource_name,
    ResourceKind       kind,
    const void*        physical_identity,
    const TextureDesc* texture_desc,
    const BufferDesc*  buffer_desc
) {
    if (!InvalidateCompile()) {
        return {};
    }
    if (resource_name.empty()) {
        declaration_errors.emplace_back("resource name cannot be empty");
        return {};
    }
    if (texture_desc != nullptr && !IsValidTextureDesc(*texture_desc)) {
        declaration_errors.emplace_back("typed texture descriptor is invalid: " + std::string(resource_name));
        return {};
    }
    if (buffer_desc != nullptr && buffer_desc->byte_size == 0) {
        declaration_errors.emplace_back(
            "typed buffer descriptor has zero byte size: " + std::string(resource_name)
        );
        return {};
    }

    for (uint32_t index = 0; index < resources.size(); ++index) {
        auto& resource = resources[index];
        if (resource.name == resource_name ||
            std::find(resource.aliases.begin(), resource.aliases.end(), resource_name) !=
                resource.aliases.end()) {
            declaration_errors.emplace_back(
                "duplicate resource or alias name: " + std::string(resource_name)
            );
            return {};
        }
        if (physical_identity == nullptr || resource.physical_identity != physical_identity) {
            continue;
        }
        if (!resource.imported) {
            declaration_errors.emplace_back(
                "physical identity aliases transient resource: " + std::string(resource_name)
            );
            return {};
        }
        if (resource.kind != kind) {
            declaration_errors.emplace_back(
                "physical identity imported with different resource kinds: " + std::string(resource_name)
            );
            return {};
        }
        if (texture_desc != nullptr) {
            if (resource.typed_desc && resource.texture_desc != *texture_desc) {
                declaration_errors.emplace_back(
                    "physical texture alias imported with a different descriptor: " +
                    std::string(resource_name)
                );
                return {};
            }
            resource.texture_desc = *texture_desc;
            resource.typed_desc   = true;
        }
        if (buffer_desc != nullptr) {
            if (resource.typed_desc && resource.buffer_desc != *buffer_desc) {
                declaration_errors.emplace_back(
                    "physical buffer alias imported with a different descriptor: " +
                    std::string(resource_name)
                );
                return {};
            }
            resource.buffer_desc = *buffer_desc;
            resource.typed_desc  = true;
        }
        resource.aliases.emplace_back(resource_name);
        return ResourceHandle{index, graph_id};
    }

    ResourceDeclaration resource{};
    resource.name              = resource_name;
    resource.kind              = kind;
    resource.physical_identity = physical_identity;
    resource.imported          = true;
    if (texture_desc != nullptr) {
        resource.texture_desc = *texture_desc;
        resource.typed_desc   = true;
    }
    if (buffer_desc != nullptr) {
        resource.buffer_desc = *buffer_desc;
        resource.typed_desc  = true;
    }
    resources.emplace_back(std::move(resource));
    return ResourceHandle{static_cast<uint32_t>(resources.size() - 1), graph_id};
}

RenderGraph::ResourceHandle RenderGraph::CreateTransient(std::string_view resource_name, ResourceKind kind) {
    return CreateTransientInternal(resource_name, kind, nullptr, nullptr);
}

RenderGraph::TextureHandle
RenderGraph::CreateTransientTexture(std::string_view resource_name, TextureDesc desc) {
    return TextureHandle{CreateTransientInternal(resource_name, ResourceKind::Texture, &desc, nullptr)};
}

RenderGraph::BufferHandle
RenderGraph::CreateTransientBuffer(std::string_view resource_name, BufferDesc desc) {
    return BufferHandle{CreateTransientInternal(resource_name, ResourceKind::Buffer, nullptr, &desc)};
}

RenderGraph::TextureHandle RenderGraph::CreateTransientTexture(
    std::string_view               resource_name,
    const RGTransientTextureDesc& allocation_desc
) {
    if (!allocation_desc.IsValid()) {
        if (InvalidateCompile()) {
            declaration_errors.emplace_back(
                "transient texture allocation descriptor is invalid: " +
                std::string(resource_name)
            );
        }
        return {};
    }
    const TextureDesc logical_desc{
        .mip_count   = allocation_desc.mip_count,
        .layer_count = allocation_desc.PhysicalLayerCount(),
        .aspects     = RaiseTextureAspects(allocation_desc.aspect_flags),
    };
    const auto handle =
        TextureHandle{CreateTransientInternal(
            resource_name,
            ResourceKind::Texture,
            &logical_desc,
            nullptr
        )};
    if (handle.IsValid()) {
        resources[handle.resource.index].transient_texture_desc =
            allocation_desc;
    }
    return handle;
}

RenderGraph::BufferHandle RenderGraph::CreateTransientBuffer(
    std::string_view              resource_name,
    const RGTransientBufferDesc& allocation_desc
) {
    if (!allocation_desc.IsValid()) {
        if (InvalidateCompile()) {
            declaration_errors.emplace_back(
                "transient buffer allocation descriptor is invalid: " +
                std::string(resource_name)
            );
        }
        return {};
    }
    const BufferDesc logical_desc{.byte_size = allocation_desc.ByteSize()};
    const auto handle =
        BufferHandle{CreateTransientInternal(
            resource_name,
            ResourceKind::Buffer,
            nullptr,
            &logical_desc
        )};
    if (handle.IsValid()) {
        resources[handle.resource.index].transient_buffer_desc =
            allocation_desc;
    }
    return handle;
}

RenderGraph::TokenHandle RenderGraph::CreateTransientToken(std::string_view resource_name) {
    return TokenHandle{CreateTransientInternal(resource_name, ResourceKind::Token, nullptr, nullptr)};
}

TextureRef RenderGraph::GetPhysicalTexture(TextureHandle resource) const {
    if (!IsValidResource(resource.Untyped())) {
        return {};
    }
    const auto& declaration = resources[resource.resource.index];
    return declaration.kind == ResourceKind::Texture ?
               declaration.physical_texture :
               TextureRef{};
}

BufferRef RenderGraph::GetPhysicalBuffer(BufferHandle resource) const {
    if (!IsValidResource(resource.Untyped())) {
        return {};
    }
    const auto& declaration = resources[resource.resource.index];
    return declaration.kind == ResourceKind::Buffer ?
               declaration.physical_buffer :
               BufferRef{};
}

RenderGraph::ResourceHandle RenderGraph::CreateTransientInternal(
    std::string_view   resource_name,
    ResourceKind       kind,
    const TextureDesc* texture_desc,
    const BufferDesc*  buffer_desc
) {
    if (!InvalidateCompile()) {
        return {};
    }
    if (resource_name.empty()) {
        declaration_errors.emplace_back("resource name cannot be empty");
        return {};
    }
    if (texture_desc != nullptr && !IsValidTextureDesc(*texture_desc)) {
        declaration_errors.emplace_back("typed texture descriptor is invalid: " + std::string(resource_name));
        return {};
    }
    if (buffer_desc != nullptr && buffer_desc->byte_size == 0) {
        declaration_errors.emplace_back(
            "typed buffer descriptor has zero byte size: " + std::string(resource_name)
        );
        return {};
    }
    for (const auto& resource : resources) {
        if (resource.name == resource_name ||
            std::find(resource.aliases.begin(), resource.aliases.end(), resource_name) !=
                resource.aliases.end()) {
            declaration_errors.emplace_back("duplicate resource name: " + std::string(resource_name));
            return {};
        }
    }

    ResourceDeclaration resource{};
    resource.name     = resource_name;
    resource.kind     = kind;
    resource.imported = false;
    if (texture_desc != nullptr) {
        resource.texture_desc = *texture_desc;
        resource.typed_desc   = true;
    }
    if (buffer_desc != nullptr) {
        resource.buffer_desc = *buffer_desc;
        resource.typed_desc  = true;
    }
    resources.emplace_back(std::move(resource));
    return ResourceHandle{static_cast<uint32_t>(resources.size() - 1), graph_id};
}

void RenderGraph::Export(ResourceHandle resource) {
    MarkExported(resource, true);
}

bool RenderGraph::MarkExported(ResourceHandle resource, bool whole_resource) {
    if (!InvalidateCompile()) {
        return false;
    }
    if (!IsValidResource(resource)) {
        declaration_errors.emplace_back("Export received an invalid resource handle");
        return false;
    }
    auto& declaration                    = resources[resource.index];
    declaration.exported                = true;
    declaration.whole_resource_exported |= whole_resource;
    return true;
}

void RenderGraph::SetInitialState(
    TextureHandle resource,
    TextureState  state,
    QueueRole     owner_queue,
    AccessMode    last_access,
    TextureRange  range
) {
    AddStateDeclaration(
        resource.Untyped(),
        ResourceRange::Texture(range),
        ResourceState::Texture(state),
        owner_queue,
        last_access,
        true
    );
}

void RenderGraph::SetInitialState(
    BufferHandle resource,
    BufferState  state,
    QueueRole    owner_queue,
    AccessMode   last_access,
    BufferRange  range
) {
    AddStateDeclaration(
        resource.Untyped(),
        ResourceRange::Buffer(range),
        ResourceState::Buffer(state),
        owner_queue,
        last_access,
        true
    );
}

void RenderGraph::Export(
    TextureHandle resource,
    TextureState  final_state,
    QueueRole     owner_queue,
    AccessMode    next_access,
    TextureRange  range
) {
    if (!MarkExported(resource.Untyped(), false)) {
        return;
    }
    AddStateDeclaration(
        resource.Untyped(),
        ResourceRange::Texture(range),
        ResourceState::Texture(final_state),
        owner_queue,
        next_access,
        false
    );
}

void RenderGraph::Export(
    BufferHandle resource,
    BufferState  final_state,
    QueueRole    owner_queue,
    AccessMode   next_access,
    BufferRange  range
) {
    if (!MarkExported(resource.Untyped(), false)) {
        return;
    }
    AddStateDeclaration(
        resource.Untyped(),
        ResourceRange::Buffer(range),
        ResourceState::Buffer(final_state),
        owner_queue,
        next_access,
        false
    );
}

RenderGraph::PassHandle
RenderGraph::AddPass(std::string_view pass_name, const SetupCallback& setup, ExecuteCallback execute) {
    if (!InvalidateCompile()) {
        return {};
    }
    if (pass_name.empty()) {
        declaration_errors.emplace_back("pass name cannot be empty");
        return {};
    }
    if (!execute) {
        declaration_errors.emplace_back("pass has no execute callback: " + std::string(pass_name));
        return {};
    }
    if (std::any_of(passes.begin(), passes.end(), [&](const PassDeclaration& pass) {
            return pass.name == pass_name;
        })) {
        declaration_errors.emplace_back("duplicate pass name: " + std::string(pass_name));
        return {};
    }

    PassDeclaration pass{};
    pass.name    = pass_name;
    pass.execute = std::move(execute);
    passes.emplace_back(std::move(pass));
    const uint32_t pass_index = static_cast<uint32_t>(passes.size() - 1);
    PassBuilder    builder(*this, pass_index);
    if (setup) {
        setup(builder);
    }
    return PassHandle{pass_index, graph_id};
}

RenderGraph::PassHandle RenderGraph::AddRecordPass(
    std::string_view     pass_name,
    const SetupCallback& setup,
    RecordCallback       record,
    PassExecutionClass   execution,
    uint32_t             workload
) {
    if (!InvalidateCompile()) {
        return {};
    }
    if (pass_name.empty()) {
        declaration_errors.emplace_back("pass name cannot be empty");
        return {};
    }
    if (!record) {
        declaration_errors.emplace_back("pass has no record callback: " + std::string(pass_name));
        return {};
    }
    if (std::any_of(passes.begin(), passes.end(), [&](const PassDeclaration& pass) {
            return pass.name == pass_name;
        })) {
        declaration_errors.emplace_back("duplicate pass name: " + std::string(pass_name));
        return {};
    }

    PassDeclaration pass{};
    pass.name            = pass_name;
    pass.record          = std::move(record);
    pass.execution_class = execution;
    pass.workload        = workload;
    passes.emplace_back(std::move(pass));
    const uint32_t pass_index = static_cast<uint32_t>(passes.size() - 1);
    PassBuilder    builder(*this, pass_index);
    if (setup) {
        setup(builder);
    }
    return PassHandle{pass_index, graph_id};
}

bool RenderGraph::Compile() {
    if (executed) {
        compile_error = "graph has already executed and cannot be compiled again";
        return false;
    }
    return RenderGraphCompiler(*this).Compile();
}

bool RenderGraph::Execute() {
    return Execute({});
}

bool RenderGraph::Execute(const PassCompletedCallback& after_pass) {
    if (!compiled) {
        compile_error = "Execute called before a successful Compile";
        return false;
    }
    if (executed) {
        compile_error = "a per-frame RenderGraph can only be executed once";
        return false;
    }
    const bool has_active_allocation_backed_transient = std::any_of(
        compiled_plan.resources.begin(),
        compiled_plan.resources.end(),
        [](const CompiledResource& resource) {
            return !resource.imported &&
                   resource.first_use != PassHandle::InvalidIndex &&
                   resource.transient_slot != PassHandle::InvalidIndex;
        }
    );
    if (has_active_allocation_backed_transient) {
        compile_error =
            "allocation-backed transient resources require active ExecuteRecording";
        return false;
    }
    if (std::any_of(compiled_plan.execution_order.begin(),
                    compiled_plan.execution_order.end(),
                    [&](PassHandle handle) { return static_cast<bool>(passes[handle.index].record); })) {
        compile_error = "serial Execute cannot run command-recording passes without owned CommandLists";
        return false;
    }
    executed = true;

    for (const PassHandle pass_handle : compiled_plan.execution_order) {
        auto& pass = passes[pass_handle.index];
        assert(pass.execute);
        pass.execute();
        if (after_pass) {
            after_pass(ExecutedPassInfo{
                .handle      = pass_handle,
                .name        = pass.name,
                .domain      = pass.domain,
                .side_effect = pass.side_effect,
                .execution_class = pass.execution_class,
                .translate_execution_class =
                    pass.translate_execution_class,
            });
        }
    }
    return true;
}

bool RenderGraph::ExecuteRecording(
    const PassCompletedCallback&        after_main_thread_pass,
    const RecordingSourceSetupCallback& configure_recording_source,
    bool                                parallel_recording_enabled,
    const RecordingBatchPublisher&      publish_recording_batch,
    const ActiveRecordingOptions&       active_recording
) {
    if (!compiled) {
        compile_error = "ExecuteRecording called before a successful Compile";
        return false;
    }
    if (executed) {
        compile_error = "a per-frame RenderGraph can only be executed once";
        return false;
    }

    struct TransientBindingReleaseGuard {
        RenderGraphTransientAllocator* allocator{nullptr};
        RenderGraph*                   graph{nullptr};
        bool                           armed{false};

        ~TransientBindingReleaseGuard() {
            if (armed && allocator != nullptr && graph != nullptr) {
                allocator->ReleaseNonExported(*graph);
            }
        }
    };

    const bool has_active_allocation_backed_transient = std::any_of(
        compiled_plan.resources.begin(),
        compiled_plan.resources.end(),
        [](const CompiledResource& resource) {
            return !resource.imported &&
                   resource.first_use != PassHandle::InvalidIndex &&
                   resource.transient_slot != PassHandle::InvalidIndex;
        }
    );
    if (has_active_allocation_backed_transient && !active_recording.enabled) {
        compile_error =
            "allocation-backed transient resources require active recording";
        return false;
    }

    std::vector<MaterializedPassState> active_pass_states(passes.size());
    TransientBindingReleaseGuard transient_release_guard{};
    bool active_has_physical_main_thread = false;
    bool active_async_multiqueue          = false;
    RenderGraphTransientAllocator* active_transient_allocator = nullptr;
    if (active_recording.enabled) {
        active_transient_allocator =
            active_recording.transient_allocator != nullptr ?
                active_recording.transient_allocator :
                &RenderGraphTransientAllocator::Global();
        std::string allocation_error{};
        if (!active_transient_allocator->Prepare(*this, allocation_error)) {
            compile_error = std::move(allocation_error);
            return false;
        }
        transient_release_guard.allocator = active_transient_allocator;
        transient_release_guard.graph     = this;
        transient_release_guard.armed     = true;

        RenderGraphLowering::LoweredPlan lowered_plan{};
        std::string                      lowering_error{};
        if (!RenderGraphLowering::Lower(*this, lowered_plan, lowering_error)) {
            compile_error = std::move(lowering_error);
            return false;
        }
        const bool active_uses_non_graphics_queue = std::any_of(
            compiled_plan.queue_batches.begin(),
            compiled_plan.queue_batches.end(),
            [](const CompiledQueueBatch& batch) {
                return batch.queue.role != QueueRole::Graphics;
            }
        );
        if (active_uses_non_graphics_queue) {
            if (!RenderDevice::IsInitialized()) {
                compile_error =
                    "active non-Graphics queue lowering requires an initialized RHI";
                return false;
            }
            const QueueTopology runtime_topology = QueueTopology::FromRHI();
            for (const auto& batch : compiled_plan.queue_batches) {
                if (batch.queue.role == QueueRole::None ||
                    batch.queue != runtime_topology.Resolve(batch.queue.role)) {
                    compile_error =
                        "compiled queue topology does not match the initialized "
                        "RHI; construct the graph with QueueTopology::FromRHI()";
                    return false;
                }
            }
        }
        if (!compiled_plan.queue_batches.empty()) {
            const uint32_t first_native_queue =
                compiled_plan.queue_batches.front().queue.native_queue_id;
            active_async_multiqueue = std::any_of(
                compiled_plan.queue_batches.begin() + 1,
                compiled_plan.queue_batches.end(),
                [&](const CompiledQueueBatch& batch) {
                    return batch.queue.native_queue_id != first_native_queue;
                }
            );
        }
        if (active_async_multiqueue &&
            (configure_recording_source || publish_recording_batch)) {
            compile_error =
                "active multi-queue lowering requires the built-in immutable "
                "RHI graph handoff";
            return false;
        }
        if (active_uses_non_graphics_queue) {
            const bool has_caller_thread_gpu_pass = std::any_of(
                compiled_plan.queue_batches.begin(),
                compiled_plan.queue_batches.end(),
                [&](const CompiledQueueBatch& batch) {
                    return std::any_of(
                        batch.passes.begin(),
                        batch.passes.end(),
                        [&](PassHandle pass) {
                            const auto execution =
                                passes[pass.index].execution_class;
                            return execution != PassExecutionClass::SerialRecord &&
                                   execution !=
                                       PassExecutionClass::ParallelRecordEligible;
                        }
                    );
                }
            );
            if (has_caller_thread_gpu_pass) {
                compile_error =
                    "active non-Graphics queue lowering requires every GPU pass "
                    "to use the managed recording handoff";
                return false;
            }
        }

        auto append_instruction =
            [&](const RenderGraphLowering::LoweredInstruction& instruction,
                std::vector<BarrierCreateInfo>&                 destination,
                std::string_view                                placement) {
                BarrierCreateInfo barrier{};
                std::string       materialization_error{};
                if (!MaterializeInstruction(instruction, barrier, materialization_error)) {
                    compile_error =
                        "RenderGraph active lowering " + std::string(placement) +
                        " instruction " + std::to_string(instruction.correlation_id) +
                        " failed: " + materialization_error;
                    return false;
                }
                destination.emplace_back(std::move(barrier));
                return true;
            };

        for (const auto& instruction : lowered_plan.prologue) {
            if (!IsValidPass(instruction.dst_pass) ||
                !append_instruction(
                    instruction,
                    active_pass_states[instruction.dst_pass.index].before,
                    "prologue"
                )) {
                if (compile_error.empty()) {
                    compile_error =
                        "RenderGraph active lowering prologue has no valid destination pass";
                }
                return false;
            }
        }
        for (const auto& pass_instructions : lowered_plan.passes) {
            if (!IsValidPass(pass_instructions.pass)) {
                compile_error = "RenderGraph active lowering references an invalid pass";
                return false;
            }
            auto& materialized = active_pass_states[pass_instructions.pass.index];
            materialized.keepalive = pass_instructions.keepalive;
            for (const auto& instruction : pass_instructions.before) {
                if (!append_instruction(instruction, materialized.before, "before-pass")) {
                    return false;
                }
            }
            for (const auto& instruction : pass_instructions.after) {
                if (!append_instruction(instruction, materialized.after, "after-pass")) {
                    return false;
                }
            }
            if (active_async_multiqueue) {
                materialized.async_queue_scope = graph_id;
            }
        }

        try {
            for (const auto& sync : lowered_plan.queue_syncs) {
                FenceRef fence = RenderDevice::Get().CreateFence();
                if (!fence.IsValid()) {
                    compile_error =
                        "RHI returned no fence for active multi-queue sync " +
                        std::to_string(sync.correlation_id);
                    return false;
                }
                active_pass_states[sync.signal_pass.index]
                    .signal_fences.emplace_back(
                        RHIRecordingFencePoint{.fence = fence, .value = 1}
                    );
                active_pass_states[sync.wait_pass.index]
                    .wait_fences.emplace_back(
                        RHIRecordingFencePoint{.fence = std::move(fence), .value = 1}
                    );
            }
        } catch (const std::exception& exception) {
            compile_error =
                std::string("failed to create active multi-queue synchronization: ") +
                exception.what();
            return false;
        } catch (...) {
            compile_error =
                "failed to create active multi-queue synchronization";
            return false;
        }

        for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
            const auto& state = active_pass_states[pass_index];
            if (!state.RequiresCompletionLifetime()) {
                continue;
            }
            if (state.keepalive.empty()) {
                compile_error =
                    "RenderGraph active lowering produced physical commands without keepalive "
                    "ownership for pass '" +
                    passes[pass_index].name + "'";
                return false;
            }
            const auto execution = passes[pass_index].execution_class;
            if (execution == PassExecutionClass::CpuPrepare ||
                execution == PassExecutionClass::ExternalControl) {
                compile_error =
                    "RenderGraph active lowering cannot materialize physical state for pass '" +
                    passes[pass_index].name + "' on its execution class";
                return false;
            }
            if (execution == PassExecutionClass::MainThread) {
                active_has_physical_main_thread = true;
                if (active_recording.main_thread_command_list == nullptr) {
                    compile_error =
                        "RenderGraph active lowering requires a caller-owned CommandList for "
                        "main-thread pass '" +
                        passes[pass_index].name + "'";
                    return false;
                }
                if (active_recording.main_thread_command_list->GetQueueType() !=
                    EQueueType::Graphics) {
                    compile_error =
                        "RenderGraph active lowering requires a Graphics caller-owned CommandList";
                    return false;
                }
                if (!active_recording.main_thread_command_list->IsEmpty()) {
                    compile_error =
                        "RenderGraph active lowering requires an isolated empty caller-owned "
                        "CommandList";
                    return false;
                }
            }
        }
        if (active_has_physical_main_thread && after_main_thread_pass) {
            compile_error =
                "RenderGraph active lowering requires caller-owned MainThread "
                "commands to remain unsealed until ExecuteRecording returns";
            return false;
        }

        const bool has_any_managed_record_pass = std::any_of(
            compiled_plan.recording_batches.begin(),
            compiled_plan.recording_batches.end(),
            [](const CompiledRecordingBatch& batch) {
                return batch.execution == PassExecutionClass::SerialRecord ||
                       batch.execution ==
                           PassExecutionClass::ParallelRecordEligible;
            }
        );
        const bool has_any_caller_thread_pass = std::any_of(
            compiled_plan.recording_batches.begin(),
            compiled_plan.recording_batches.end(),
            [](const CompiledRecordingBatch& batch) {
                return batch.execution == PassExecutionClass::MainThread ||
                       batch.execution == PassExecutionClass::CpuPrepare ||
                       batch.execution == PassExecutionClass::ExternalControl;
            }
        );
        if (has_any_caller_thread_pass && has_any_managed_record_pass) {
            compile_error =
                "RenderGraph active lowering does not yet support mixing "
                "caller-thread and managed record passes in one transaction";
            return false;
        }
        if (active_has_physical_main_thread) {
            const bool has_nonphysical_or_nonmain_batch = std::any_of(
                compiled_plan.recording_batches.begin(),
                compiled_plan.recording_batches.end(),
                [&](const CompiledRecordingBatch& batch) {
                    return batch.execution != PassExecutionClass::MainThread ||
                           batch.passes.size() != 1 ||
                           !active_pass_states[batch.passes.front().index]
                                .RequiresCompletionLifetime();
                }
            );
            if (has_nonphysical_or_nonmain_batch) {
                compile_error =
                    "RenderGraph active MainThread lowering currently requires "
                    "an isolated graph of physical MainThread passes";
                return false;
            }
        }
    }

    struct RecordingJob {
        PassHandle          pass{};
        std::string         pass_name{};
        RecordCallback      record{};
        SharedPtr<CommandList> command_list{};
        RHIRecordingGateRef completion{};
        ERHITranslateExecutionClass translate_execution_class{
            ERHITranslateExecutionClass::Parallel
        };
        std::vector<BarrierCreateInfo>                    before{};
        std::vector<BarrierCreateInfo>                    after{};
        std::vector<RenderGraphLowering::PhysicalBinding> keepalive{};
    };
    struct RecordingError {
        std::mutex  mutex{};
        std::string message{};
    };
    struct RecordingLifetime {
        std::vector<RenderGraphLowering::PhysicalBinding> keepalive{};
        RecordCallback                                    record{};
        ExecuteCallback                                   execute{};
    };
    struct PendingGateGuard {
        const Array<RHIRecordingGateRef>* gates{nullptr};
        bool                              armed{true};

        ~PendingGateGuard() {
            if (!armed || gates == nullptr) {
                return;
            }
            for (const auto& gate : *gates) {
                if (gate && gate->Status() == ERHIRecordingStatus::Pending) {
                    gate->Fail();
                }
            }
        }

        void Disarm() noexcept {
            armed = false;
        }
    };
    struct ProducerGateGuard {
        RHIRecordingGateRef gate{};
        bool                armed{true};

        ~ProducerGateGuard() {
            if (armed && gate &&
                gate->Status() == ERHIRecordingStatus::Pending) {
                gate->Fail();
            }
        }

        void Disarm() noexcept {
            armed = false;
        }
    };
    struct ManagedRecordingOwner {
        // Members are destroyed in reverse declaration order: release the
        // lease before dropping the strong CommandList owner.
        SharedPtr<CommandList>                    command_list{};
        CommandList::ManagedRecordingLease        lease{};
    };
    struct ActiveMainCommandStreamGuard {
        CommandList* command_list{nullptr};
        std::optional<CommandList::ManagedRecordingLease> lease{};
        bool armed{false};

        void Abort() noexcept {
            if (!armed) {
                return;
            }
            lease.reset();
            if (command_list != nullptr) {
                try {
                    auto cleanup_callbacks =
                        command_list->DrainOrdinaryCallbacksForRejection();
                    for (auto& callback : cleanup_callbacks) {
                        if (!callback) {
                            continue;
                        }
                        try {
                            callback();
                        } catch (const std::exception& exception) {
                            LOG_ERROR(
                                "[RenderGraph] rejected MainThread cleanup "
                                "callback threw: {}",
                                exception.what()
                            );
                        } catch (...) {
                            LOG_ERROR(
                                "[RenderGraph] rejected MainThread cleanup "
                                "callback threw"
                            );
                        }
                    }
                } catch (const std::exception& exception) {
                    LOG_ERROR(
                        "[RenderGraph] failed to drain rejected MainThread "
                        "commands: {}",
                        exception.what()
                    );
                } catch (...) {
                    LOG_ERROR(
                        "[RenderGraph] failed to drain rejected MainThread commands"
                    );
                }
            }
            armed = false;
        }

        void Commit() noexcept {
            lease.reset();
            armed = false;
        }

        ~ActiveMainCommandStreamGuard() {
            Abort();
        }
    };
    struct ActiveRecordingTransactionGuard {
        RHIRecordingGateRef* commit{nullptr};
        Array<ManagedRecordingOwner>* owners{nullptr};
        bool armed{false};

        void ReleaseLeases() noexcept {
            if (owners == nullptr) {
                return;
            }
            for (auto& owner : *owners) {
                owner.lease.Release();
            }
            owners->clear();
        }

        ~ActiveRecordingTransactionGuard() {
            if (!armed) {
                return;
            }
            // Release the frontend mutation lock before failing the commit
            // gate. The RHI handoff may wake immediately and drain rejected
            // sources on its executor thread.
            ReleaseLeases();
            if (commit != nullptr && *commit) {
                (*commit)->Fail();
            }
        }

        bool Commit() noexcept {
            if (!armed || commit == nullptr || !*commit) {
                return true;
            }
            // A successful commit is the only point at which the RHI worker
            // may seal graph-managed CommandLists.
            ReleaseLeases();
            if (!(*commit)->Signal()) {
                return false;
            }
            armed = false;
            return true;
        }
    };

    RHIRecordingGateRef active_recording_commit{};
    Array<ManagedRecordingOwner> active_recording_owners{};
    try {
        if (active_recording.enabled) {
            active_recording_commit = RHIRecordingGate::Create();
        }
    } catch (const std::exception& exception) {
        compile_error =
            std::string("failed to create active recording transaction: ") +
            exception.what();
        return false;
    } catch (...) {
        compile_error = "failed to create active recording transaction";
        return false;
    }
    ActiveRecordingTransactionGuard active_transaction_guard{
        .commit = &active_recording_commit,
        .owners = &active_recording_owners,
        .armed  = active_recording.enabled,
    };
    ActiveMainCommandStreamGuard active_main_stream_guard{
        .command_list =
            active_has_physical_main_thread ?
                active_recording.main_thread_command_list :
                nullptr,
    };
    if (active_has_physical_main_thread) {
        try {
            active_main_stream_guard.lease.emplace(
                active_recording.main_thread_command_list
                    ->AcquireManagedRecordingLease()
            );
            active_main_stream_guard.armed = true;
        } catch (const std::exception& exception) {
            compile_error =
                std::string("failed to acquire active MainThread recording lease: ") +
                exception.what();
            return false;
        } catch (...) {
            compile_error =
                "failed to acquire active MainThread recording lease";
            return false;
        }
    }
    executed = true;

    auto make_pass_info = [&](PassHandle handle) {
        const auto& pass = passes[handle.index];
        return ExecutedPassInfo{
            .handle          = handle,
            .name            = pass.name,
            .domain          = pass.domain,
            .side_effect     = pass.side_effect,
            .execution_class = pass.execution_class,
            .translate_execution_class =
                pass.translate_execution_class,
        };
    };

    auto store_error = [](const std::shared_ptr<RecordingError>& error, std::string message) {
        std::lock_guard lock(error->mutex);
        if (error->message.empty()) {
            error->message = std::move(message);
        }
    };
    auto attach_recording_lifetime =
        [](CommandList&                                      command_list,
           std::vector<RenderGraphLowering::PhysicalBinding> keepalive,
           RecordCallback                                    record,
           ExecuteCallback                                   execute = {}) {
            auto lifetime = std::make_shared<RecordingLifetime>(
                RecordingLifetime{
                    .keepalive = std::move(keepalive),
                    .record    = std::move(record),
                    .execute   = std::move(execute),
                }
            );
            // Ordinary callbacks run before success callbacks. Holding the
            // same token in both tails keeps raw command/callback captures
            // alive through every user callback on success, failure, or
            // cancellation.
            command_list.AddCallback([lifetime] {});
            command_list.AddSuccessCallback([lifetime] {});
        };

    auto has_cpu_recording_dependency = [&](PassHandle candidate,
                                            size_t     group_begin,
                                            size_t     group_end) {
        for (const auto& edge : compiled_plan.edges) {
            if (edge.dst != candidate) {
                continue;
            }

            const bool source_is_in_group = std::any_of(
                compiled_plan.recording_batches.begin() + group_begin,
                compiled_plan.recording_batches.begin() + group_end,
                [&](const CompiledRecordingBatch& batch) {
                    return batch.passes.size() == 1 && batch.passes.front() == edge.src;
                }
            );
            if (!source_is_in_group) {
                continue;
            }

            for (const auto& reason : edge.reasons) {
                if (reason.kind == EdgeReasonKind::Explicit) {
                    return true;
                }
                if (IsValidResource(reason.resource) &&
                    resources[reason.resource.index].kind == ResourceKind::Token) {
                    return true;
                }
            }
        }
        return false;
    };

    size_t batch_index = 0;
    while (batch_index < compiled_plan.recording_batches.size()) {
        const auto& first_batch = compiled_plan.recording_batches[batch_index];
        if (first_batch.passes.size() != 1) {
            compile_error = "recording execution currently requires one pass per compiled batch";
            return false;
        }

        const PassHandle first_handle = first_batch.passes.front();
        auto&            first_pass   = passes[first_handle.index];
        if (first_batch.execution == PassExecutionClass::MainThread ||
            first_batch.execution == PassExecutionClass::CpuPrepare ||
            first_batch.execution == PassExecutionClass::ExternalControl) {
            if (!first_pass.execute || first_pass.record) {
                compile_error = "compiled caller-thread pass has an invalid callback shape: " +
                                first_pass.name;
                return false;
            }

            const auto& pass_state = active_pass_states[first_handle.index];
            bool        main_thread_lifetime_attached = false;
            const bool  active_physical_main_thread =
                active_recording.enabled &&
                pass_state.RequiresCompletionLifetime();
            auto attach_main_thread_lifetime = [&] {
                if (!active_physical_main_thread ||
                    main_thread_lifetime_attached) {
                    return;
                }
                attach_recording_lifetime(
                    *active_recording.main_thread_command_list,
                    pass_state.keepalive,
                    {},
                    first_pass.execute
                );
                main_thread_lifetime_attached = true;
            };
            auto reject_main_thread =
                [&](std::string message) {
                    if (active_has_physical_main_thread) {
                        try {
                            attach_main_thread_lifetime();
                        } catch (const std::exception& exception) {
                            message +=
                                "; failed to retain MainThread recording lifetime: ";
                            message += exception.what();
                        } catch (...) {
                            message +=
                                "; failed to retain MainThread recording lifetime";
                        }

                        active_main_stream_guard.Abort();
                    }
                    compile_error = std::move(message);
                    return false;
                };

            try {
                if (active_physical_main_thread) {
                    auto& command_list =
                        *active_recording.main_thread_command_list;
                    command_list.SetResourceStateOwnership(
                        ERHIResourceStateOwnership::Explicit
                    );
                    if (!pass_state.before.empty()) {
                        command_list.Barriers(
                            std::span<const BarrierCreateInfo>(
                                pass_state.before.data(),
                                pass_state.before.size()
                            )
                        );
                    }
                }

                const uint64 main_thread_seal_generation =
                    active_physical_main_thread ?
                        active_recording.main_thread_command_list
                            ->GetSealGeneration() :
                        0;
                first_pass.execute();

                if (active_physical_main_thread &&
                    active_recording.main_thread_command_list
                            ->GetSealGeneration() !=
                        main_thread_seal_generation) {
                    throw std::logic_error(
                        "active MainThread callback sealed its managed CommandList"
                    );
                }
                if (active_physical_main_thread &&
                    !active_recording.main_thread_command_list
                         ->HasExplicitResourceStateOwnership()) {
                    throw std::logic_error(
                        "active MainThread callback changed explicit state ownership"
                    );
                }
                if (active_physical_main_thread && !pass_state.after.empty()) {
                    active_recording.main_thread_command_list->Barriers(
                        std::span<const BarrierCreateInfo>(
                            pass_state.after.data(), pass_state.after.size()
                        )
                    );
                }
                if (active_physical_main_thread) {
                    attach_main_thread_lifetime();
                }
            } catch (const std::exception& exception) {
                return reject_main_thread(
                    "main-thread pass '" + first_pass.name +
                    "' failed: " + exception.what()
                );
            } catch (...) {
                return reject_main_thread(
                    "main-thread pass '" + first_pass.name + "' failed"
                );
            }
            if (first_batch.execution == PassExecutionClass::MainThread &&
                after_main_thread_pass) {
                after_main_thread_pass(make_pass_info(first_handle));
            }
            ++batch_index;
            continue;
        }

        size_t group_end = batch_index + 1;
        if (first_batch.execution == PassExecutionClass::ParallelRecordEligible) {
            while (group_end < compiled_plan.recording_batches.size()) {
                const auto& candidate = compiled_plan.recording_batches[group_end];
                if (candidate.execution != PassExecutionClass::ParallelRecordEligible ||
                    candidate.passes.size() != 1 ||
                    has_cpu_recording_dependency(
                        candidate.passes.front(), batch_index, group_end
                    )) {
                    break;
                }
                ++group_end;
            }
        }

        Array<RecordingJob>        jobs{};
        Array<RHIRecordingSource>  sources{};
        Array<RHIRecordingGateRef> group_gates{};
        jobs.reserve(group_end - batch_index);
        sources.reserve(group_end - batch_index);
        group_gates.reserve(group_end - batch_index);

        for (size_t index = batch_index; index < group_end; ++index) {
            const auto& batch = compiled_plan.recording_batches[index];
            const auto  handle = batch.passes.front();
            auto&       pass   = passes[handle.index];
            const auto  queue  = ToRHIQueue(batch.queue.role);
            if (!pass.record || pass.execute || queue == EQueueType::Ignore) {
                compile_error = "compiled recording pass has an invalid callback or queue: " +
                                pass.name;
                return false;
            }

            RecordingJob job{
                .pass         = handle,
                .pass_name    = pass.name,
                .record       = pass.record,
                .command_list = MakeShared<CommandList>(queue),
                .completion   = RHIRecordingGate::Create(),
                .translate_execution_class =
                    batch.translate_execution_class,
            };
            job.command_list->SetTranslateExecutionClass(
                job.translate_execution_class
            );
            if (active_recording.enabled) {
                const auto& pass_state = active_pass_states[handle.index];
                job.before             = pass_state.before;
                job.after              = pass_state.after;
                job.keepalive          = pass_state.keepalive;
            }
            RHIRecordingSource source{
                .command_list = job.command_list,
                .completion   = job.completion,
                .commit       = active_recording_commit,
            };
            if (batch.translate_execution_class !=
                ERHITranslateExecutionClass::Parallel) {
                source.submit_metadata.translate_execution_class =
                    batch.translate_execution_class;
            }
            if (active_recording.enabled) {
                const auto& pass_state = active_pass_states[handle.index];
                source.submit_metadata.wait_fences =
                    pass_state.wait_fences;
                source.submit_metadata.signal_fences =
                    pass_state.signal_fences;
                source.submit_metadata.async_queue_scope =
                    pass_state.async_queue_scope;
            }
            if (active_recording.enabled) {
                try {
                    auto lease =
                        job.command_list->AcquireManagedRecordingLease();
                    active_recording_owners.emplace_back(
                        ManagedRecordingOwner{
                            .command_list = job.command_list,
                            .lease        = std::move(lease),
                        }
                    );
                } catch (const std::exception& exception) {
                    compile_error =
                        "failed to acquire managed recording lease for pass '" +
                        pass.name + "': " + exception.what();
                    return false;
                } catch (...) {
                    compile_error =
                        "failed to acquire managed recording lease for pass '" +
                        pass.name + "'";
                    return false;
                }
            }
            if (configure_recording_source) {
                try {
                    // Configuration may run while an earlier transaction
                    // group is already waiting in the handoff FIFO. Treat it
                    // as an admission owner so Sync/Flush cannot self-join
                    // behind the pending graph commit gate.
                    RHIThreadRoleScope configuration_owner(
                        ERHIThreadRole::RecordWorker
                    );
                    configure_recording_source(make_pass_info(handle), source);
                } catch (const std::exception& exception) {
                    compile_error = "recording source configuration failed for pass '" + pass.name +
                                    "': " + exception.what();
                    return false;
                } catch (...) {
                    compile_error = "recording source configuration failed for pass '" + pass.name + "'";
                    return false;
                }
                if (source.command_list != job.command_list ||
                    source.completion != job.completion ||
                    source.commit != active_recording_commit) {
                    compile_error = "recording source configuration changed ownership for pass '" +
                                    pass.name + "'";
                    return false;
                }
                if (batch.translate_execution_class ==
                        ERHITranslateExecutionClass::SerialControl &&
                    source.submit_metadata.translate_execution_class !=
                        ERHITranslateExecutionClass::SerialControl) {
                    compile_error =
                        "recording source configuration weakened the declared "
                        "SerialControl translation policy for pass '" +
                        pass.name + "'";
                    return false;
                }
                if (job.completion->Status() != ERHIRecordingStatus::Pending) {
                    compile_error =
                        "recording source configuration completed the producer gate for pass '" +
                        pass.name + "'; the gate must remain pending until recording finishes";
                    return false;
                }
                if (active_recording_commit &&
                    active_recording_commit->Status() !=
                        ERHIRecordingStatus::Pending) {
                    compile_error =
                        "recording source configuration completed the active "
                        "transaction gate for pass '" +
                        pass.name + "'";
                    return false;
                }
                if (!source.command_list->IsEmpty() ||
                    source.command_list->HasExplicitResourceStateOwnership() ||
                    source.command_list->GetTranslateExecutionClass() !=
                        batch.translate_execution_class) {
                    compile_error =
                        "recording source configuration may only change submit metadata "
                        "for pass '" +
                        pass.name + "'";
                    return false;
                }
            }
            group_gates.emplace_back(job.completion);
            jobs.emplace_back(std::move(job));
            sources.emplace_back(std::move(source));
        }

        PendingGateGuard pending_gate_guard{.gates = &group_gates};

        const auto error = std::make_shared<RecordingError>();
        auto run_job =
            [error, store_error, attach_recording_lifetime](RecordingJob job) mutable {
            RHIThreadRoleScope record_owner(ERHIThreadRole::RecordWorker);
            ProducerGateGuard producer_gate_guard{.gate = job.completion};
            bool lifetime_attached = false;
            auto attach_lifetime = [&] {
                if (lifetime_attached) {
                    return;
                }
                attach_recording_lifetime(
                    *job.command_list,
                    std::move(job.keepalive),
                    std::move(job.record)
                );
                lifetime_attached = true;
            };
            auto fail_job =
                [&](std::string_view detail, bool has_detail) noexcept {
                    try {
                        std::string message =
                            "record pass '" + job.pass_name + "' failed";
                        if (has_detail) {
                            message += ": ";
                            message += detail;
                        }
                        store_error(error, std::move(message));
                    } catch (...) {
                    }
                    try {
                        attach_lifetime();
                    } catch (...) {
                        try {
                            store_error(
                                error,
                                "record pass '" + job.pass_name +
                                    "' failed while retaining its recording "
                                    "lifetime"
                            );
                        } catch (...) {
                        }
                    }
                    job.completion->Fail();
                    producer_gate_guard.Disarm();
                };
            try {
                const bool owns_explicit_state = !job.keepalive.empty();
                if (owns_explicit_state || !job.before.empty() || !job.after.empty()) {
                    job.command_list->SetResourceStateOwnership(
                        ERHIResourceStateOwnership::Explicit
                    );
                }
                if (!job.before.empty()) {
                    job.command_list->Barriers(
                        std::span<const BarrierCreateInfo>(
                            job.before.data(), job.before.size()
                        )
                    );
                }
                const uint64 seal_generation =
                    job.command_list->GetSealGeneration();
                job.record(*job.command_list);
                if (job.command_list->GetSealGeneration() != seal_generation) {
                    throw std::logic_error(
                        "record callback sealed its managed CommandList"
                    );
                }
                if (owns_explicit_state &&
                    !job.command_list->HasExplicitResourceStateOwnership()) {
                    throw std::logic_error(
                        "record callback changed explicit state ownership"
                    );
                }
                if (job.command_list->GetTranslateExecutionClass() !=
                    job.translate_execution_class) {
                    throw std::logic_error(
                        "record callback changed its managed translation class"
                    );
                }
                if (!job.after.empty()) {
                    job.command_list->Barriers(
                        std::span<const BarrierCreateInfo>(
                            job.after.data(), job.after.size()
                        )
                    );
                }
                attach_lifetime();
                if (!job.completion->Signal()) {
                    throw std::logic_error(
                        "recording completion gate was completed before its producer"
                    );
                }
                producer_gate_guard.Disarm();
            } catch (const std::exception& exception) {
                fail_job(exception.what(), true);
            } catch (...) {
                fail_job({}, false);
            }
        };

        // Register the unfinished sources before dispatch, matching the
        // dev_parallel/UE-style producer-consumer handoff. RHI owns stable
        // source order; worker completion order is irrelevant.
        Array<uint64> publication_generations{};
        publication_generations.reserve(jobs.size());
        for (const auto& job : jobs) {
            publication_generations.emplace_back(
                job.command_list->GetSealGeneration()
            );
        }
        try {
            // Publication is an admission-only phase. The producers have not
            // started yet, so a custom publisher must not block on their gates
            // or on RHI work routed behind this pending handoff.
            RHIThreadRoleScope publication_owner(
                ERHIThreadRole::RecordWorker
            );
            if (publish_recording_batch) {
                publish_recording_batch(std::move(sources));
            } else {
                RHIExecutor::Get().SubmitRecording(std::move(sources), ERHIExecSubmitFlags::None);
            }
        } catch (const std::exception& exception) {
            compile_error = std::string("failed to publish recording batch: ") + exception.what();
            return false;
        } catch (...) {
            compile_error = "failed to publish recording batch";
            return false;
        }
        for (size_t index = 0; index < jobs.size(); ++index) {
            const auto& job = jobs[index];
            if (job.completion->Status() != ERHIRecordingStatus::Pending ||
                job.command_list->GetSealGeneration() !=
                    publication_generations[index] ||
                !job.command_list->IsEmpty() ||
                job.command_list->HasExplicitResourceStateOwnership() ||
                job.command_list->GetTranslateExecutionClass() !=
                    job.translate_execution_class ||
                (active_recording_commit &&
                 active_recording_commit->Status() !=
                     ERHIRecordingStatus::Pending)) {
                compile_error =
                    "recording batch publisher mutated pending source state for pass '" +
                    job.pass_name + "'";
                return false;
            }
        }

        const bool task_graph_available = TaskGraph::IsInitialized();
        const bool task_graph_dispatch =
            first_batch.execution == PassExecutionClass::ParallelRecordEligible &&
            parallel_recording_enabled && task_graph_available;
        if (first_batch.execution == PassExecutionClass::SerialRecord ||
            !parallel_recording_enabled || !task_graph_available) {
            for (auto& job : jobs) {
                run_job(std::move(job));
            }
        } else {
            GraphEventArray record_events{};
            record_events.reserve(jobs.size());
            size_t dispatched_count = 0;
            try {
                for (; dispatched_count < jobs.size(); ++dispatched_count) {
                    record_events.emplace_back(LambdaTask::Dispatch(
                        [job = std::move(jobs[dispatched_count]), run_job]() mutable {
                            run_job(std::move(job));
                        }
                    ));
                }
            } catch (const std::exception& exception) {
                store_error(error, std::string("failed to dispatch recording task: ") + exception.what());
                for (size_t index = dispatched_count; index < jobs.size(); ++index) {
                    group_gates[index]->Fail();
                }
            } catch (...) {
                store_error(error, "failed to dispatch recording task");
                for (size_t index = dispatched_count; index < jobs.size(); ++index) {
                    group_gates[index]->Fail();
                }
            }
            if (!record_events.empty()) {
                // Do not use TaskGraph::WaitUntilTasksComplete here. On a named Render Thread that
                // wait is allowed to pump the same named queue, which can execute the next
                // RenderFrame re-entrantly while this frame still owns mutable renderer state.
                // The producer gate is the handoff contract: Signal/Fail happens only after the
                // worker has stopped mutating its CommandList, and its condition variable gives the
                // caller the required happens-before edge without processing unrelated RT work.
                // Keep record_events alive until every producer is terminal so the task/event
                // ownership also remains explicit across the blocking join.
                for (const auto& gate : group_gates) {
                    (void)gate->Wait();
                }
            }
        }

        bool group_succeeded = true;
        for (const auto& gate : group_gates) {
            group_succeeded =
                gate->Wait() == ERHIRecordingStatus::Succeeded && group_succeeded;
        }
        if (!group_succeeded) {
            std::lock_guard lock(error->mutex);
            compile_error = error->message.empty() ? "recording batch failed" : error->message;
            return false;
        }
        if (task_graph_dispatch && group_end - batch_index > 1) {
            std::ostringstream pass_list;
            for (size_t index = batch_index; index < group_end; ++index) {
                if (index != batch_index) {
                    pass_list << ',';
                }
                const auto pass_handle = compiled_plan.recording_batches[index].passes.front();
                pass_list << passes[pass_handle.index].name;
            }

            const std::string pass_names = pass_list.str();
            const std::string dispatch_key = name + '\n' + pass_names;
            static std::mutex                      logged_dispatch_mutex;
            static std::unordered_set<std::string> logged_dispatches;
            bool                                   first_dispatch = false;
            {
                std::lock_guard lock(logged_dispatch_mutex);
                first_dispatch = logged_dispatches.emplace(dispatch_key).second;
            }
            if (first_dispatch) {
                LOG_INFO(
                    "[RenderGraph][ParallelRecordDispatch] graph={} passes=[{}] "
                    "dispatch=task-graph worker_jobs={} completed=true",
                    name,
                    pass_names,
                    group_end - batch_index
                );
            }
        }
        pending_gate_guard.Disarm();
        batch_index = group_end;
    }
    if (!active_transaction_guard.Commit()) {
        compile_error =
            "active recording transaction gate completed before graph commit";
        return false;
    }
    active_main_stream_guard.Commit();
    return true;
}

std::string RenderGraph::Dump() const {
    std::ostringstream stream;
    stream << "graph='" << name
           << "' frontend=typed-rdg mode=compiled barrier_plan=explicit "
              "sync_plan=queue-dag external_endpoints=unbound state_plan="
           << (compiled_plan.state_plan_complete ? "complete" : "incomplete") << " compiled="
           << (compiled ? "true" : "false") << " executed=" << (executed ? "true" : "false")
           << " passes=" << passes.size() << " resources=" << resources.size();
    if (!compile_error.empty()) {
        stream << " error='" << compile_error << "'";
    }
    stream << '\n';

    stream << "queue_topology:";
    for (const QueueRole role : {QueueRole::Graphics, QueueRole::Compute, QueueRole::Copy}) {
        const auto binding = queue_topology.Resolve(role);
        stream << ' ' << ToString(role) << "=native" << binding.native_queue_id << "/family"
               << binding.family_id;
    }
    stream << '\n';

    stream << "passes:\n";
    const bool has_execution_order = compiled_plan.execution_order.size() == passes.size();
    for (uint32_t item_index = 0; item_index < passes.size(); ++item_index) {
        const uint32_t pass_index =
            has_execution_order ? compiled_plan.execution_order[item_index].index : item_index;
        const auto& pass = passes[pass_index];
        stream << "  [" << item_index << "] " << pass.name << " declared=" << pass_index
               << " scheduled=" << (has_execution_order ? "true" : "false")
               << " queue=" << ToString(pass.domain.queue)
               << " pipeline=" << ToString(pass.domain.pipeline)
               << " cpu=" << ToString(pass.execution_class)
               << " translate=" << ToString(pass.translate_execution_class)
               << " workload=" << pass.workload
               << " side_effect=" << (pass.side_effect ? "true" : "false") << " accesses=[";
        for (uint32_t access_index = 0; access_index < pass.accesses.size(); ++access_index) {
            const auto& access = pass.accesses[access_index];
            if (access_index != 0) {
                stream << ", ";
            }
            stream << ToString(access.mode) << ':';
            if (IsValidResource(access.resource)) {
                stream << resources[access.resource.index].name;
            } else {
                stream << "invalid";
            }
            stream << '(';
            if (access.typed) {
                AppendRange(stream, access.range);
            } else {
                stream << "legacy=" << access.legacy_range;
            }
            stream << " state=";
            AppendState(stream, access.state);
            stream << ')';
        }
        stream << "] references=[";
        for (uint32_t reference_index = 0; reference_index < pass.references.size();
             ++reference_index) {
            if (reference_index != 0) {
                stream << ", ";
            }
            const auto reference = pass.references[reference_index];
            stream << (IsValidResource(reference) ? resources[reference.index].name : "invalid");
        }
        stream << "]\n";
    }

    stream << "resources:\n";
    for (uint32_t index = 0; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        stream << "  [" << index << "] " << resource.name << " kind=" << ToString(resource.kind)
               << " lifetime=" << (resource.imported ? "imported" : "transient");
        if (resource.kind == ResourceKind::Texture && resource.typed_desc) {
            stream << " desc=[mips=" << resource.texture_desc.mip_count
                   << " layers=" << resource.texture_desc.layer_count << " aspects=";
            AppendTextureAspects(stream, resource.texture_desc.aspects);
            stream << " sharing="
                   << (resource.texture_desc.sharing_mode == TextureDesc::SharingMode::Exclusive ?
                           "exclusive" :
                           "concurrent")
                   << ']';
        } else if (resource.kind == ResourceKind::Buffer && resource.typed_desc) {
            stream << " desc=[bytes=" << resource.buffer_desc.byte_size << " sharing="
                   << (resource.buffer_desc.sharing_mode == TextureDesc::SharingMode::Exclusive ?
                           "exclusive" :
                           "concurrent")
                   << ']';
        }
        stream << " first=";
        if (resource.first_use == PassHandle::InvalidIndex) {
            stream << "unused";
        } else {
            stream << resource.first_use;
        }
        stream << " last=";
        if (resource.last_use == PassHandle::InvalidIndex) {
            stream << "unused";
        } else {
            stream << resource.last_use;
        }
        uint32_t version_count = 0;
        uint32_t transient_slot = PassHandle::InvalidIndex;
        if (index < compiled_plan.resources.size()) {
            version_count  = compiled_plan.resources[index].version_count;
            transient_slot = compiled_plan.resources[index].transient_slot;
        }
        stream << " versions=" << version_count
               << " exported=" << (resource.exported ? "true" : "false")
               << " aliases=[";
        auto aliases = resource.aliases;
        std::sort(aliases.begin(), aliases.end());
        for (uint32_t alias_index = 0; alias_index < aliases.size(); ++alias_index) {
            if (alias_index != 0) {
                stream << ", ";
            }
            stream << aliases[alias_index];
        }
        stream << "] slot=";
        if (transient_slot == PassHandle::InvalidIndex) {
            stream << "none";
        } else {
            stream << transient_slot;
        }
        stream << " initial_states=" << resource.initial_states.size()
               << " final_states=" << resource.final_states.size() << "\n";
    }

    stream << "compiled_accesses:\n";
    for (const auto& access : compiled_plan.accesses) {
        stream << "  " << passes[access.pass.index].name << ' ' << ToString(access.mode) << ':'
               << resources[access.resource.index].name << ' ';
        AppendRange(stream, access.range);
        stream << " state=";
        AppendState(stream, access.state);
        stream << " domain=" << ToString(access.domain.queue) << '/' << ToString(access.domain.pipeline);
        stream << " version=";
        AppendVersion(stream, access.input_version);
        stream << "->";
        AppendVersion(stream, access.output_version);
        stream << '\n';
    }

    stream << "edges:\n";
    for (const auto& edge : compiled_plan.edges) {
        stream << "  " << passes[edge.src.index].name << " -> " << passes[edge.dst.index].name
               << " reasons=[";
        for (uint32_t reason_index = 0; reason_index < edge.reasons.size(); ++reason_index) {
            const auto& reason = edge.reasons[reason_index];
            if (reason_index != 0) {
                stream << ", ";
            }
            stream << ToString(reason.kind);
            if (reason.resource.IsValid()) {
                stream << ':' << resources[reason.resource.index].name << '@';
                AppendRange(stream, reason.range);
                stream << ' ';
                AppendVersion(stream, reason.input_version);
                stream << "->";
                AppendVersion(stream, reason.output_version);
            }
        }
        stream << "]\n";
    }

    auto append_pass_endpoint = [&](PassHandle pass, bool import_boundary, bool export_boundary) {
        if (pass.IsValid() && pass.owner_id == graph_id && pass.index < passes.size()) {
            stream << passes[pass.index].name;
        } else if (import_boundary) {
            stream << "<import>";
        } else if (export_boundary) {
            stream << "<export>";
        } else {
            stream << "<initial>";
        }
    };

    stream << "barriers:\n";
    for (uint32_t barrier_index = 0; barrier_index < compiled_plan.barriers.size(); ++barrier_index) {
        const auto& barrier = compiled_plan.barriers[barrier_index];
        stream << "  [" << barrier_index << "] " << resources[barrier.resource.index].name << '@';
        AppendRange(stream, barrier.range);
        stream << ' ';
        append_pass_endpoint(barrier.src_pass, barrier.import_boundary, false);
        stream << '(' << ToString(barrier.src_domain.queue) << '/'
               << ToString(barrier.src_domain.pipeline) << ' ';
        AppendState(stream, barrier.before_state);
        stream << ' ' << ToString(barrier.before_access) << ") -> ";
        append_pass_endpoint(barrier.dst_pass, false, barrier.export_boundary);
        stream << '(' << ToString(barrier.dst_domain.queue) << '/'
               << ToString(barrier.dst_domain.pipeline) << ' ';
        AppendState(stream, barrier.after_state);
        stream << ' ' << ToString(barrier.after_access) << ") flags=[";
        bool wrote_flag = false;
        auto append_flag = [&](bool enabled, const char* flag) {
            if (!enabled) {
                return;
            }
            if (wrote_flag) {
                stream << ',';
            }
            stream << flag;
            wrote_flag = true;
        };
        append_flag(barrier.execution_dependency, "execution");
        append_flag(barrier.memory_dependency, "memory");
        append_flag(barrier.state_transition, "transition");
        append_flag(barrier.queue_dependency, "queue");
        append_flag(barrier.queue_ownership, "ownership");
        append_flag(barrier.discard_previous_contents, "discard");
        append_flag(barrier.import_boundary, "import");
        append_flag(barrier.export_boundary, "export");
        append_flag(barrier.source_state_unknown, "unknown-src");
        append_flag(barrier.transient_alias, "transient-alias");
        if (!wrote_flag) {
            stream << "none";
        }
        stream << "] sources=[";
        for (uint32_t source_index = 0; source_index < barrier.sources.size(); ++source_index) {
            if (source_index != 0) {
                stream << ',';
            }
            const auto& source = barrier.sources[source_index];
            append_pass_endpoint(source.pass, false, false);
            stream << '(' << ToString(source.domain.queue) << '/'
                   << ToString(source.domain.pipeline) << ' ';
            AppendState(stream, source.state);
            stream << ' ' << ToString(source.access) << ')';
        }
        stream << "]\n";
    }

    stream << "alias_boundaries:\n";
    for (const auto& alias : compiled_plan.alias_boundaries) {
        stream << "  slot=" << alias.transient_slot << ' '
               << resources[alias.predecessor_resource.index].name << " -> "
               << resources[alias.successor_resource.index].name << " passes="
               << passes[alias.primary_src_pass.index].name << "->"
               << passes[alias.dst_pass.index].name
               << " frontier=[";
        for (uint32_t index = 0; index < alias.source_frontier.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << passes[alias.source_frontier[index].index].name;
        }
        stream << "] barrier=" << alias.barrier_index << '\n';
    }

    stream << "barrier_placements:\n  prologue=[";
    for (uint32_t index = 0; index < compiled_plan.prologue_barriers.size(); ++index) {
        if (index != 0) {
            stream << ',';
        }
        stream << compiled_plan.prologue_barriers[index];
    }
    stream << "] epilogue=[";
    for (uint32_t index = 0; index < compiled_plan.epilogue_barriers.size(); ++index) {
        if (index != 0) {
            stream << ',';
        }
        stream << compiled_plan.epilogue_barriers[index];
    }
    stream << "]\n";

    stream << "queue_batches:\n";
    for (const auto& batch : compiled_plan.queue_batches) {
        stream << "  [" << batch.id << "] " << ToString(batch.queue.role) << " native="
               << batch.queue.native_queue_id << " family=" << batch.queue.family_id
               << " available=" << (batch.queue.available ? "true" : "false")
               << " external_control=" << (batch.external_control ? "true" : "false")
               << " passes=[";
        for (uint32_t pass_index = 0; pass_index < batch.passes.size(); ++pass_index) {
            if (pass_index != 0) {
                stream << ',';
            }
            stream << passes[batch.passes[pass_index].index].name;
        }
        stream << "] pre=[";
        for (uint32_t index = 0; index < batch.pre_barriers.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << batch.pre_barriers[index];
        }
        stream << "] post=[";
        for (uint32_t index = 0; index < batch.post_barriers.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << batch.post_barriers[index];
        }
        stream << "] waits=[";
        for (uint32_t index = 0; index < batch.wait_syncs.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << batch.wait_syncs[index];
        }
        stream << "] signals=[";
        for (uint32_t index = 0; index < batch.signal_syncs.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << batch.signal_syncs[index];
        }
        stream << "]\n";
    }

    stream << "queue_syncs:\n";
    for (const auto& sync : compiled_plan.queue_syncs) {
        stream << "  [" << sync.id << "] batch" << sync.signal_batch << " -> batch"
               << sync.wait_batch << " mode=" << (sync.gpu_wait_required ? "gpu" : "host-fifo")
               << " edges=[";
        for (uint32_t index = 0; index < sync.dependency_edges.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << sync.dependency_edges[index];
        }
        stream << "] barriers=[";
        for (uint32_t index = 0; index < sync.barriers.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << sync.barriers[index];
        }
        stream << "]\n";
    }

    stream << "pass_barriers:\n";
    for (const auto& placement : compiled_plan.pass_barriers) {
        stream << "  " << passes[placement.pass.index].name << " before=[";
        for (uint32_t index = 0; index < placement.before.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << placement.before[index];
        }
        stream << "]\n";
    }

    stream << "dependency_waves:\n";
    for (uint32_t wave_index = 0; wave_index < compiled_plan.dependency_waves.size(); ++wave_index) {
        stream << "  [" << wave_index << "] [";
        const auto& wave = compiled_plan.dependency_waves[wave_index];
        for (uint32_t pass_index = 0; pass_index < wave.passes.size(); ++pass_index) {
            if (pass_index != 0) {
                stream << ", ";
            }
            stream << passes[wave.passes[pass_index].index].name;
        }
        stream << "]\n";
    }

    stream << "recording_batches:\n";
    for (const auto& batch : compiled_plan.recording_batches) {
        stream << "  [" << batch.id << "] queue=" << ToString(batch.queue.role)
               << "/native" << batch.queue.native_queue_id << "/family" << batch.queue.family_id
               << " cpu=" << ToString(batch.execution)
               << " translate=" << ToString(batch.translate_execution_class)
               << " workload=" << batch.workload
               << " wave=";
        if (batch.dependency_wave == PassHandle::InvalidIndex) {
            stream << "none";
        } else {
            stream << batch.dependency_wave;
        }
        stream << " passes=[";
        for (uint32_t index = 0; index < batch.passes.size(); ++index) {
            if (index != 0) {
                stream << ", ";
            }
            stream << passes[batch.passes[index].index].name;
        }
        stream << "]\n";
    }
    return stream.str();
}

void RenderGraph::AddAccess(
    uint32_t         pass_index,
    ResourceHandle   resource,
    AccessMode       mode,
    ResourceRange    range,
    ResourceState    state,
    bool             typed,
    bool             explicit_state,
    std::string_view legacy_range
) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size()) {
        declaration_errors.emplace_back("access declaration has invalid pass index");
        return;
    }
    if (!IsValidResource(resource)) {
        declaration_errors.emplace_back(
            "pass '" + passes[pass_index].name + "' declared an invalid resource"
        );
        return;
    }
    if (range.kind != resources[resource.index].kind) {
        declaration_errors.emplace_back(
            "pass '" + passes[pass_index].name + "' used a typed handle with the wrong resource kind"
        );
        return;
    }
    if (state.kind != resources[resource.index].kind) {
        declaration_errors.emplace_back(
            "pass '" + passes[pass_index].name + "' used a state with the wrong resource kind"
        );
        return;
    }
    if (mode == AccessMode::None || mode == AccessMode::Unknown) {
        declaration_errors.emplace_back(
            "pass '" + passes[pass_index].name + "' declared an invalid access mode"
        );
        return;
    }
    passes[pass_index].accesses.push_back(
        AccessDeclaration{
            resource,
            mode,
            range,
            state,
            std::string(legacy_range),
            typed,
            explicit_state
        }
    );
}

void RenderGraph::AddStateDeclaration(
    ResourceHandle resource,
    ResourceRange  range,
    ResourceState  state,
    QueueRole      queue,
    AccessMode     boundary_access,
    bool           initial
) {
    if (!InvalidateCompile()) {
        return;
    }
    if (!IsValidResource(resource)) {
        declaration_errors.emplace_back("state declaration received an invalid resource handle");
        return;
    }
    auto& declaration = resources[resource.index];
    if (declaration.kind == ResourceKind::Token || range.kind != declaration.kind ||
        state.kind != declaration.kind) {
        declaration_errors.emplace_back(
            "state declaration has the wrong resource kind for '" + declaration.name + "'"
        );
        return;
    }
    if (initial && !declaration.imported) {
        declaration_errors.emplace_back(
            "only imported resources may declare an initial state: " + declaration.name
        );
        return;
    }
    StateDeclaration state_declaration{range, state, queue, boundary_access};
    auto&            target = initial ? declaration.initial_states : declaration.final_states;
    target.push_back(std::move(state_declaration));
}

void RenderGraph::AddDependency(uint32_t pass_index, PassHandle dependency) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size() || !IsValidPass(dependency)) {
        declaration_errors.emplace_back("explicit dependency contains an invalid pass handle");
        return;
    }
    if (dependency.index == pass_index) {
        declaration_errors.emplace_back("pass cannot depend on itself: " + passes[pass_index].name);
        return;
    }
    auto& dependencies = passes[pass_index].explicit_dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), dependency) == dependencies.end()) {
        dependencies.push_back(dependency);
    }
}

void RenderGraph::MarkSideEffect(uint32_t pass_index) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size()) {
        declaration_errors.emplace_back("side-effect declaration has invalid pass index");
        return;
    }
    passes[pass_index].side_effect = true;
}

void RenderGraph::SetExecutionDomain(
    uint32_t pass_index,
    QueueRole queue,
    PipelineType pipeline
) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size()) {
        declaration_errors.emplace_back("execution-domain declaration has invalid pass index");
        return;
    }
    passes[pass_index].domain = ExecutionDomain{queue, pipeline};
}

void RenderGraph::AddReference(uint32_t pass_index, ResourceHandle resource) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size()) {
        declaration_errors.emplace_back("resource reference has invalid pass index");
        return;
    }
    if (!IsValidResource(resource)) {
        declaration_errors.emplace_back(
            "pass '" + passes[pass_index].name + "' references an invalid resource identity"
        );
        return;
    }
    auto& references = passes[pass_index].references;
    if (std::find(references.begin(), references.end(), resource) == references.end()) {
        references.emplace_back(resource);
    }
}

void RenderGraph::SetPassExecutionClass(
    uint32_t           pass_index,
    PassExecutionClass execution_class,
    uint32_t           workload
) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size()) {
        declaration_errors.emplace_back("recording-class declaration has invalid pass index");
        return;
    }
    if (workload == 0) {
        declaration_errors.emplace_back(
            "pass '" + passes[pass_index].name + "' has zero recording workload"
        );
        return;
    }
    passes[pass_index].execution_class = execution_class;
    passes[pass_index].workload        = workload;
}

void RenderGraph::SetPassTranslateExecutionClass(
    uint32_t                    pass_index,
    ERHITranslateExecutionClass execution_class
) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size()) {
        declaration_errors.emplace_back(
            "translation-class declaration has invalid pass index"
        );
        return;
    }
    if (execution_class != ERHITranslateExecutionClass::Parallel &&
        execution_class != ERHITranslateExecutionClass::SerialControl) {
        declaration_errors.emplace_back(
            "pass '" + passes[pass_index].name +
            "' has an invalid translation execution class"
        );
        return;
    }
    passes[pass_index].translate_execution_class = execution_class;
}

bool RenderGraph::InvalidateCompile() {
    if (executed) {
        declaration_errors.emplace_back("graph declarations cannot be mutated after execution");
        return false;
    }
    compiled = false;
    compile_error.clear();
    compiled_plan.Clear();
    ReleaseNonExportedTransientBindings();
    for (auto& resource : resources) {
        resource.first_use = PassHandle::InvalidIndex;
        resource.last_use  = PassHandle::InvalidIndex;
    }
    return true;
}

void RenderGraph::ReleaseNonExportedTransientBindings() noexcept {
    for (auto& resource : resources) {
        if (resource.imported || resource.exported) {
            continue;
        }
        resource.physical_identity = nullptr;
        resource.physical_texture  = {};
        resource.physical_buffer   = {};
    }
}

bool RenderGraph::FailCompile(std::string message) {
    compile_error = std::move(message);
    compiled      = false;
    return false;
}

bool RenderGraph::IsValidResource(ResourceHandle resource) const {
    return resource.IsValid() && resource.owner_id == graph_id && resource.index < resources.size();
}

bool RenderGraph::IsValidPass(PassHandle pass) const {
    return pass.IsValid() && pass.owner_id == graph_id && pass.index < passes.size();
}

} // namespace Moer::Render
