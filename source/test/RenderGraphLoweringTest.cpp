#include "rendergraph/RenderGraph.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rhi/RHIResource.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using Moer::Render::Buffer;
using Moer::Render::BufferInfo;
using Moer::Render::BufferRef;
using Moer::Render::RenderGraph;
using Moer::Render::RenderGraphLowering;
using Moer::Render::Texture;
using Moer::Render::TextureInfo;
using Moer::Render::TextureRef;

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
    FakeTexture(uint8_t mips, uint16_t layers, ETextureAspectFlags aspects) :
        Texture(MakeInfo(mips, layers, aspects)) {}

    Moer::uint GetMipByteSize(Moer::uint) const override {
        return 4;
    }

    void SetName(std::string_view) override {}

private:
    static TextureInfo
    MakeInfo(uint8_t mips, uint16_t layers, ETextureAspectFlags aspects) {
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
    explicit FakeBuffer(uint64_t byte_size) :
        Buffer(BufferInfo{
            byte_size,
            1,
            EBufferUsageFlags::UNORDERED_ACCESS |
                EBufferUsageFlags::TRANSFER_SRC |
                EBufferUsageFlags::TRANSFER_DST
        }) {}

    void SetName(std::string_view) override {}
};

template<typename Enum>
[[nodiscard]] bool HasFlag(Enum value, Enum flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
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
        RenderGraph::TextureState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        range
    );
    const auto writer = graph.AddPass(
        "Writer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(color, RenderGraph::TextureState::RenderTarget, range).SideEffect();
        },
        [] {}
    );
    const auto graphics_reader = graph.AddPass(
        "GraphicsReader",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(color, RenderGraph::TextureState::ShaderResource, range).SideEffect();
        },
        [] {}
    );
    const auto compute_reader = graph.AddPass(
        "ComputeReader",
        [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Compute
                   )
                .Read(color, RenderGraph::TextureState::ShaderResource, range)
                .SideEffect();
        },
        [] {}
    );
    const auto uav_writer = graph.AddPass(
        "UavWriter",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Compute
                   )
                .Write(color, RenderGraph::TextureState::UnorderedAccess, range)
                .SideEffect();
        },
        [] {}
    );
    graph.Export(
        color,
        RenderGraph::TextureState::ShaderResource,
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
            first.prologue.front().range.texture.mip_count == 1 &&
            first.prologue.front().texture_aspects == ETextureAspectFlags::COLOR,
        test_name,
        "prologue must retain discard, normalized range, aspect and layouts"
    );

    const auto before_uav = first.Before(uav_writer);
    const auto fan_in = std::find_if(
        before_uav.begin(),
        before_uav.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.after_state ==
                       RenderGraph::ResourceState::Texture(
                           RenderGraph::TextureState::UnorderedAccess
                       );
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
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.AddPass(
        "StorageRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(color, RenderGraph::TextureState::ShaderResource).SideEffect();
        },
        [] {}
    );
    const auto sampled_read = graph.AddPass(
        "SampledRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(color, RenderGraph::TextureState::Sampled).SideEffect();
        },
        [] {}
    );
    graph.Export(
        color,
        RenderGraph::TextureState::Sampled,
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
        RenderGraph::TextureState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddPass(
        "DepthWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(depth, RenderGraph::TextureState::DepthStencilWrite).SideEffect();
        },
        [] {}
    );
    const auto depth_read = graph.AddPass(
        "DepthRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(depth, RenderGraph::TextureState::DepthStencilRead).SideEffect();
        },
        [] {}
    );
    graph.Export(
        depth,
        RenderGraph::TextureState::DepthStencilRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);

    const auto before_read = lowered.Before(depth_read);
    const auto transition = std::find_if(
        before_read.begin(),
        before_read.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.after_state ==
                       RenderGraph::ResourceState::Texture(
                           RenderGraph::TextureState::DepthStencilRead
                       );
        }
    );
    suite.Check(
        transition != before_read.end() &&
            transition->destination.layout ==
                ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE &&
            transition->destination.access == ERHIAccessFlags::DEPTH_STENCIL_READ,
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
        RenderGraph::TextureState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    const auto writer = graph.AddPass(
        "Writer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(color, RenderGraph::TextureState::RenderTarget)
                .SideEffect();
        },
        [] {}
    );
    graph.Export(
        color,
        RenderGraph::TextureState::PresentationSource,
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
    const auto transition = std::find_if(
        after_writer.begin(),
        after_writer.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.export_boundary &&
                   instruction.after_state ==
                       RenderGraph::ResourceState::Texture(
                           RenderGraph::TextureState::PresentationSource
                       );
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
        RenderGraph::TextureState::PresentationSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    rejected_graph.AddPass(
        "ReadImportedColor",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(
                    rejected_color,
                    RenderGraph::TextureState::ShaderResource
                )
                .SideEffect();
        },
        [] {}
    );
    rejected_graph.Export(
        rejected_color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(
        !rejected_graph.Compile() &&
            Contains(
                rejected_graph.GetCompileError(),
                "export-boundary-only"
            ),
        test_name,
        "PresentationSource must be rejected as an import boundary before lowering"
    );
    RenderGraphLowering::LoweredPlan rejected_lowered{};
    std::string rejected_error{};
    suite.Check(
        !RenderGraphLowering::Lower(
            rejected_graph,
            rejected_lowered,
            rejected_error
        ) &&
            rejected_lowered.prologue.empty() &&
            rejected_lowered.passes.empty() &&
            rejected_lowered.queue_syncs.empty() &&
            Contains(rejected_error, "compiled before lowering"),
        test_name,
        "a graph with a PresentationSource import boundary reached lowering"
    );
}

void TestSameStateReadGetsStateSeed(TestSuite& suite) {
    constexpr std::string_view test_name = "same-state read receives explicit state seed";
    BufferRef buffer = MoerNew(FakeBuffer)(256);

    RenderGraph graph("ReadSeed");
    const auto data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 256}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto first_read = graph.AddPass(
        "FirstRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferState::ShaderResource).SideEffect();
        },
        [] {}
    );
    const auto second_read = graph.AddPass(
        "SecondRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferState::ShaderResource).SideEffect();
        },
        [] {}
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
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
    const auto before_second = lowered.Before(second_read);
    const auto seed = std::find_if(
        before_second.begin(),
        before_second.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                   RenderGraphLowering::InstructionKind::StateSeed;
        }
    );
    suite.Check(
        seed != before_second.end() &&
            seed->barrier_index == RenderGraphLowering::InvalidBarrierIndex &&
            seed->access_index != RenderGraphLowering::InvalidAccessIndex &&
            seed->range.buffer.offset == 0 &&
            seed->range.buffer.size == 256 &&
            seed->source.stages == ERHIPipelineStageFlags::PS_ALL_GRAPHICS &&
            seed->source.access == ERHIAccessFlags::SHADER_READ &&
            seed->source.stages == seed->destination.stages &&
            seed->source.access == seed->destination.access,
        test_name,
        "a barrier-free read must seed the independent CommandList tracker"
    );
    suite.Check(
        lowered.Keepalive(second_read).size() == 1 &&
            lowered.Keepalive(second_read).front().buffer.Get() == buffer.Get(),
        test_name,
        "state-seeded pass must retain its buffer even without a compiler barrier"
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
    const auto producer = graph.AddPass(
        "GraphicsProducer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Graphics
                   )
                .Write(token)
                .SideEffect();
        },
        [] {}
    );
    const auto consumer = graph.AddPass(
        "ComputeConsumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Compute
                   )
                .Read(token)
                .SideEffect();
        },
        [] {}
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
    graph.AddPass(
        "GraphicsProducer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Graphics
                   )
                .Write(token)
                .SideEffect();
        },
        [] {}
    );
    graph.AddPass(
        "ComputeConsumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Compute
                   )
                .Read(token)
                .SideEffect();
        },
        [] {}
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddPass(
        "GraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess).SideEffect();
        },
        [] {}
    );
    const auto consumer = graph.AddPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Compute
                   )
                .Read(data, RenderGraph::BufferState::ShaderResource)
                .SideEffect();
        },
        [] {}
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);
    const auto before_consumer = lowered.Before(consumer);
    const auto barrier = std::find_if(
        before_consumer.begin(),
        before_consumer.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.instruction_kind ==
                       RenderGraphLowering::InstructionKind::Barrier &&
                   instruction.after_state ==
                       RenderGraph::ResourceState::Buffer(
                           RenderGraph::BufferState::ShaderResource
                       );
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
            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Exclusive,
        }
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddPass(
        "GraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess).SideEffect();
        },
        [] {}
    );
    const auto consumer = graph.AddPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Compute
                   )
                .Read(data, RenderGraph::BufferState::ShaderResource)
                .SideEffect();
        },
        [] {}
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
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
            acquire->destination.access == ERHIAccessFlags::SHADER_READ,
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddPass(
        "InitialGraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess).SideEffect();
        },
        [] {}
    );
    const auto graphics_reader = graph.AddPass(
        "GraphicsRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferState::ShaderResource).SideEffect();
        },
        [] {}
    );
    const auto compute_reader = graph.AddPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Compute
                   )
                .Read(data, RenderGraph::BufferState::ShaderResource)
                .SideEffect();
        },
        [] {}
    );
    const auto final_writer = graph.AddPass(
        "FinalGraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess).SideEffect();
        },
        [] {}
    );
    graph.Export(
        data,
        RenderGraph::BufferState::UnorderedAccess,
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
            barrier->source.access == ERHIAccessFlags::SHADER_READ,
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
                            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Exclusive,
        }
    );
    graph.SetInitialState(
        data,
        RenderGraph::TextureState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    const auto producer = graph.AddPass(
        "GraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::TextureState::UnorderedAccess).SideEffect();
        },
        [] {}
    );
    const auto consumer = graph.AddPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute)
                .Read(data, RenderGraph::TextureState::Sampled)
                .SideEffect();
        },
        [] {}
    );
    graph.Export(
        data,
        RenderGraph::TextureState::Sampled,
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
    const uint32_t ownership_barrier_index =
        static_cast<uint32_t>(ownership_barrier - compiled.barriers.begin());

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
    uint32_t release_count = 0;
    uint32_t acquire_count = 0;
    for (const auto& pass : lowered.passes) {
        release_count += static_cast<uint32_t>(std::count_if(
            pass.after.begin(),
            pass.after.end(),
            [&](const RenderGraphLowering::LoweredInstruction& instruction) {
                return instruction.instruction_kind == RenderGraphLowering::InstructionKind::QueueRelease &&
                       instruction.correlation_id == ownership_barrier_index;
            }
        ));
        acquire_count += static_cast<uint32_t>(std::count_if(
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
             .sharing_mode = RenderGraph::TextureDesc::SharingMode::Exclusive,
        }
    );
    graph.SetInitialState(
        data, RenderGraph::BufferState::Undefined, RenderGraph::QueueRole::None, RenderGraph::AccessMode::None
    );
    graph.AddPass(
        "GraphicsWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess).SideEffect();
        },
        [] {}
    );
    const auto graphics_reader = graph.AddPass(
        "GraphicsRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferState::ShaderResource).SideEffect();
        },
        [] {}
    );
    const auto compute_reader = graph.AddPass(
        "ComputeRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute)
                .Read(data, RenderGraph::BufferState::ShaderResource)
                .SideEffect();
        },
        [] {}
    );
    const auto copy_writer = graph.AddPass(
        "CopyWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy)
                .Write(data, RenderGraph::BufferState::TransferDestination)
                .SideEffect();
        },
        [] {}
    );
    graph.Export(
        data,
        RenderGraph::BufferState::TransferDestination,
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
    const uint32_t ownership_barrier_index =
        static_cast<uint32_t>(ownership_barrier - compiled.barriers.begin());

    RenderGraphLowering::LoweredPlan lowered{};
    std::string                      error{};
    suite.Check(RenderGraphLowering::Lower(graph, lowered, error), test_name, error);

    uint32_t release_count = 0;
    for (const auto& pass : lowered.passes) {
        release_count += static_cast<uint32_t>(std::count_if(
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
    graph.AddPass(
        "Graphics",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(token).SideEffect();
        },
        [] {}
    );
    graph.AddPass(
        "Compute",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Compute
                   )
                .Read(token)
                .SideEffect();
        },
        [] {}
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
    graph.AddPass(
        "Compute",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Compute
                   )
                .Write(token)
                .SideEffect();
        },
        [] {}
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
            RenderGraph::TextureState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(handle, RenderGraph::TextureState::RenderTarget).SideEffect();
            },
            [] {}
        );
        graph.Export(
            handle,
            RenderGraph::TextureState::ShaderResource,
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
        graph.AddPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Copy
                       )
                    .Write(handle, RenderGraph::BufferState::TransferDestination)
                    .SideEffect();
            },
            [] {}
        );
        graph.Export(
            handle,
            RenderGraph::BufferState::TransferSource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "transient physical resource");
    }
    {
        constexpr std::string_view name = "automatic access rejected";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph(name);
        const auto handle =
            graph.ImportBuffer("Buffer", buffer, RenderGraph::BufferDesc{.byte_size = 64});
        graph.SetInitialState(
            handle,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        graph.AddPass(
            "Read",
            [=](RenderGraph::PassBuilder& builder) { builder.Read(handle).SideEffect(); },
            [] {}
        );
        graph.Export(
            handle,
            RenderGraph::BufferState::ShaderResource,
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
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Unknown
        );
        graph.AddPass(
            "Read",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Read(handle, RenderGraph::BufferState::ShaderResource).SideEffect();
            },
            [] {}
        );
        graph.Export(
            handle,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "Unknown boundary access");
    }
    {
        constexpr std::string_view name = "present boundary rejected";
        TextureRef texture = MoerNew(FakeTexture)(1, 1, ETextureAspectFlags::COLOR);
        RenderGraph graph(name);
        const auto handle =
            graph.ImportTexture("Color", texture, RenderGraph::TextureDesc{});
        graph.SetInitialState(
            handle,
            RenderGraph::TextureState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(handle, RenderGraph::TextureState::RenderTarget).SideEffect();
            },
            [] {}
        );
        graph.Export(
            handle,
            RenderGraph::TextureState::Present,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "Present boundary");
    }
    {
        constexpr std::string_view name = "external control rejected";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph(name);
        const auto handle =
            graph.ImportBuffer("Buffer", buffer, RenderGraph::BufferDesc{.byte_size = 64});
        graph.SetInitialState(
            handle,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddPass(
            "External",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(handle, RenderGraph::BufferState::UnorderedAccess)
                    .ExternalControl()
                    .SideEffect();
            },
            [] {}
        );
        graph.Export(
            handle,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "ExternalControl");
    }
    {
        constexpr std::string_view name = "incomplete state plan rejected";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph(name);
        const auto handle =
            graph.ImportBuffer("Buffer", buffer, RenderGraph::BufferDesc{.byte_size = 64});
        graph.AddPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(handle, RenderGraph::BufferState::UnorderedAccess).SideEffect();
            },
            [] {}
        );
        graph.Export(
            handle,
            RenderGraph::BufferState::ShaderResource,
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
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(handle, RenderGraph::BufferState::UnorderedAccess).SideEffect();
            },
            [] {}
        );
        ExpectLowerFailure(suite, graph, name, "explicit final state");
    }
}

} // namespace

int main() {
    TestSuite suite;
    TestTextureFanInAndDeterminism(suite);
    TestShaderResourceAndSampledLayoutsRemainDistinct(suite);
    TestDepthAttachmentReadUsesBackendAttachmentLayout(suite);
    TestPresentationSourceLowersToCommonTransferRead(suite);
    TestSameStateReadGetsStateSeed(suite);
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
