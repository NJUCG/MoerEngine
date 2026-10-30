#include "rendergraph/RenderGraphLowering.h"
#include "misc/Types.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHIResource.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Moer::Render::Buffer;
using Moer::Render::BufferInfo;
using Moer::Render::BufferRef;
using Moer::Render::RenderGraph;
using Moer::Render::RenderGraphLowering;
using Moer::Render::Texture;
using Moer::Render::TextureInfo;
using Moer::Render::TextureRef;

template<typename Type>
concept PassTextureAccessType = requires(RenderGraph::PassBuilder& builder, RenderGraph::TextureHandle texture, Type type) {
    builder.Read(texture, type);
    builder.Write(texture, type);
    builder.ReadWrite(texture, type);
};

template<typename Type>
concept PassBufferAccessType = requires(RenderGraph::PassBuilder& builder, RenderGraph::BufferHandle buffer, Type type) {
    builder.Read(buffer, type);
    builder.Write(buffer, type);
    builder.ReadWrite(buffer, type);
};

static_assert(PassTextureAccessType<RenderGraph::TextureAccessType>);
static_assert(!PassTextureAccessType<RenderGraph::TextureBoundary>);
static_assert(!PassTextureAccessType<RenderGraph::TextureBoundaryState>);
static_assert(!PassTextureAccessType<RenderGraph::BufferAccessType>);
static_assert(PassBufferAccessType<RenderGraph::BufferAccessType>);
static_assert(!PassBufferAccessType<RenderGraph::BufferBoundary>);
static_assert(!PassBufferAccessType<RenderGraph::TextureAccessType>);

class TestSuite {
public:
    void Check(bool condition, std::string_view test, std::string_view message) {
        if (condition) {
            return;
        }
        ++failures;
        std::cerr << "[FAIL] " << test << ": " << message << "\n";
    }

    [[nodiscard]] int FailureCount() const {
        return failures;
    }

private:
    int failures = 0;
};

[[nodiscard]] bool Contains(std::string_view text, std::string_view expected) {
    return text.find(expected) != std::string_view::npos;
}

class FakeTexture final : public Texture {
public:
    FakeTexture(Moer::uint8 mips, Moer::uint16 layers, ETextureAspectFlags aspects) :
        Texture(MakeInfo(mips, layers, aspects)) {}

    Moer::uint32 GetMipByteSize(Moer::uint32) const override {
        return 4;
    }

    void SetName(std::string_view) override {}

private:
    static TextureInfo
    MakeInfo(Moer::uint8 mips, Moer::uint16 layers, ETextureAspectFlags aspects) {
        TextureInfo info{};
        info.usage = ETextureUsageFlags::SAMPLED |
                     ETextureUsageFlags::UNORDERED_ACCESS |
                     ETextureUsageFlags::COLOR_ATTACHMENT |
                     ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT |
                     ETextureUsageFlags::TRANSFER_SRC |
                     ETextureUsageFlags::TRANSFER_DST;
        info.extent       = {64, 64};
        info.num_mips     = mips;
        info.array_size   = layers;
        info.aspect_flags = aspects;
        return info;
    }
};

class FakeBuffer final : public Buffer {
public:
    explicit FakeBuffer(Moer::uint64 byte_size) :
        Buffer(BufferInfo{byte_size, 1, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST}) {}

    void SetName(std::string_view) override {}
};

template<typename Enum>
[[nodiscard]] bool HasFlag(Enum value, Enum flag) {
    return (static_cast<Moer::uint32>(value) & static_cast<Moer::uint32>(flag)) != 0;
}

void TestKeepaliveCombinesCellsAndReferences(TestSuite& suite) {
    constexpr std::string_view name = "keepalive combines cells and identity references";
    RenderGraphLowering::LoweredPlan lowered;
    std::string dump;
    {
        RenderGraph graph(name);
        std::array<RenderGraph::BufferHandle, 3> handles;
        for (Moer::uint32 index = 0; index < handles.size(); ++index) {
            BufferRef buffer = MoerNew(FakeBuffer)(256);
            handles[index] = graph.ImportBuffer(
                std::to_string(index), buffer, RenderGraph::BufferDesc{.byte_size = 256}
            );
            graph.SetInitialState(handles[index], RenderGraph::BufferAccessType::ShaderRead,
                                  RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
            graph.Export(handles[index], RenderGraph::BufferAccessType::ShaderRead,
                                RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
        }
        for (const bool references : {true, false}) {
            graph.AddRecordPass(
                references ? "Referenced" : "CellsOnly",
                [=](RenderGraph::PassBuilder& builder) {
                    builder.Read(handles[2], RenderGraph::BufferAccessType::ShaderRead)
                        .Read(handles[1], RenderGraph::BufferAccessType::ShaderRead,
                              RenderGraph::BufferRange{0, 128})
                        .Read(handles[1], RenderGraph::BufferAccessType::ShaderRead,
                              RenderGraph::BufferRange{128, 128}).SideEffect();
                    if (references) {
                        builder.Reference(handles[2]).Reference(handles[0]).Reference(handles[1]);
                    }
                },
                [](Moer::Render::CommandList&) {}, RenderGraph::PassRecordPolicy::Serial
            );
        }
        suite.Check(graph.Compile(), name, graph.GetCompileError());
        std::string error;
        const bool success = RenderGraphLowering::Lower(graph, lowered, error);
        suite.Check(success, name, error);
        dump = lowered.Dump();
    }
    suite.Check(lowered.passes.size() == 2, name, "both passes must survive graph destruction");
    if (lowered.passes.size() != 2) { return; }
    for (size_t pass = 0; pass < lowered.passes.size(); ++pass) {
        const auto& bindings = lowered.passes[pass].keepalive;
        suite.Check(bindings.size() == 3 - pass, name, "retain each physical resource exactly once");
        for (size_t index = 0; index < bindings.size(); ++index) {
            suite.Check(bindings[index].resource.resource_id == index + pass && bindings[index].buffer,
                        name, "bindings must stay owned and ordered, including reference-only resources");
        }
    }
    suite.Check(lowered.Dump() == dump, name, "lowered output must outlive graph and scratch storage");
}

void TestReverseDependencyUsesCompiledOrder(TestSuite& suite) {
    constexpr std::string_view test_name = "reverse dependency preserves resource semantics through lowering";
    RenderGraph graph("ReverseDependency", RenderGraph::QueueTopology::DedicatedQueues());
    BufferRef shared_buffer = MoerNew(FakeBuffer)(256);
    BufferRef produced_buffer = MoerNew(FakeBuffer)(256);
    const auto shared = graph.ImportBuffer("Shared", shared_buffer, RenderGraph::BufferDesc{.byte_size = 256});
    const auto produced = graph.ImportBuffer("Produced", produced_buffer, RenderGraph::BufferDesc{.byte_size = 256});
    graph.SetInitialState(shared, RenderGraph::BufferAccessType::ShaderRead,
                          RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    graph.SetInitialState(produced, RenderGraph::BufferBoundary::Undefined,
                          RenderGraph::QueueRole::None, RenderGraph::AccessMode::None);
    std::optional<RenderGraph::PassBuilder> first_builder;
    std::vector<std::string_view> callbacks;
    const auto first_reader = graph.AddRecordPass(
        "FirstReader",
        [&](RenderGraph::PassBuilder& builder) {
            builder.Read(shared, RenderGraph::BufferAccessType::ShaderRead);
            first_builder.emplace(builder);
        },
        [&](Moer::Render::CommandList&) { callbacks.push_back("FirstReader"); },
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto writer = graph.AddRecordPass(
        "ComputeWriter",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute)
                .Write(produced, RenderGraph::BufferAccessType::Storage);
        },
        [&](Moer::Render::CommandList&) { callbacks.push_back("ComputeWriter"); },
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto last_reader = graph.AddRecordPass(
        "LastReader",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(shared, RenderGraph::BufferAccessType::ShaderRead)
                .Read(produced, RenderGraph::BufferAccessType::ShaderRead);
        },
        [&](Moer::Render::CommandList&) { callbacks.push_back("LastReader"); },
        RenderGraph::PassRecordPolicy::Serial
    );
    // The compatible Shared readers may reverse, while Produced must retain
    // its writer->reader ordering. No declaration acquires a different version.
    first_builder->DependsOn(last_reader);
    graph.Export(shared, RenderGraph::BufferAccessType::TransferSource,
                 RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    graph.Export(produced, RenderGraph::BufferAccessType::ShaderRead,
                 RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    if (!compiled) {
        return;
    }
    const auto& plan = graph.GetCompiledPlan();
    const std::vector<RenderGraph::PassHandle> expected_order{writer, last_reader, first_reader};
    suite.Check(plan.execution_order == expected_order, test_name,
                "an acyclic backward explicit edge must determine execution order");
    const auto& shared_lifetime = plan.resources[shared.Untyped().resource_id];
    const auto& produced_lifetime = plan.resources[produced.Untyped().resource_id];
    suite.Check(shared_lifetime.first_use == 1 && shared_lifetime.last_use == 2 &&
                    produced_lifetime.first_use == 0 && produced_lifetime.last_use == 1,
                test_name, "resource lifetimes must use execution positions rather than pass identities");
    const auto write_access = std::find_if(plan.accesses.begin(), plan.accesses.end(), [&](const auto& access) {
        return access.pass == writer && access.resource == produced.Untyped();
    });
    const auto read_access = std::find_if(plan.accesses.begin(), plan.accesses.end(), [&](const auto& access) {
        return access.pass == last_reader && access.resource == produced.Untyped();
    });
    suite.Check(write_access != plan.accesses.end() && read_access != plan.accesses.end() &&
                    write_access->output_version == read_access->input_version &&
                    read_access->input_version != RenderGraph::InvalidVersion,
                test_name, "reordering must preserve the version selected by the resource declarations");
    suite.Check(plan.queue_batches.size() == 2 &&
                    plan.queue_batches[0].passes == std::vector<RenderGraph::PassHandle>{writer} &&
                    plan.queue_batches[1].passes == std::vector<RenderGraph::PassHandle>{last_reader, first_reader},
                test_name, "queue batches must follow the compiled order across queue families");

    RenderGraphLowering::LoweredPlan lowered;
    std::string error;
    const bool lowered_ok = RenderGraphLowering::Lower(graph, lowered, error);
    suite.Check(lowered_ok, test_name, error);
    if (!lowered_ok) {
        return;
    }
    std::vector<RenderGraph::PassHandle> lowered_order;
    for (const auto& pass : lowered.passes) {
        lowered_order.push_back(pass.pass);
    }
    suite.Check(lowered_order == expected_order, test_name,
                "lowered instructions must preserve the compiled execution order");
    suite.Check(lowered.queue_syncs.size() == 1 &&
                    lowered.queue_syncs.front().signal_pass == writer &&
                    lowered.queue_syncs.front().wait_pass == last_reader &&
                    lowered.queue_syncs.front().signal_batch == 0 &&
                    lowered.queue_syncs.front().wait_batch == 1,
                test_name, "the cross-family wait must precede the actual consumer batch");
    const auto shared_export = [&](const auto& instruction) {
        return instruction.export_boundary && instruction.resource == shared.Untyped();
    };
    const auto after_first = lowered.After(first_reader);
    const auto after_last = lowered.After(last_reader);
    const auto final_barrier = std::find_if(after_first.begin(), after_first.end(), shared_export);
    suite.Check(final_barrier != after_first.end() && final_barrier->source_frontier.size() == 2 &&
                    std::none_of(after_last.begin(), after_last.end(), shared_export),
                test_name, "the complete reader frontier must export after the latest execution position, not greatest pass id");

    const std::string compiled_dump = graph.Dump();
    const std::string lowered_dump = lowered.Dump();
    const bool recompiled = graph.Compile();
    suite.Check(recompiled, test_name, graph.GetCompileError());
    const bool lowered_again = recompiled && RenderGraphLowering::Lower(graph, lowered, error);
    suite.Check(lowered_again, test_name, error);
    suite.Check(lowered_again && graph.Dump() == compiled_dump && lowered.Dump() == lowered_dump,
                test_name, "recompilation must preserve the reordered execution and lowering plans");
    const bool recorded = lowered_again && graph.ExecuteFrontendRecordingPlan(
        {}, false, [](Moer::Array<Moer::Render::RHIRecordingSource>&&) {}
    );
    suite.Check(recorded, test_name, graph.GetCompileError());
    suite.Check(callbacks == std::vector<std::string_view>{"ComputeWriter", "LastReader", "FirstReader"},
                test_name, "serial recording callbacks must follow compiled order exactly once");
}

void TestTextureFanInAndDeterminism(TestSuite& suite) {
    constexpr std::string_view test_name = "texture fan-in and deterministic lowering";
    TextureRef texture = MoerNew(FakeTexture)(2, 1, ETextureAspectFlags::COLOR);

    RenderGraph graph("TextureFanIn");
    const auto  color = graph.ImportTexture(
        "Color",
        texture,
        RenderGraph::TextureDesc{
            .mip_count   = 2,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    const auto range = RenderGraph::TextureRange::Mips(0, 1);
    graph.SetInitialState(
        color,
        RenderGraph::TextureBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        range
    );
    const auto writer = graph.AddRecordPass(
        "Writer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(color, RenderGraph::TextureAccessType::ColorAttachment, range).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto graphics_reader = graph.AddRecordPass(
        "GraphicsReader",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(color, RenderGraph::TextureAccessType::Storage, range).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_reader = graph.AddRecordPass(
        "ComputeReader",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::GpuWorkType::Compute
            )
                .Read(color, RenderGraph::TextureAccessType::Storage, range)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto uav_writer = graph.AddRecordPass(
        "UavWriter",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::GpuWorkType::Compute
            )
                .Write(color, RenderGraph::TextureAccessType::Storage, range)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        color,
        RenderGraph::TextureAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        range
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan first{};
    RenderGraphLowering::LoweredPlan second{};
    std::string error{};
    suite.Check(
        RenderGraphLowering::Lower(graph, first, error),
        test_name,
        error
    );
    suite.Check(
        RenderGraphLowering::Lower(graph, second, error),
        test_name,
        error
    );
    suite.Check(first.Dump() == second.Dump(), test_name, "lowered dumps must be deterministic");
    suite.Check(
        first.prologue.size() == 1 &&
            first.prologue.front().discard_previous_contents &&
            first.prologue.front().source.layout ==
                ETextureLayout::TEXTURE_LAYOUT_UNDEFINED &&
            first.prologue.front().destination.layout ==
                ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT &&
            std::get<RenderGraph::TextureRange>(first.prologue.front().range).mip_count == 1 &&
            first.prologue.front().texture_aspects == ETextureAspectFlags::COLOR,
        test_name,
        "prologue must retain discard, normalized range, aspect and layouts"
    );

    const auto before_uav = first.Before(uav_writer);
    const auto fan_in     = std::find_if(
        before_uav.begin(),
        before_uav.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.after_state ==
                       RenderGraph::ResourceState{RenderGraph::TextureAccessType::Storage};
        }
    );
    suite.Check(
        fan_in != before_uav.end() && fan_in->source_frontier.size() >= 2 &&
            HasFlag(fan_in->source.stages, ERHIPipelineStageFlags::PS_ALL_GRAPHICS) &&
            HasFlag(fan_in->source.stages, ERHIPipelineStageFlags::PS_COMPUTE_SHADER) &&
            fan_in->source.access == ERHIAccessFlags::SHADER_READ &&
            fan_in->source.layout == ETextureLayout::TEXTURE_LAYOUT_COMMON,
        test_name,
        "fan-in source stages and accesses must be ORed across the authoritative frontier"
    );
    suite.Check(
        first.Keepalive(writer).size() == 1 &&
            first.Keepalive(graphics_reader).size() == 1 &&
            first.Keepalive(compute_reader).size() == 1 &&
            first.Keepalive(uav_writer).size() == 1 &&
            first.Keepalive(compute_reader).front().texture.Get() == texture.Get(),
        test_name,
        "every GPU pass must retain its physical resource"
    );
    suite.Check(
        first.After(uav_writer).size() == 1 &&
            first.After(uav_writer).front().export_boundary,
        test_name,
        "epilogue must be attached after its latest source pass"
    );
}

void TestShaderResourceAndSampledLayoutsRemainDistinct(TestSuite& suite) {
    constexpr std::string_view test_name =
        "shader-resource and sampled layouts remain distinct";
    TextureRef texture = MoerNew(FakeTexture)(1, 1, ETextureAspectFlags::COLOR);

    RenderGraph graph("DistinctShaderReadLayouts");
    const auto color = graph.ImportTexture(
        "Color",
        texture,
        RenderGraph::TextureDesc{}
    );
    graph.SetInitialState(
        color,
        RenderGraph::TextureAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.AddRecordPass(
        "StorageRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(color, RenderGraph::TextureAccessType::Storage).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto sampled_read = graph.AddRecordPass(
        "SampledRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(color, RenderGraph::TextureAccessType::Sampled).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        color,
        RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);
    const auto before_sampled = lowered.Before(sampled_read);
    const auto transition = std::find_if(
        before_sampled.begin(),
        before_sampled.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.state_transition;
        }
    );
    suite.Check(
        transition != before_sampled.end() &&
            transition->source.layout == ETextureLayout::TEXTURE_LAYOUT_COMMON &&
            transition->destination.layout ==
                ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            transition->source.access == ERHIAccessFlags::SHADER_READ &&
            transition->destination.access == ERHIAccessFlags::SHADER_READ,
        test_name,
        "the compiler/lowerer must preserve the physical SRV-to-sampled layout transition"
    );
}

void TestDepthAttachmentReadUsesBackendAttachmentLayout(TestSuite& suite) {
    constexpr std::string_view test_name =
        "depth attachment read uses backend attachment layout";
    TextureRef texture = MoerNew(FakeTexture)(1, 1, ETextureAspectFlags::DEPTH_SLICE);

    RenderGraph graph("DepthAttachmentReadLayout");
    const auto depth = graph.ImportTexture(
        "Depth",
        texture,
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Depth,
        }
    );
    graph.SetInitialState(
        depth,
        RenderGraph::TextureBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddRecordPass(
        "DepthWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(depth, RenderGraph::TextureAccessType::DepthStencilAttachment).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto depth_read = graph.AddRecordPass(
        "DepthRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(depth, RenderGraph::TextureAccessType::DepthStencilAttachment).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        depth,
        RenderGraph::TextureAccessType::DepthStencilAttachment,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);

    const auto before_read = lowered.Before(depth_read);
    const auto transition  = std::find_if(
        before_read.begin(),
        before_read.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.after_state ==
                       RenderGraph::ResourceState{RenderGraph::TextureAccessType::DepthStencilAttachment};
        }
    );
    suite.Check(
        transition != before_read.end() &&
            transition->destination.layout ==
                ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE &&
            transition->destination.access == ERHIAccessFlags::DEPTH_STENCIL_READ &&
            HasFlag(transition->source.access, ERHIAccessFlags::DEPTH_STENCIL_WRITE) &&
            transition->source.layout == transition->destination.layout,
        test_name,
        "load-only depth attachments must match the backend attachment layout while retaining read access"
    );
}

void TestPresentationSourceLowersToCommonTransferRead(TestSuite& suite) {
    constexpr std::string_view test_name =
        "presentation source lowers to common transfer read";
    TextureRef texture =
        MoerNew(FakeTexture)(1, 1, ETextureAspectFlags::COLOR);

    RenderGraph graph("PresentationSourceLowering");
    const auto  color = graph.ImportTexture(
        "Color",
        texture,
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    graph.SetInitialState(
        color,
        RenderGraph::TextureBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    const auto writer = graph.AddRecordPass(
        "Writer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(color, RenderGraph::TextureAccessType::ColorAttachment)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        color,
        RenderGraph::TextureBoundary::PresentationSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(
        RenderGraphLowering::Lower(graph, lowered, error),
        test_name,
        error
    );

    const auto after_writer = lowered.After(writer);
    const auto transition   = std::find_if(
        after_writer.begin(),
        after_writer.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.export_boundary &&
                   instruction.after_state ==
                       RenderGraph::ResourceState{RenderGraph::TextureBoundary::PresentationSource};
        }
    );
    suite.Check(
        transition != after_writer.end() &&
            transition->destination.layout ==
                ETextureLayout::TEXTURE_LAYOUT_COMMON &&
            transition->destination.stages ==
                ERHIPipelineStageFlags::PS_TRANSFER &&
            transition->destination.access ==
                ERHIAccessFlags::TRANSFER_READ,
        test_name,
        "presentation export must lower to COMMON / TRANSFER / TRANSFER_READ"
    );

    TextureRef rejected_texture =
        MoerNew(FakeTexture)(1, 1, ETextureAspectFlags::COLOR);
    RenderGraph rejected_graph("PresentationSourceImportLoweringRejected");
    const auto rejected_color = rejected_graph.ImportTexture(
        "Color",
        rejected_texture,
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    rejected_graph.SetInitialState(
        rejected_color,
        RenderGraph::TextureBoundary::PresentationSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    rejected_graph.AddRecordPass(
        "ReadImportedColor",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(
                    rejected_color,
                    RenderGraph::TextureAccessType::Storage
                )
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    rejected_graph.Export(
        rejected_color,
        RenderGraph::TextureAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(rejected_graph.Compile(), test_name, rejected_graph.GetCompileError());
    RenderGraphLowering::LoweredPlan imported_plan{};
    std::string                      imported_error{};
    suite.Check(
        RenderGraphLowering::Lower(rejected_graph, imported_plan, imported_error), test_name, imported_error
    );
    suite.Check(
        !imported_plan.prologue.empty() &&
            imported_plan.prologue.front().source.stages == ERHIPipelineStageFlags::PS_TRANSFER &&
            imported_plan.prologue.front().source.access == ERHIAccessFlags::TRANSFER_READ &&
            imported_plan.prologue.front().source.layout == ETextureLayout::TEXTURE_LAYOUT_COMMON,
        test_name,
        "presentation reimport must wait for the preceding copy read in GENERAL"
    );
}

void TestUntouchedImportedRangesNeedNoCommands(TestSuite& suite) {
    constexpr std::string_view name    = "untouched imported ranges preserve read boundary";
    TextureRef                 texture = MoerNew(FakeTexture)(2, 1, ETextureAspectFlags::COLOR);
    RenderGraph                graph(name);
    const auto                 image = graph.ImportTexture("Image", texture, {.mip_count = 2});
    graph.SetInitialState(
        image,
        RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        image,
        RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto pass = graph.AddRecordPass(
        "ReadMip0",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(image, RenderGraph::TextureAccessType::Sampled, RenderGraph::TextureRange::Mips(0, 1))
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan plan;
    std::string                      error;
    suite.Check(RenderGraphLowering::Lower(graph, plan, error), name, error);
    for (const auto& instruction : plan.After(pass)) {
        suite.Check(
            std::get<RenderGraph::TextureRange>(instruction.range).mip_first == 0,
            name,
            "untouched mip must not be attached to an unrelated pass"
        );
    }
}

void TestAccelerationBuildInputScopes(TestSuite& suite) {
    constexpr std::string_view name   = "build input covers geometry and referenced BLAS reads";
    BufferRef                  buffer = MoerNew(FakeBuffer)(256);
    RenderGraph                graph(name);
    const auto                 input = graph.ImportBuffer("Input", buffer, {.byte_size = 256});
    graph.SetInitialState(
        input,
        RenderGraph::BufferAccessType::TransferDestination,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    graph.AddRecordPass(
        "Build",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Compute)
                .Read(input, RenderGraph::BufferAccessType::AccelerationStructureBuildInput)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        input,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    suite.Check(graph.Compile(), name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan plan{};
    std::string                      error;
    suite.Check(RenderGraphLowering::Lower(graph, plan, error), name, error);
    suite.Check(
        plan.prologue.size() == 1 &&
            plan.prologue.front().destination.stages ==
                ERHIPipelineStageFlags::PS_ACCELERATION_STRUCTURE_BUILD &&
            HasFlag(plan.prologue.front().destination.access, ERHIAccessFlags::SHADER_READ) &&
            HasFlag(
                plan.prologue.front().destination.access, ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT
            ),
        name,
        "transfer writes must become visible to both raw geometry and AS build reads"
    );
}

void TestSameStateReadNeedsNoBarrier(TestSuite& suite) {
    constexpr std::string_view test_name = "same-state read needs no synthetic barrier";
    BufferRef buffer = MoerNew(FakeBuffer)(256);

    RenderGraph graph("ReadSeed");
    const auto data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 256}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto first_read = graph.AddRecordPass(
        "FirstRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferAccessType::ShaderRead).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto second_read = graph.AddRecordPass(
        "SecondRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferAccessType::ShaderRead).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);

    suite.Check(
        lowered.prologue.size() == 1 && lowered.prologue.front().dst_pass == first_read,
        test_name,
        "the first access must adopt its explicit import boundary"
    );
    suite.Check(lowered.Before(second_read).empty(), test_name,
                "read-after-read must not emit a synthetic dependency");
    suite.Check(
        lowered.Keepalive(second_read).size() == 1 &&
            lowered.Keepalive(second_read).front().buffer.Get() == buffer.Get(),
        test_name,
        "barrier-free pass must retain its buffer even without a compiler barrier"
    );
}

void ExpectLowerFailure(
    TestSuite&       suite,
    RenderGraph&     graph,
    std::string_view test_name,
    std::string_view expected
);

void TestCrossNativeTokenSynchronization(TestSuite& suite) {
    constexpr std::string_view test_name =
        "cross-native token synchronization is lowered";
    RenderGraph graph("CrossNativeTokenSync", RenderGraph::QueueTopology::DedicatedQueues());
    const auto token = graph.CreateTransientToken("GraphicsToCompute");
    const auto producer = graph.AddRecordPass(
        "GraphicsProducer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::GpuWorkType::Graphics
                   )
                .Write(token)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto consumer = graph.AddRecordPass(
        "ComputeConsumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::GpuWorkType::Compute
                   )
                .Read(token)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);
    suite.Check(
        lowered.queue_syncs.size() == 1 &&
            lowered.queue_syncs.front().correlation_id == 0 &&
            lowered.queue_syncs.front().signal_pass == producer &&
            lowered.queue_syncs.front().wait_pass == consumer &&
            lowered.queue_syncs.front().signal_queue ==
                graph.GetQueueTopology().Resolve(RenderGraph::QueueRole::Graphics) &&
            lowered.queue_syncs.front().wait_queue ==
                graph.GetQueueTopology().Resolve(RenderGraph::QueueRole::Compute),
        test_name,
        "the token hazard must become one Graphics-to-Compute lowered GPU sync"
    );
    suite.Check(
        lowered.prologue.empty() && lowered.Before(consumer).empty(),
        test_name,
        "a token-only dependency must not manufacture a physical barrier"
    );
}

void TestSameNativeTokenSynchronizationNeedsNoGpuSync(TestSuite& suite) {
    constexpr std::string_view test_name =
        "same-native token synchronization needs no lowered GPU sync";
    RenderGraph graph("SameNativeTokenSync", RenderGraph::QueueTopology::SingleQueue());
    const auto token = graph.CreateTransientToken("GraphicsToCompute");
    graph.AddRecordPass(
        "GraphicsProducer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::GpuWorkType::Graphics
                   )
                .Write(token)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "ComputeConsumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::GpuWorkType::Compute
                   )
                .Read(token)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    suite.Check(
        graph.GetCompiledPlan().queue_syncs.size() == 1 &&
            !graph.GetCompiledPlan().queue_syncs.front().gpu_wait_required,
        test_name,
        "the compiler must retain the logical queue edge without requesting a GPU wait"
    );
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);
    suite.Check(
        lowered.queue_syncs.empty(),
        test_name,
        "logical roles sharing one native queue must rely on native submission order"
    );
}

void TestSameNativePhysicalBarrierRetainsSourceScope(TestSuite& suite) {
    constexpr std::string_view test_name =
        "same-native physical queue barrier retains source scope";
    BufferRef buffer = MoerNew(FakeBuffer)(256);
    RenderGraph graph("SameNativePhysicalBarrier", RenderGraph::QueueTopology::SingleQueue());
    const auto data = graph.ImportBuffer(
        "Shared",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 256}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddRecordPass(
        "GraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto consumer = graph.AddRecordPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::GpuWorkType::Compute
            )
                .Read(data, RenderGraph::BufferAccessType::ShaderRead)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);
    const auto before_consumer = lowered.Before(consumer);
    const auto barrier         = std::find_if(
        before_consumer.begin(),
        before_consumer.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.after_state ==
                       RenderGraph::ResourceState{RenderGraph::BufferAccessType::ShaderRead};
        }
    );
    suite.Check(
        lowered.queue_syncs.empty() &&
            barrier != before_consumer.end() &&
            !barrier->queue_acquire &&
            barrier->source.stages != ERHIPipelineStageFlags::PS_NONE &&
            barrier->source.access == ERHIAccessFlags::SHADER_WRITE,
        test_name,
        "one native queue needs no GPU wait but must retain its producer scope"
    );
}

void TestSameFamilyQueueBarrierLowersToAcquire(TestSuite& suite) {
    constexpr std::string_view test_name =
        "same-family queue barrier lowers to destination acquire";
    const RenderGraph::QueueTopology topology{
        .graphics = {RenderGraph::QueueRole::Graphics, 0, 0},
        .compute  = {RenderGraph::QueueRole::Compute, 1, 0},
        .copy     = {RenderGraph::QueueRole::Copy, 2, 0},
    };
    BufferRef buffer = MoerNew(FakeBuffer)(256);
    RenderGraph graph("SameFamilyQueueAcquire", topology);
    const auto data = graph.ImportBuffer(
        "Shared",
        buffer,
        RenderGraph::BufferDesc{
            .byte_size    = 256,
            .sharing_mode = RenderGraph::SharingMode::Exclusive,
        }
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddRecordPass(
        "GraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto consumer = graph.AddRecordPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::GpuWorkType::Compute
            )
                .Read(data, RenderGraph::BufferAccessType::ShaderRead)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);
    const auto before_consumer = lowered.Before(consumer);
    const auto acquire = std::find_if(
        before_consumer.begin(),
        before_consumer.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.queue_acquire;
        }
    );
    suite.Check(
        lowered.queue_syncs.size() == 1 &&
            acquire != before_consumer.end() &&
            acquire->source.stages == ERHIPipelineStageFlags::PS_NONE &&
            acquire->source.access == ERHIAccessFlags::UNDEFINED &&
            acquire->destination.stages == ERHIPipelineStageFlags::PS_COMPUTE_SHADER &&
            acquire->destination.access == (ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::UNIFORM_READ),
        test_name,
        "the GPU wait must be paired with a destination-local acquire whose source scope is empty"
    );
}

void TestMixedQueueFanInRetainsLocalSourceScope(TestSuite& suite) {
    constexpr std::string_view test_name =
        "mixed queue fan-in retains the local source scope";
    const RenderGraph::QueueTopology topology{
        .graphics = {RenderGraph::QueueRole::Graphics, 0, 0},
        .compute  = {RenderGraph::QueueRole::Compute, 1, 0},
        .copy     = {RenderGraph::QueueRole::Copy, 2, 0},
    };
    BufferRef buffer = MoerNew(FakeBuffer)(256);
    RenderGraph graph("MixedQueueFanIn", topology);
    const auto data = graph.ImportBuffer(
        "Shared",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 256}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddRecordPass(
        "InitialGraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto graphics_reader = graph.AddRecordPass(
        "GraphicsRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferAccessType::ShaderRead).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_reader = graph.AddRecordPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::GpuWorkType::Compute
            )
                .Read(data, RenderGraph::BufferAccessType::ShaderRead)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto final_writer = graph.AddRecordPass(
        "FinalGraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);
    const auto before_writer = lowered.Before(final_writer);
    const auto barrier = std::find_if(
        before_writer.begin(),
        before_writer.end(),
        [&](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.queue_acquire &&
                   std::find(
                       instruction.source_frontier.begin(),
                       instruction.source_frontier.end(),
                       graphics_reader
                   ) != instruction.source_frontier.end() &&
                   std::find(
                       instruction.source_frontier.begin(),
                       instruction.source_frontier.end(),
                       compute_reader
                   ) != instruction.source_frontier.end();
        }
    );
    suite.Check(
        barrier != before_writer.end() &&
            HasFlag(
                barrier->source.stages,
                ERHIPipelineStageFlags::PS_ALL_GRAPHICS
            ) &&
            !HasFlag(
                barrier->source.stages,
                ERHIPipelineStageFlags::PS_COMPUTE_SHADER
            ) &&
            barrier->source.access == (ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::UNIFORM_READ),
        test_name,
        "the semaphore covers the remote source while the barrier retains "
        "the same-native graphics source"
    );
}

void TestCrossFamilyExclusiveOwnershipLowersPairedTransfer(TestSuite& suite) {
    constexpr std::string_view test_name = "cross-family exclusive ownership lowers one paired transfer";
    TextureRef                 texture   = MoerNew(FakeTexture)(2, 2, ETextureAspectFlags::COLOR);
    RenderGraph                graph("CrossFamilyOwnership", RenderGraph::QueueTopology::DedicatedQueues());
    const auto                 data = graph.ImportTexture(
        "Exclusive",
        texture,
        RenderGraph::TextureDesc{
                            .mip_count    = 2,
                            .layer_count  = 2,
                            .aspects      = RenderGraph::TextureAspect::Color,
                            .sharing_mode = RenderGraph::SharingMode::Exclusive,
        }
    );
    graph.SetInitialState(
        data,
        RenderGraph::TextureBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    const auto producer = graph.AddRecordPass(
        "GraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::TextureAccessType::Storage).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto consumer = graph.AddRecordPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute)
                .Read(data, RenderGraph::TextureAccessType::Sampled)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& compiled          = graph.GetCompiledPlan();
    const auto  ownership_barrier = std::find_if(
        compiled.barriers.begin(),
        compiled.barriers.end(),
        [&](const RenderGraph::CompiledBarrier& barrier) {
            return barrier.resource == data.Untyped() && barrier.dst_pass == consumer &&
                   barrier.queue_ownership;
        }
    );
    suite.Check(
        ownership_barrier != compiled.barriers.end(),
        test_name,
        "compiler did not retain the internal ownership transfer"
    );
    if (ownership_barrier == compiled.barriers.end()) {
        return;
    }
    const Moer::uint32 ownership_barrier_index =
        static_cast<Moer::uint32>(ownership_barrier - compiled.barriers.begin());

    RenderGraphLowering::LoweredPlan lowered{};
    std::string                      error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);

    const auto after_producer = lowered.After(producer);
    const auto release        = std::find_if(
        after_producer.begin(),
        after_producer.end(),
        [&](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueRelease &&
                   instruction.correlation_id == ownership_barrier_index;
        }
    );
    const auto before_consumer = lowered.Before(consumer);
    const auto acquire         = std::find_if(
        before_consumer.begin(),
        before_consumer.end(),
        [&](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueAcquire &&
                   instruction.correlation_id == ownership_barrier_index;
        }
    );
    Moer::uint32 release_count = 0;
    Moer::uint32 acquire_count = 0;
    for (const auto& pass : lowered.passes) {
        release_count += static_cast<Moer::uint32>(std::count_if(
            pass.after.begin(),
            pass.after.end(),
            [&](const RenderGraphLowering::LoweredInstruction& instruction) {
                return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueRelease &&
                       instruction.correlation_id == ownership_barrier_index;
            }
        ));
        acquire_count += static_cast<Moer::uint32>(std::count_if(
            pass.before.begin(),
            pass.before.end(),
            [&](const RenderGraphLowering::LoweredInstruction& instruction) {
                return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueAcquire &&
                       instruction.correlation_id == ownership_barrier_index;
            }
        ));
    }
    suite.Check(
        release_count == 1 && acquire_count == 1 && release != after_producer.end() &&
            acquire != before_consumer.end(),
        test_name,
        "one ownership barrier must lower to exactly one release and one acquire"
    );
    if (release == after_producer.end() || acquire == before_consumer.end()) {
        return;
    }

    suite.Check(
        release->barrier_index == ownership_barrier_index &&
            acquire->barrier_index == ownership_barrier_index && release->resource == acquire->resource &&
            release->resource == data.Untyped() && release->range == acquire->range &&
            release->before_state == acquire->before_state && release->after_state == acquire->after_state &&
            release->source.layout == acquire->source.layout &&
            release->destination.layout == acquire->destination.layout &&
            release->source.layout == ETextureLayout::TEXTURE_LAYOUT_COMMON &&
            release->destination.layout == ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        test_name,
        "paired halves must preserve identical correlation, resource, range, state and layout"
    );

    const auto& topology = graph.GetQueueTopology();
    suite.Check(
        release->transfer_source == topology.graphics && acquire->transfer_source == topology.graphics &&
            release->transfer_destination == topology.compute &&
            acquire->transfer_destination == topology.compute &&
            release->transfer_source.family_id != release->transfer_destination.family_id &&
            !release->queue_acquire && acquire->queue_acquire,
        test_name,
        "paired halves must retain the Graphics-to-Compute family transfer"
    );

    const auto transfer_sync = std::find_if(
        lowered.queue_syncs.begin(),
        lowered.queue_syncs.end(),
        [&](const RenderGraphLowering::QueueSyncInstruction& sync) {
            return std::find(
                       sync.ownership_transfer_barriers.begin(),
                       sync.ownership_transfer_barriers.end(),
                       ownership_barrier_index
                   ) != sync.ownership_transfer_barriers.end();
        }
    );
    suite.Check(
        transfer_sync != lowered.queue_syncs.end() && transfer_sync->signal_pass == producer &&
            transfer_sync->wait_pass == consumer && transfer_sync->signal_queue == topology.graphics &&
            transfer_sync->wait_queue == topology.compute,
        test_name,
        "release owner must signal the destination acquire through its correlated GPU sync"
    );
}

void TestCrossFamilyOwnershipFanInUsesOneReleaseOwner(TestSuite& suite) {
    constexpr std::string_view       test_name = "cross-family ownership fan-in uses one release owner";
    const RenderGraph::QueueTopology topology{
        .graphics = {RenderGraph::QueueRole::Graphics, 0, 0},
        .compute  = {RenderGraph::QueueRole::Compute, 1, 0},
        .copy     = {RenderGraph::QueueRole::Copy, 2, 1},
    };
    BufferRef   buffer = MoerNew(FakeBuffer)(256);
    RenderGraph graph("CrossFamilyOwnershipFanIn", topology);
    const auto  data = graph.ImportBuffer(
        "Exclusive",
        buffer,
        RenderGraph::BufferDesc{
             .byte_size    = 256,
             .sharing_mode = RenderGraph::SharingMode::Exclusive,
        }
    );
    graph.SetInitialState(
        data, RenderGraph::BufferBoundary::Undefined, RenderGraph::QueueRole::None, RenderGraph::AccessMode::None
    );
    graph.AddRecordPass(
        "GraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto graphics_reader = graph.AddRecordPass(
        "GraphicsRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferAccessType::ShaderRead).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_reader = graph.AddRecordPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute)
                .Read(data, RenderGraph::BufferAccessType::ShaderRead)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto copy_writer = graph.AddRecordPass(
        "CopyWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::GpuWorkType::Transfer)
                .Write(data, RenderGraph::BufferAccessType::TransferDestination)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::TransferDestination,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Write
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& compiled          = graph.GetCompiledPlan();
    const auto  ownership_barrier = std::find_if(
        compiled.barriers.begin(),
        compiled.barriers.end(),
        [&](const RenderGraph::CompiledBarrier& barrier) {
            return barrier.resource == data.Untyped() && barrier.dst_pass == copy_writer &&
                   barrier.queue_ownership &&
                   std::find_if(
                       barrier.sources.begin(),
                       barrier.sources.end(),
                       [&](const RenderGraph::CompiledBarrierSource& source) {
                           return source.pass == graphics_reader;
                       }
                   ) != barrier.sources.end() &&
                   std::find_if(
                       barrier.sources.begin(),
                       barrier.sources.end(),
                       [&](const RenderGraph::CompiledBarrierSource& source) {
                           return source.pass == compute_reader;
                       }
                   ) != barrier.sources.end();
        }
    );
    suite.Check(
        ownership_barrier != compiled.barriers.end(),
        test_name,
        "compiler did not retain both source-family native queue readers"
    );
    if (ownership_barrier == compiled.barriers.end()) {
        return;
    }
    const Moer::uint32 ownership_barrier_index =
        static_cast<Moer::uint32>(ownership_barrier - compiled.barriers.begin());

    RenderGraphLowering::LoweredPlan lowered{};
    std::string                      error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);

    Moer::uint32 release_count = 0;
    for (const auto& pass : lowered.passes) {
        release_count += static_cast<Moer::uint32>(std::count_if(
            pass.after.begin(),
            pass.after.end(),
            [&](const RenderGraphLowering::LoweredInstruction& instruction) {
                return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueRelease &&
                       instruction.correlation_id == ownership_barrier_index;
            }
        ));
    }
    const auto after_compute   = lowered.After(compute_reader);
    const auto compute_release = std::find_if(
        after_compute.begin(),
        after_compute.end(),
        [&](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueRelease &&
                   instruction.correlation_id == ownership_barrier_index;
        }
    );
    const auto after_graphics       = lowered.After(graphics_reader);
    const bool graphics_has_release = std::any_of(
        after_graphics.begin(),
        after_graphics.end(),
        [&](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueRelease &&
                   instruction.correlation_id == ownership_barrier_index;
        }
    );
    suite.Check(
        release_count == 1 && compute_release != after_compute.end() && !graphics_has_release &&
            compute_release->transfer_source == topology.compute &&
            compute_release->transfer_destination == topology.copy,
        test_name,
        "the maximum source batch must be the unique Compute release owner"
    );

    const auto before_copy  = lowered.Before(copy_writer);
    const auto copy_acquire = std::find_if(
        before_copy.begin(),
        before_copy.end(),
        [&](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueAcquire &&
                   instruction.correlation_id == ownership_barrier_index;
        }
    );
    suite.Check(
        copy_acquire != before_copy.end() && copy_acquire->transfer_source == topology.compute &&
            copy_acquire->transfer_destination == topology.copy,
        test_name,
        "the Copy destination must acquire from the designated Compute release owner"
    );

    const auto ownership_join = std::find_if(
        lowered.queue_syncs.begin(),
        lowered.queue_syncs.end(),
        [&](const RenderGraphLowering::QueueSyncInstruction& sync) {
            return sync.signal_queue == topology.graphics && sync.wait_queue == topology.compute &&
                   std::find(
                       sync.ownership_join_barriers.begin(),
                       sync.ownership_join_barriers.end(),
                       ownership_barrier_index
                   ) != sync.ownership_join_barriers.end();
        }
    );
    suite.Check(
        ownership_join != lowered.queue_syncs.end(),
        test_name,
        "the other source native queue must join the designated release batch"
    );

    const auto transfer_sync = std::find_if(
        lowered.queue_syncs.begin(),
        lowered.queue_syncs.end(),
        [&](const RenderGraphLowering::QueueSyncInstruction& sync) {
            return sync.signal_queue == topology.compute &&
                   sync.wait_queue == topology.copy &&
                   std::find(
                       sync.ownership_transfer_barriers.begin(),
                       sync.ownership_transfer_barriers.end(),
                       ownership_barrier_index
                   ) != sync.ownership_transfer_barriers.end();
        }
    );
    suite.Check(
        transfer_sync != lowered.queue_syncs.end(),
        test_name,
        "the unique release owner must signal the Copy acquire"
    );
}

void TestMissingCrossNativeSyncFailsClosed(TestSuite& suite) {
    constexpr std::string_view test_name =
        "missing cross-native queue sync fails closed";
    RenderGraph graph(
        "MissingCrossNativeSync",
        RenderGraph::QueueTopology::DedicatedQueues()
    );
    const auto token = graph.CreateTransientToken("GraphicsToCompute");
    graph.AddRecordPass(
        "Graphics",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(token).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "Compute",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::GpuWorkType::Compute
                   )
                .Read(token)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    auto& plan = const_cast<RenderGraph::CompiledPlan&>(
        graph.GetCompiledPlan()
    );
    plan.queue_syncs.clear();
    for (auto& batch : plan.queue_batches) {
        batch.signal_syncs.clear();
        batch.wait_syncs.clear();
    }

    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(
        !RenderGraphLowering::Lower(graph, lowered, error) &&
            Contains(
                error,
                "cross-native dependency edge is not correlated"
            ) &&
            lowered.prologue.empty() && lowered.passes.empty() &&
            lowered.queue_syncs.empty(),
        test_name,
        error
    );
}

void TestUnavailableQueueFailsAtCompile(TestSuite& suite) {
    constexpr std::string_view test_name =
        "unavailable logical queue fails at compile";
    auto topology = RenderGraph::QueueTopology::SingleQueue();
    topology.compute.available = false;
    RenderGraph graph("UnavailableCompute", topology);
    const auto token = graph.CreateTransientToken("ComputeOnly");
    graph.AddRecordPass(
        "Compute",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::GpuWorkType::Compute
                   )
                .Write(token)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !graph.Compile() &&
            Contains(graph.GetCompileError(), "unavailable logical queue"),
        test_name,
        graph.GetCompileError()
    );
}

void ExpectLowerFailure(
    TestSuite&       suite,
    RenderGraph&     graph,
    std::string_view test_name,
    std::string_view expected
) {
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    const bool result = RenderGraphLowering::Lower(graph, lowered, error);
    suite.Check(!result, test_name, "lowering unexpectedly succeeded");
    suite.Check(Contains(error, expected), test_name, error);
    suite.Check(
        lowered.prologue.empty() && lowered.passes.empty() &&
            lowered.queue_syncs.empty(),
        test_name,
        "failed lowering must not expose a partial plan"
    );
}

void TestFailClosedContracts(TestSuite& suite) {
    {
        constexpr std::string_view name = "raw physical binding rejected";
        TextureRef texture = MoerNew(FakeTexture)(1, 1, ETextureAspectFlags::COLOR);
        RenderGraph graph(name);
        const auto handle = graph.ImportTexture(
            "Raw",
            static_cast<const void*>(texture.Get()),
            RenderGraph::TextureDesc{}
        );
        graph.SetInitialState(
            handle,
            RenderGraph::TextureBoundary::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(handle, RenderGraph::TextureAccessType::ColorAttachment).SideEffect();
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            handle,
            RenderGraph::TextureAccessType::Storage,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "strong physical binding");
    }
    {
        constexpr std::string_view name = "transient physical resource rejected";
        RenderGraph graph(name);
        const auto handle =
            graph.CreateTransientBuffer("Transient", RenderGraph::BufferDesc{.byte_size = 64});
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::GpuWorkType::Transfer
                )
                    .Write(handle, RenderGraph::BufferAccessType::TransferDestination)
                    .SideEffect();
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            handle,
            RenderGraph::BufferAccessType::TransferSource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "transient physical resource");
    }
    {
        constexpr std::string_view name   = "unspecified access rejected";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph(name);
        const auto handle =
            graph.ImportBuffer("Buffer", buffer, RenderGraph::BufferDesc{.byte_size = 64});
        graph.SetInitialState(
            handle,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        graph.AddRecordPass(
            "Read",
            [=](RenderGraph::PassBuilder& builder) { builder.Read(handle).SideEffect(); },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            handle,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "state plan is incomplete");
    }
    {
        constexpr std::string_view name = "unknown boundary access rejected";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph(name);
        const auto handle =
            graph.ImportBuffer("Buffer", buffer, RenderGraph::BufferDesc{.byte_size = 64});
        graph.SetInitialState(
            handle,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Unknown
        );
        graph.AddRecordPass(
            "Read",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Read(handle, RenderGraph::BufferAccessType::ShaderRead).SideEffect();
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            handle,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "Unknown boundary access");
    }
    {
        constexpr std::string_view name = "incomplete state plan rejected";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph(name);
        const auto handle =
            graph.ImportBuffer("Buffer", buffer, RenderGraph::BufferDesc{.byte_size = 64});
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(handle, RenderGraph::BufferAccessType::Storage).SideEffect();
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            handle,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "state plan is incomplete");
    }
    {
        constexpr std::string_view name = "missing final boundary rejected";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph(name);
        const auto handle =
            graph.ImportBuffer("Buffer", buffer, RenderGraph::BufferDesc{.byte_size = 64});
        graph.SetInitialState(
            handle,
            RenderGraph::BufferBoundary::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(handle, RenderGraph::BufferAccessType::Storage).SideEffect();
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        ExpectLowerFailure(suite, graph, name, "explicit final state");
    }
}

} // namespace

int main() {
    TestSuite suite;
    TestKeepaliveCombinesCellsAndReferences(suite);
    TestReverseDependencyUsesCompiledOrder(suite);
    TestTextureFanInAndDeterminism(suite);
    TestShaderResourceAndSampledLayoutsRemainDistinct(suite);
    TestDepthAttachmentReadUsesBackendAttachmentLayout(suite);
    TestPresentationSourceLowersToCommonTransferRead(suite);
    TestUntouchedImportedRangesNeedNoCommands(suite);
    TestAccelerationBuildInputScopes(suite);
    TestSameStateReadNeedsNoBarrier(suite);
    TestCrossNativeTokenSynchronization(suite);
    TestSameNativeTokenSynchronizationNeedsNoGpuSync(suite);
    TestSameNativePhysicalBarrierRetainsSourceScope(suite);
    TestSameFamilyQueueBarrierLowersToAcquire(suite);
    TestMixedQueueFanInRetainsLocalSourceScope(suite);
    TestCrossFamilyExclusiveOwnershipLowersPairedTransfer(suite);
    TestCrossFamilyOwnershipFanInUsesOneReleaseOwner(suite);
    TestMissingCrossNativeSyncFailsClosed(suite);
    TestUnavailableQueueFailsAtCompile(suite);
    TestFailClosedContracts(suite);

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraphLowering: " << suite.FailureCount() << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraphLowering: all checks passed\n";
    return EXIT_SUCCESS;
}
