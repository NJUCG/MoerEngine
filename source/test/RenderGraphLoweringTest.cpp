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
) {
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan lowered{};
    std::string error{};
    const bool result = RenderGraphLowering::Lower(graph, lowered, error);
    suite.Check(!result, test_name, "lowering unexpectedly succeeded");
    suite.Check(Contains(error, expected), test_name, error);
    suite.Check(
        lowered.prologue.empty() && lowered.passes.empty(),
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
        constexpr std::string_view name = "non-graphics logical queue rejected";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph(name, RenderGraph::QueueTopology::SingleQueue());
        const auto handle =
            graph.ImportBuffer("Buffer", buffer, RenderGraph::BufferDesc{.byte_size = 64});
        graph.SetInitialState(
            handle,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddPass(
            "Compute",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Compute,
                           RenderGraph::PipelineType::Compute
                       )
                    .Write(handle, RenderGraph::BufferState::UnorderedAccess)
                    .SideEffect();
            },
            [] {}
        );
        graph.Export(
            handle,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Compute,
            RenderGraph::AccessMode::Read
        );
        ExpectLowerFailure(suite, graph, name, "Graphics logical queue");
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
    TestSameStateReadGetsStateSeed(suite);
    TestFailClosedContracts(suite);

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraphLowering: " << suite.FailureCount() << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraphLowering: all checks passed\n";
    return EXIT_SUCCESS;
}
