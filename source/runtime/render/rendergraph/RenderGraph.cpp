#include "rendergraph/RenderGraph.h"

#include "rendergraph/RenderGraphCompiler.h"
#include "rhi/RHIExecutor.h"
#include "taskgraph/TaskGraph.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <mutex>
#include <sstream>
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

RenderGraph::BufferHandle
RenderGraph::ImportBuffer(std::string_view resource_name, const void* physical_identity, BufferDesc desc) {
    return BufferHandle{ImportInternal(resource_name, ResourceKind::Buffer, physical_identity, nullptr, &desc)
    };
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

RenderGraph::TokenHandle RenderGraph::CreateTransientToken(std::string_view resource_name) {
    return TokenHandle{CreateTransientInternal(resource_name, ResourceKind::Token, nullptr, nullptr)};
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
            });
        }
    }
    return true;
}

bool RenderGraph::ExecuteRecording(
    const PassCompletedCallback&        after_main_thread_pass,
    const RecordingSourceSetupCallback& configure_recording_source,
    bool                                parallel_recording_enabled,
    const RecordingBatchPublisher&      publish_recording_batch
) {
    if (!compiled) {
        compile_error = "ExecuteRecording called before a successful Compile";
        return false;
    }
    if (executed) {
        compile_error = "a per-frame RenderGraph can only be executed once";
        return false;
    }
    executed = true;

    struct RecordingJob {
        PassHandle          pass{};
        std::string         pass_name{};
        RecordCallback      record{};
        SharedPtr<CommandList> command_list{};
        RHIRecordingGateRef completion{};
    };
    struct RecordingError {
        std::mutex  mutex{};
        std::string message{};
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

    auto make_pass_info = [&](PassHandle handle) {
        const auto& pass = passes[handle.index];
        return ExecutedPassInfo{
            .handle          = handle,
            .name            = pass.name,
            .domain          = pass.domain,
            .side_effect     = pass.side_effect,
            .execution_class = pass.execution_class,
        };
    };

    auto store_error = [](const std::shared_ptr<RecordingError>& error, std::string message) {
        std::lock_guard lock(error->mutex);
        if (error->message.empty()) {
            error->message = std::move(message);
        }
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
            first_pass.execute();
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
                    candidate.queue.role != first_batch.queue.role ||
                    candidate.queue.native_queue_id != first_batch.queue.native_queue_id ||
                    candidate.queue.family_id != first_batch.queue.family_id ||
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
            };
            RHIRecordingSource source{
                .command_list = job.command_list,
                .completion   = job.completion,
            };
            if (configure_recording_source) {
                try {
                    configure_recording_source(make_pass_info(handle), source);
                } catch (const std::exception& exception) {
                    compile_error = "recording source configuration failed for pass '" + pass.name +
                                    "': " + exception.what();
                    return false;
                } catch (...) {
                    compile_error = "recording source configuration failed for pass '" + pass.name + "'";
                    return false;
                }
                if (source.command_list != job.command_list || source.completion != job.completion) {
                    compile_error = "recording source configuration changed ownership for pass '" +
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
        auto run_job = [error, store_error](RecordingJob job) mutable {
            try {
                job.record(*job.command_list);
                // Keep every immutable capture alive until the submit reaches
                // a terminal backend/rejection callback, not merely until CPU
                // recording finishes.
                job.command_list->AddCallback(
                    [keepalive = std::move(job.record)]() mutable { keepalive = {}; }
                );
                job.completion->Signal();
            } catch (const std::exception& exception) {
                store_error(
                    error,
                    "record pass '" + job.pass_name + "' failed: " + exception.what()
                );
                job.completion->Fail();
            } catch (...) {
                store_error(error, "record pass '" + job.pass_name + "' failed");
                job.completion->Fail();
            }
        };

        // Register the unfinished sources before dispatch, matching the
        // dev_parallel/UE-style producer-consumer handoff. RHI owns stable
        // source order; worker completion order is irrelevant.
        try {
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

        const bool task_graph_available = TaskGraph::IsInitialized();
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
        pending_gate_guard.Disarm();
        batch_index = group_end;
    }
    return true;
}

std::string RenderGraph::Dump() const {
    std::ostringstream stream;
    stream << "graph='" << name
           << "' frontend=typed-rdg mode=serial barrier_owner=existing_rhi_vulkan_path "
              "sync_plan=shadow external_endpoints=unbound state_plan="
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
        if (index < compiled_plan.resources.size()) {
            version_count = compiled_plan.resources[index].version_count;
        }
        stream << " versions=" << version_count << " exported=" << (resource.exported ? "true" : "false")
               << " aliases=[";
        auto aliases = resource.aliases;
        std::sort(aliases.begin(), aliases.end());
        for (uint32_t alias_index = 0; alias_index < aliases.size(); ++alias_index) {
            if (alias_index != 0) {
                stream << ", ";
            }
            stream << aliases[alias_index];
        }
        stream << "] initial_states=" << resource.initial_states.size()
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
               << batch.queue.native_queue_id << " family=" << batch.queue.family_id << " passes=[";
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
               << " cpu=" << ToString(batch.execution) << " workload=" << batch.workload
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

bool RenderGraph::InvalidateCompile() {
    if (executed) {
        declaration_errors.emplace_back("graph declarations cannot be mutated after execution");
        return false;
    }
    compiled = false;
    compile_error.clear();
    compiled_plan.Clear();
    for (auto& resource : resources) {
        resource.first_use = PassHandle::InvalidIndex;
        resource.last_use  = PassHandle::InvalidIndex;
    }
    return true;
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
