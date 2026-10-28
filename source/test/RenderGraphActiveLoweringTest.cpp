#include "rendergraph/RenderGraph.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIResource.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using Moer::Render::BarrierCmd;
using Moer::Render::Buffer;
using Moer::Render::BufferInfo;
using Moer::Render::BufferRef;
using Moer::Render::CmdSubmit;
using Moer::Render::Command;
using Moer::Render::CommandList;
using Moer::Render::EQueueType;
using Moer::Render::ERHIRecordingStatus;
using Moer::Render::ERHIResourceStateOwnership;
using Moer::Render::ERHISyncDepth;
using Moer::Render::RHIExecutor;
using Moer::Render::RHIRecordingGate;
using Moer::Render::RHIRecordingGateView;
using Moer::Render::RHIRecordingSource;
using Moer::Render::RenderGraph;
using Moer::Render::Texture;
using Moer::Render::TextureInfo;
using Moer::Render::TextureRef;
using Moer::MakeUnique;

template<typename T>
concept WritableRecordingGate = requires(T& gate) {
    gate.Signal();
    gate.Fail();
};

static_assert(!WritableRecordingGate<RHIRecordingGateView>);
static_assert(
    !std::is_convertible_v<
        RHIRecordingGateView,
        Moer::Render::RHIRecordingGateRef
    >
);

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
    FakeTexture(
        uint8_t mips, uint16_t layers, ETextureAspectFlags aspects,
        std::shared_ptr<bool> destroyed = {},
        ETextureUsageFlags extra_usage = ETextureUsageFlags::UNDEFINED
    ) : Texture(MakeInfo(mips, layers, aspects, extra_usage)), destroyed(std::move(destroyed)) {}

    ~FakeTexture() override {
        if (destroyed) { *destroyed = true; }
    }

    Moer::uint GetMipByteSize(Moer::uint) const override {
        return 4;
    }

    void SetName(std::string_view) override {}

private:
    static TextureInfo
    MakeInfo(uint8_t mips, uint16_t layers, ETextureAspectFlags aspects, ETextureUsageFlags extra_usage) {
        TextureInfo info{};
        info.usage = ETextureUsageFlags::SAMPLED |
                     ETextureUsageFlags::UNORDERED_ACCESS |
                     ETextureUsageFlags::COLOR_ATTACHMENT |
                     ETextureUsageFlags::TRANSFER_SRC |
                     ETextureUsageFlags::TRANSFER_DST | extra_usage;
        info.extent       = {64, 64};
        info.num_mips     = mips;
        info.array_size   = layers;
        info.aspect_flags = aspects;
        return info;
    }

    std::shared_ptr<bool> destroyed;
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

class LifetimeProbeBuffer final : public Buffer {
public:
    explicit LifetimeProbeBuffer(std::shared_ptr<bool> destroyed) :
        Buffer(BufferInfo{
            64,
            1,
            EBufferUsageFlags::UNORDERED_ACCESS |
                EBufferUsageFlags::TRANSFER_SRC |
                EBufferUsageFlags::TRANSFER_DST
        }),
        destroyed(std::move(destroyed)) {}

    ~LifetimeProbeBuffer() override {
        *destroyed = true;
    }

    void SetName(std::string_view) override {}

private:
    std::shared_ptr<bool> destroyed;
};

class ProbeCommand final : public Command {
public:
    explicit ProbeCommand(int marker) : Command(EType::Custom), marker(marker) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

    [[nodiscard]] int Marker() const {
        return marker;
    }

private:
    int marker;
};

[[nodiscard]] const BarrierCmd* AsBarrier(const Moer::UniquePtr<Command>& command) {
    if (!command || command->Type() != Command::EType::Barrier) {
        return nullptr;
    }
    return static_cast<const BarrierCmd*>(command.get());
}

[[nodiscard]] const ProbeCommand* AsProbe(const Moer::UniquePtr<Command>& command) {
    if (!command || command->Type() != Command::EType::Custom) {
        return nullptr;
    }
    return static_cast<const ProbeCommand*>(command.get());
}

void TestRecordPassMaterializesExplicitBeforeBodyAfter(TestSuite& suite, bool merge = false) {
    constexpr std::string_view test_name =
        "record pass materializes explicit before-body-after";

    BufferRef  buffer  = MoerNew(FakeBuffer)(256);
    TextureRef texture = MoerNew(FakeTexture)(1, 1, ETextureAspectFlags::COLOR);

    RenderGraph graph("ActiveRecordMaterialization");
    const auto  data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 256}
    );
    const auto color = graph.ImportTexture(
        "Color",
        texture,
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.SetInitialState(
        color,
        RenderGraph::TextureBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddRecordPass(
        "WriteBoth",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage)
                .Write(color, RenderGraph::TextureAccessType::ColorAttachment)
                .SideEffect();
        },
        [](CommandList& commands) {
            commands.AddCustomCommand(
                MakeUnique<ProbeCommand>(101),
                "ActiveLoweringBody"
            );
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        color,
        RenderGraph::TextureAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    Moer::Array<RHIRecordingSource> published{};
    CommandList merged;
    merged.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
    bool executed = false;
    if (merge) {
        executed = graph.RecordAndMergeFrontendCommands(merged, false);
    } else {
        executed = graph.ExecuteFrontendRecordingPlan(
            {},
            false,
            [&](Moer::Array<RHIRecordingSource>&& sources) {
                for (auto& source : sources) { published.emplace_back(std::move(source)); }
            },
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        );
    }
    suite.Check(executed, test_name, graph.GetCompileError());
    if (!executed) { return; }
    if (!merge) {
        suite.Check(published.size() == 1 &&
                        published.front().completion.Status() == ERHIRecordingStatus::Succeeded,
                    test_name, "one completed recording source must be published");
        if (published.size() != 1) { return; }
    }
    CmdSubmit submit = merge ? merged.Submit() : published.front().command_list->Submit();
    if (merge) {
        suite.Check(submit.resource_state_ownership == ERHIResourceStateOwnership::Compiled,
                    test_name, "merged submission must preserve Compiled ownership");
    }
    suite.Check(
        submit.HasExplicitResourceStateOwnership(),
        test_name,
        "the sealed recording submit must preserve Explicit ownership"
    );
    suite.Check(
        submit.cmds.size() == 3,
        test_name,
        "the command stream must contain one before barrier, body command, and after barrier"
    );
    if (submit.cmds.size() != 3) {
        return;
    }

    const auto* before = AsBarrier(submit.cmds[0]);
    const auto* body   = AsProbe(submit.cmds[1]);
    const auto* after  = AsBarrier(submit.cmds[2]);
    suite.Check(
        before != nullptr && body != nullptr && body->Marker() == 101 && after != nullptr,
        test_name,
        "explicit barriers must bracket the record callback command"
    );
    if (before == nullptr || after == nullptr) {
        return;
    }
    suite.Check(
        before->ExplicitBuffers().size() == 1 &&
            before->ExplicitTextures().size() == 1 &&
            after->ExplicitBuffers().size() == 1 &&
            after->ExplicitTextures().size() == 1,
        test_name,
        "both strong physical imports must be materialized at graph boundaries"
    );
    suite.Check(
        before->GetSrcQueue() == EQueueType::Graphics &&
            before->GetDstQueue() == EQueueType::Graphics &&
            after->GetSrcQueue() == EQueueType::Graphics &&
            after->GetDstQueue() == EQueueType::Graphics &&
            !before->IsQueueTransition() && !after->IsQueueTransition(),
        test_name,
        "Phase 11 barriers must remain Graphics-local"
    );
    if (before->ExplicitBuffers().size() == 1 &&
        before->ExplicitTextures().size() == 1 &&
        after->ExplicitBuffers().size() == 1 &&
        after->ExplicitTextures().size() == 1) {
        suite.Check(
            before->ExplicitBuffers().front().handle ==
                    reinterpret_cast<Moer::uint64>(buffer.Get()) &&
                before->ExplicitTextures().front().handle ==
                    reinterpret_cast<Moer::uint64>(texture.Get()) &&
                after->ExplicitBuffers().front().handle ==
                    reinterpret_cast<Moer::uint64>(buffer.Get()) &&
                after->ExplicitTextures().front().handle ==
                    reinterpret_cast<Moer::uint64>(texture.Get()),
            test_name,
            "materialized barriers must retain the exact imported physical identities"
        );
    }
}

void TestReadOnlyBoundariesNeedNoCommands(TestSuite& suite, bool merge, bool partial) {
    const std::string test_name = std::string("read-only boundaries: ") +
        (merge ? "merged " : "active ") + (partial ? "partial ranges" : "whole resources");
    auto buffer_destroyed = std::make_shared<bool>(false);
    auto texture_destroyed = std::make_shared<bool>(false);
    Moer::Array<RHIRecordingSource> published;
    CommandList merged;
    merged.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
    {
        BufferRef buffer = MoerNew(LifetimeProbeBuffer)(buffer_destroyed);
        TextureRef texture = MoerNew(FakeTexture)(2, 2, ETextureAspectFlags::COLOR, texture_destroyed);
        RenderGraph graph(test_name);
        const auto data = graph.ImportBuffer("Data", buffer, {.byte_size = 64});
        const auto image = graph.ImportTexture("Image", texture, {.mip_count = 2, .layer_count = 2});
        const auto bytes = partial ? RenderGraph::BufferRange{.offset = 16, .size = 32} :
                                     RenderGraph::BufferRange::Whole();
        const auto slice = partial ? RenderGraph::TextureRange{
            .aspects = RenderGraph::TextureAspect::Color, .mip_first = 1, .mip_count = 1,
            .layer_first = 1, .layer_count = 1} : RenderGraph::TextureRange::Whole();
        graph.SetInitialState(data, RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read, bytes);
        graph.SetInitialState(image, RenderGraph::TextureAccessType::Sampled,
            RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read, slice);
        for (int marker : {1, 2}) {
            graph.AddRecordPass(
                marker == 1 ? "FirstRead" : "SecondRead",
                [=](RenderGraph::PassBuilder& pass) {
                    pass.Read(data, RenderGraph::BufferAccessType::ShaderRead, bytes)
                        .Read(image, RenderGraph::TextureAccessType::Sampled, slice).SideEffect();
                },
                [marker](CommandList& commands) {
                    // No resource reference in the command or callback: RDG must retain both resources.
                    commands.AddCustomCommand(MakeUnique<ProbeCommand>(marker), "ReadBody");
                }, RenderGraph::PassRecordPolicy::Serial);
        }
        graph.Export(data, RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read, bytes);
        graph.Export(image, RenderGraph::TextureAccessType::Sampled,
            RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read, slice);
        if (!graph.Compile()) {
            suite.Check(false, test_name, graph.GetCompileError());
            return;
        }
        const auto& plan = graph.GetCompiledPlan();
        suite.Check(plan.prologue_barriers.size() == 2 && plan.epilogue_barriers.size() == 2,
            test_name, "compiled import/export evidence must remain intact");
        const bool executed = merge ? graph.RecordAndMergeFrontendCommands(merged, false) :
            graph.ExecuteFrontendRecordingPlan({}, false,
                [&](Moer::Array<RHIRecordingSource>&& sources) {
                    for (auto& source : sources) { published.emplace_back(std::move(source)); }
                }, {.enabled = true});
        suite.Check(executed, test_name, graph.GetCompileError());
        if (!executed) { return; }
    }
    suite.Check(!*buffer_destroyed && !*texture_destroyed, test_name,
        "barrier-free resources must survive graph destruction");
    std::vector<CmdSubmit> submits;
    if (merge) {
        submits.emplace_back(merged.Submit());
    } else {
        suite.Check(published.size() == 1, test_name, "serial read passes must share one published stream");
        for (auto& source : published) {
            suite.Check(source.completion.Status() == ERHIRecordingStatus::Succeeded,
                test_name, "read source must complete");
            submits.emplace_back(source.command_list->Submit());
        }
    }
    published.clear();
    std::vector<int> markers;
    for (const auto& submit : submits) {
        suite.Check(submit.HasExplicitResourceStateOwnership(), test_name,
            "barrier-free submission must retain compiled ownership");
        for (const auto& command : submit.cmds) {
            suite.Check(AsBarrier(command) == nullptr, test_name,
                "unchanged read-only import/export emitted a GPU barrier");
            if (const auto* probe = AsProbe(command)) { markers.push_back(probe->Marker()); }
        }
    }
    suite.Check(markers == std::vector<int>{1, 2}, test_name, "read bodies must retain compiled order");
    suite.Check(!*buffer_destroyed && !*texture_destroyed, test_name,
        "submission must retain resources after recording sources are released");
    submits.clear();
    suite.Check(*buffer_destroyed && *texture_destroyed, test_name,
        "resources must be reclaimed after the last submission owner is released");
}

void TestSameStateWriteBoundaryNeedsBarrier(TestSuite& suite, bool write_pass) {
    const std::string test_name = write_pass ? "same-state write boundaries remain barriers" :
                                               "same-state import write remains visible to a reader";
    BufferRef buffer = MoerNew(FakeBuffer)(64);
    RenderGraph graph(test_name);
    const auto data = graph.ImportBuffer("Data", buffer, {.byte_size = 64});
    graph.SetInitialState(data, RenderGraph::BufferAccessType::Storage,
        RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Write);
    graph.AddRecordPass("UseStorage", [=](RenderGraph::PassBuilder& pass) {
        if (write_pass) { pass.Write(data, RenderGraph::BufferAccessType::Storage); }
        else { pass.Read(data, RenderGraph::BufferAccessType::Storage); }
        pass.SideEffect();
    }, [](CommandList&) {}, RenderGraph::PassRecordPolicy::Serial);
    graph.Export(data, RenderGraph::BufferAccessType::Storage,
        RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    CommandList commands;
    commands.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
    const bool recorded = graph.Compile() && graph.RecordAndMergeFrontendCommands(commands, false);
    suite.Check(recorded, test_name, graph.GetCompileError());
    if (!recorded) { return; }
    auto submit = commands.Submit();
    size_t count = 0;
    for (const auto& command : submit.cmds) {
        if (const auto* barrier = AsBarrier(command)) {
            count += barrier->ExplicitBuffers().size();
        }
    }
    suite.Check(count == (write_pass ? 2u : 1u), test_name,
        "equal storage states must not remove a write memory dependency");
}

void TestPresentationPublicationSurvivesNoOpFiltering(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name = "unchanged presentation boundary still publishes its state";
    for (const bool present_usage : {false, true}) {
        TextureRef texture = MoerNew(FakeTexture)(1, 1, ETextureAspectFlags::COLOR, nullptr,
            present_usage ? ETextureUsageFlags::PRESENTATION_SOURCE : ETextureUsageFlags::UNDEFINED);
        // A presentation texture or a Present export must still publish its state.
        RenderGraphLowering::LoweredInstruction instruction{};
        instruction.resource_kind = RenderGraph::ResourceKind::Texture;
        instruction.physical.texture = texture;
        instruction.range = RenderGraph::TextureRange{
            .aspects = RenderGraph::TextureAspect::Color, .mip_count = 1, .layer_count = 1};
        instruction.texture_aspects = ETextureAspectFlags::COLOR;
        instruction.before_state = present_usage ?
            RenderGraph::ResourceState{RenderGraph::TextureAccessType::Sampled} :
            RenderGraph::ResourceState{RenderGraph::TextureBoundary::PresentationSource};
        instruction.after_state = instruction.before_state;
        instruction.before_access = instruction.after_access = RenderGraph::AccessMode::Read;
        instruction.export_boundary = true;
        instruction.source = instruction.destination = {
            .stages = ERHIPipelineStageFlags::PS_TRANSFER,
            .access = ERHIAccessFlags::TRANSFER_READ,
            .layout = ETextureLayout::TEXTURE_LAYOUT_COMMON,
            .has_texture_layout = true};
        std::optional<BarrierCreateInfo> barrier;
        std::string error;
        suite.Check(RenderGraphLowering::MaterializeInstruction(instruction, barrier, error), test_name, error);
        suite.Check(barrier && barrier->publish_external_state, test_name,
            "Present readiness must still be published when no transition is needed");
    }
}

void TestUnrepresentableReadOnlyRangeFails(TestSuite& suite, bool merge) {
    constexpr std::string_view test_name = "read-only filtering must not bypass RHI view range validation";
    TextureRef texture = MoerNew(FakeTexture)(1, 256, ETextureAspectFlags::COLOR);
    RenderGraph graph(test_name);
    const auto image = graph.ImportTexture("Image", texture, {.mip_count = 1, .layer_count = 256});
    graph.SetInitialState(image, RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    graph.Export(image, RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    int callbacks = 0, publications = 0;
    graph.AddRecordPass("Read", [=](RenderGraph::PassBuilder& pass) {
        pass.Read(image, RenderGraph::TextureAccessType::Sampled).SideEffect();
    }, [&](CommandList&) { ++callbacks; }, RenderGraph::PassRecordPolicy::Serial);
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    CommandList commands;
    commands.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
    const bool executed = merge ? graph.RecordAndMergeFrontendCommands(commands, false) :
        graph.ExecuteFrontendRecordingPlan({}, false,
            [&](Moer::Array<RHIRecordingSource>&&) { ++publications; }, {.enabled = true});
    suite.Check(!executed && Contains(graph.GetCompileError(), "cannot be represented"),
        test_name, graph.GetCompileError());
    suite.Check(callbacks == 0 && publications == 0 && commands.IsEmpty(), test_name,
        "invalid range must fail before callbacks, publication or destination mutation");
}

void TestPartialPhysicalRangesMaterialize(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name = "compiled barriers preserve buffer ranges and texture aspects";
    BufferRef buffer = MoerNew(FakeBuffer)(128);
    TextureRef texture = MoerNew(FakeTexture)(2, 2, ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE);
    RenderGraph graph("PartialPhysicalRanges");
    const auto data = graph.ImportBuffer("Data", buffer, {.byte_size = 128});
    const auto depth = graph.ImportTexture("DepthStencil", texture,
        {.mip_count = 2, .layer_count = 2,
         .aspects = RenderGraph::TextureAspect::Depth | RenderGraph::TextureAspect::Stencil});
    const RenderGraph::BufferRange bytes{.offset = 16, .size = 32};
    const RenderGraph::TextureRange slice{.aspects = RenderGraph::TextureAspect::Depth,
        .mip_first = 1, .mip_count = 1, .layer_first = 1, .layer_count = 1};
    graph.SetInitialState(data, RenderGraph::BufferBoundary::Undefined, RenderGraph::QueueRole::None, RenderGraph::AccessMode::None, bytes);
    graph.SetInitialState(depth, RenderGraph::TextureBoundary::Undefined, RenderGraph::QueueRole::None, RenderGraph::AccessMode::None, slice);
    graph.AddRecordPass(
        "WriteSlice",
        [=](RenderGraph::PassBuilder& pass) {
            pass.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Transfer)
                .Write(data, RenderGraph::BufferAccessType::TransferDestination, bytes)
                .Write(depth, RenderGraph::TextureAccessType::TransferDestination, slice)
                .SideEffect();
        },
        [](CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(data, RenderGraph::BufferAccessType::TransferSource, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read, bytes);
    graph.Export(depth, RenderGraph::TextureAccessType::TransferSource, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read, slice);
    CommandList commands;
    commands.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
    suite.Check(graph.Compile() && graph.RecordAndMergeFrontendCommands(commands, false),
                test_name, graph.GetCompileError());
    auto submit = commands.Submit();
    size_t buffer_barriers = 0, texture_barriers = 0;
    for (const auto& command : submit.cmds) {
        const auto* barrier = AsBarrier(command);
        if (!barrier) { continue; }
        for (const auto& range : barrier->ExplicitBuffers()) {
            ++buffer_barriers;
            suite.Check(range.offset == 16 && range.byte_size == 32, test_name,
                        "materialization widened the buffer range");
        }
        for (const auto& range : barrier->ExplicitTextures()) {
            ++texture_barriers;
            suite.Check(range.mip_level == 1 && range.mip_count == 1 &&
                        range.array_layer == 1 && range.array_count == 1 &&
                        range.texture_aspects == ETextureAspectFlags::DEPTH_SLICE,
                        test_name, "materialization widened the texture aspect or subresource");
        }
    }
    suite.Check(buffer_barriers == 2 && texture_barriers == 2, test_name,
                "partial import/export boundaries were not materialized");
}

void ExpectActiveFailClosed(
    TestSuite&       suite,
    RenderGraph&     graph,
    std::string_view test_name,
    std::string_view expected_error,
    int&             record_calls,
    int&             configure_calls,
    int&             publish_calls
) {
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        [&](std::span<const RenderGraph::ExecutedPassInfo>, RHIRecordingSource&) {
            ++configure_calls;
        },
        false,
        [&](Moer::Array<RHIRecordingSource>&&) {
            ++publish_calls;
        },
        RenderGraph::ActiveRecordingOptions{.enabled = true}
    );
    suite.Check(!executed, test_name, "active execution unexpectedly succeeded");
    suite.Check(
        Contains(graph.GetCompileError(), expected_error),
        test_name,
        graph.GetCompileError()
    );
    suite.Check(
        record_calls == 0 && configure_calls == 0 && publish_calls == 0,
        test_name,
        "lowering rejection must happen before record, source setup, or publication"
    );
}

void TestUnsupportedInputsFailBeforeCallbacksOrPublish(TestSuite& suite) {
    {
        constexpr std::string_view test_name =
            "Unspecified access fails before callbacks or publication";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph("ActiveUnspecifiedFailClosed");
        const auto  data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        int record_calls    = 0;
        int configure_calls = 0;
        int publish_calls   = 0;
        graph.AddRecordPass(
            "UnspecifiedRead",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Read(data).SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            data,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectActiveFailClosed(
            suite,
            graph,
            test_name,
            "state plan is incomplete",
            record_calls,
            configure_calls,
            publish_calls
        );
    }

    {
        constexpr std::string_view test_name =
            "raw physical binding fails before callbacks or publication";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph("ActiveRawBindingFailClosed");
        const auto  data = graph.ImportBuffer(
            "Data",
            static_cast<const void*>(buffer.Get()),
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferBoundary::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int record_calls    = 0;
        int configure_calls = 0;
        int publish_calls   = 0;
        graph.AddRecordPass(
            "RawWrite",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferAccessType::Storage)
                    .SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            data,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        ExpectActiveFailClosed(
            suite,
            graph,
            test_name,
            "strong physical binding",
            record_calls,
            configure_calls,
            publish_calls
        );
    }

    {
        constexpr std::string_view test_name =
            "source metadata callback cannot record commands";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph("ActiveConfigureMutationFailClosed");
        const auto  data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferBoundary::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int record_calls    = 0;
        int configure_calls = 0;
        int publish_calls   = 0;
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferAccessType::Storage)
                    .SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            data,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            [&](std::span<const RenderGraph::ExecutedPassInfo>, RHIRecordingSource& source) {
                ++configure_calls;
                source.command_list->AddCustomCommand(
                    MakeUnique<ProbeCommand>(404),
                    "IllegalConfigureCommand"
                );
            },
            false,
            [&](Moer::Array<RHIRecordingSource>&&) {
                ++publish_calls;
            },
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        );
        suite.Check(!executed, test_name, "mutating configuration unexpectedly succeeded");
        suite.Check(
            Contains(graph.GetCompileError(), "only change submit metadata"),
            test_name,
            graph.GetCompileError()
        );
        suite.Check(
            configure_calls == 1 && record_calls == 0 && publish_calls == 0,
            test_name,
            "configuration mutation must fail before recording or publication"
        );
    }
}

void TestRecordingSourceSetupCannotCompleteProducerGate(TestSuite& suite) {
    for (const bool active : {false, true}) {
        for (const bool signal : {false, true}) {
            const std::string test_name =
                std::string(active ? "active" : "legacy") +
                " source setup cannot replace producer gate with a " +
                (signal ? "succeeded" : "failed") + " gate";
            BufferRef buffer = MoerNew(FakeBuffer)(64);
            RenderGraph graph("SetupCannotCompleteProducerGate");
            const auto data = graph.ImportBuffer(
                "Data",
                buffer,
                RenderGraph::BufferDesc{.byte_size = 64}
            );
            graph.SetInitialState(
                data,
                RenderGraph::BufferBoundary::Undefined,
                RenderGraph::QueueRole::None,
                RenderGraph::AccessMode::None
            );
            int record_calls    = 0;
            int configure_calls = 0;
            int publish_calls   = 0;
            graph.AddRecordPass(
                "Write",
                [=](RenderGraph::PassBuilder& builder) {
                    builder.Write(data, RenderGraph::BufferAccessType::Storage)
                        .SideEffect();
                },
                [&](CommandList&) {
                    ++record_calls;
                },
                RenderGraph::PassRecordPolicy::Serial
            );
            graph.Export(
                data,
                RenderGraph::BufferAccessType::ShaderRead,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );

            suite.Check(graph.Compile(), test_name, graph.GetCompileError());
            const bool executed = graph.ExecuteFrontendRecordingPlan(
                [&](std::span<const RenderGraph::ExecutedPassInfo>, RHIRecordingSource& source) {
                    ++configure_calls;
                    auto replacement = RHIRecordingGate::Create();
                    if (signal) {
                        (void)replacement->Signal();
                    } else {
                        (void)replacement->Fail();
                    }
                    source.completion = replacement;
                },
                false,
                [&](Moer::Array<RHIRecordingSource>&&) {
                    ++publish_calls;
                },
                RenderGraph::ActiveRecordingOptions{.enabled = active}
            );
            suite.Check(
                !executed,
                test_name,
                "source setup unexpectedly completed producer gate"
            );
            suite.Check(
                Contains(graph.GetCompileError(), "changed ownership"),
                test_name,
                graph.GetCompileError()
            );
            suite.Check(
                configure_calls == 1 && record_calls == 0 && publish_calls == 0,
                test_name,
                "terminal producer gate must fail before recording or publication"
            );
        }
    }
}

void TestManagedRecordCallbackCannotSealOrDowngrade(
    TestSuite& suite,
    ERHIResourceStateOwnership ownership = ERHIResourceStateOwnership::Compiled
) {
    for (const auto execution :
         {RenderGraph::PassRecordPolicy::Serial,
          RenderGraph::PassRecordPolicy::ParallelAllowed}) {
        for (const bool seal : {false, true}) {
            const std::string test_name =
                std::string(
                    execution == RenderGraph::PassRecordPolicy::Serial ?
                        "serial" :
                        "parallel-eligible"
                ) +
                " managed record callback cannot " +
                (seal ? "seal" : "downgrade ownership");
            BufferRef buffer = MoerNew(FakeBuffer)(64);
            RenderGraph graph("ManagedRecordMutationRejected");
            const auto data = graph.ImportBuffer(
                "Data",
                buffer,
                RenderGraph::BufferDesc{.byte_size = 64}
            );
            graph.SetInitialState(
                data,
                RenderGraph::BufferBoundary::Undefined,
                RenderGraph::QueueRole::None,
                RenderGraph::AccessMode::None
            );
            int record_calls  = 0;
            int publish_calls = 0;
            graph.AddRecordPass(
                "Write",
                [=](RenderGraph::PassBuilder& builder) {
                    builder.Write(data, RenderGraph::BufferAccessType::Storage)
                        .SideEffect();
                },
                [&](CommandList& commands) {
                    ++record_calls;
                    if (seal) {
                        [[maybe_unused]] CmdSubmit discarded = commands.Submit();
                    } else {
                        commands.SetResourceStateOwnership(
                            ERHIResourceStateOwnership::BackendTracked
                        );
                    }
                },
                execution
            );
            graph.Export(
                data,
                RenderGraph::BufferAccessType::ShaderRead,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );

            suite.Check(graph.Compile(), test_name, graph.GetCompileError());
            const bool executed = graph.ExecuteFrontendRecordingPlan(
                {},
                true,
                [&](Moer::Array<RHIRecordingSource>&&) {
                    ++publish_calls;
                },
                RenderGraph::ActiveRecordingOptions{.enabled = true, .state_ownership = ownership}
            );
            suite.Check(!executed, test_name, "managed mutation unexpectedly succeeded");
            suite.Check(
                Contains(
                    graph.GetCompileError(),
                    seal ? "Submit is forbidden while graph-managed recording is active" :
                           "changed explicit state ownership"
                ),
                test_name,
                graph.GetCompileError()
            );
            suite.Check(
                record_calls == 1 && publish_calls == 1,
                test_name,
                "managed mutation must fail its already-admitted producer"
            );
        }
    }
}

void TestTokenOnlyCompiledPassPreservesOwnership(TestSuite& suite) {
    constexpr std::string_view test_name = "token-only compiled pass retains exact ownership";
    for (const bool downgrade : {false, true}) {
        RenderGraph graph("CompiledTokenOnly");
        const auto table = graph.ImportToken("Table");
        bool observed_compiled = false;
        graph.AddRecordPass(
            "PublishTable",
            [=](RenderGraph::PassBuilder& builder) { builder.Write(table).SideEffect(); },
            [&](CommandList& commands) {
                observed_compiled = commands.GetResourceStateOwnership() ==
                                    ERHIResourceStateOwnership::Compiled;
                if (downgrade) {
                    commands.SetResourceStateOwnership(ERHIResourceStateOwnership::BackendTracked);
                }
            },
            RenderGraph::PassRecordPolicy::ParallelAllowed
        );
        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        Moer::Array<RHIRecordingSource> published;
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            {},
            true,
            [&](Moer::Array<RHIRecordingSource>&& sources) { published = std::move(sources); },
            RenderGraph::ActiveRecordingOptions{
                .enabled = true, .state_ownership = ERHIResourceStateOwnership::Compiled
            }
        );
        suite.Check(observed_compiled, test_name, "token-only pass fell back to BackendTracked");
        suite.Check(executed == !downgrade, test_name, graph.GetCompileError());
        suite.Check(published.size() == 1, test_name, "expected one recording source");
        if (!published.empty()) {
            suite.Check(
                published.front().completion.Status() ==
                    (downgrade ? ERHIRecordingStatus::Failed : ERHIRecordingStatus::Succeeded),
                test_name, "producer completion did not reflect ownership validation"
            );
            if (!downgrade) {
                const auto submit = published.front().command_list->Submit();
                suite.Check(submit.resource_state_ownership == ERHIResourceStateOwnership::Compiled,
                            test_name, "token-only submit lost compiled ownership");
            }
        }
    }
}

void TestManagedRecordCallbackCannotMoveCommandList(TestSuite& suite) {
    for (const auto execution :
         {RenderGraph::PassRecordPolicy::Serial,
          RenderGraph::PassRecordPolicy::ParallelAllowed}) {
        for (const bool move_construct : {false, true}) {
            const std::string test_name =
                std::string(
                    execution == RenderGraph::PassRecordPolicy::Serial ?
                        "serial" :
                        "parallel-eligible"
                ) +
                " managed record callback cannot " +
                (move_construct ? "move-construct" : "move-assign") +
                " CommandList";
            BufferRef buffer = MoerNew(FakeBuffer)(64);
            RenderGraph graph("ManagedRecordMoveRejected");
            const auto data = graph.ImportBuffer(
                "Data",
                buffer,
                RenderGraph::BufferDesc{.byte_size = 64}
            );
            graph.SetInitialState(
                data,
                RenderGraph::BufferBoundary::Undefined,
                RenderGraph::QueueRole::None,
                RenderGraph::AccessMode::None
            );
            int record_calls  = 0;
            int publish_calls = 0;
            graph.AddRecordPass(
                "Write",
                [=](RenderGraph::PassBuilder& builder) {
                    builder.Write(data, RenderGraph::BufferAccessType::Storage)
                        .SideEffect();
                },
                [&](CommandList& commands) {
                    ++record_calls;
                    if (move_construct) {
                        [[maybe_unused]] CommandList stolen(
                            std::move(commands)
                        );
                    } else {
                        commands = CommandList(EQueueType::Graphics);
                    }
                },
                execution
            );
            graph.Export(
                data,
                RenderGraph::BufferAccessType::ShaderRead,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );

            suite.Check(graph.Compile(), test_name, graph.GetCompileError());
            const bool executed = graph.ExecuteFrontendRecordingPlan(
                {},
                true,
                [&](Moer::Array<RHIRecordingSource>&&) {
                    ++publish_calls;
                },
                RenderGraph::ActiveRecordingOptions{.enabled = true}
            );
            suite.Check(!executed, test_name, "managed move unexpectedly succeeded");
            suite.Check(
                Contains(
                    graph.GetCompileError(),
                    "CommandList move is forbidden while graph-managed recording is active"
                ),
                test_name,
                graph.GetCompileError()
            );
            suite.Check(
                record_calls == 1 && publish_calls == 1,
                test_name,
                "managed move must fail its already-admitted producer"
            );
        }
    }
}

void TestRecordProducerRejectsBlockingSync(TestSuite& suite) {
    for (const auto execution :
         {RenderGraph::PassRecordPolicy::Serial,
          RenderGraph::PassRecordPolicy::ParallelAllowed}) {
        const std::string test_name =
            std::string(
                execution == RenderGraph::PassRecordPolicy::Serial ?
                    "serial" :
                    "parallel-eligible"
            ) +
            " record producer rejects blocking Sync";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph("RecordProducerRejectsSync");
        const auto data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferBoundary::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int record_calls  = 0;
        int publish_calls = 0;
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferAccessType::Storage)
                    .SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
                RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            },
            execution
        );
        graph.Export(
            data,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            {},
            true,
            [&](Moer::Array<RHIRecordingSource>&&) {
                ++publish_calls;
            },
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        );
        suite.Check(!executed, test_name, "blocking Sync unexpectedly succeeded");
        suite.Check(
            Contains(graph.GetCompileError(), "blocking RHI lifecycle call"),
            test_name,
            graph.GetCompileError()
        );
        suite.Check(
            record_calls == 1 && publish_calls == 1,
            test_name,
            "blocking call must fail the admitted producer without hanging"
        );
    }
}

void TestRecordingPublisherRejectsBlockingSync(TestSuite& suite) {
    constexpr std::string_view test_name =
        "recording publisher rejects blocking Sync";
    BufferRef buffer = MoerNew(FakeBuffer)(64);
    RenderGraph graph("PublisherRejectsSync");
    const auto data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    int record_calls  = 0;
    int publish_calls = 0;
    graph.AddRecordPass(
        "Write",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage)
                .SideEffect();
        },
        [&](CommandList&) {
            ++record_calls;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&&) {
            ++publish_calls;
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        },
        RenderGraph::ActiveRecordingOptions{.enabled = true}
    );
    suite.Check(!executed, test_name, "blocking publisher unexpectedly succeeded");
    suite.Check(
        Contains(graph.GetCompileError(), "blocking RHI lifecycle call"),
        test_name,
        graph.GetCompileError()
    );
    suite.Check(
        publish_calls == 1 && record_calls == 0,
        test_name,
        "publisher rejection must happen before producer dispatch"
    );
}

void TestRecordingConfigurationRejectsBlockingSync(TestSuite& suite) {
    constexpr std::string_view test_name =
        "recording source configuration rejects blocking Sync";
    BufferRef buffer = MoerNew(FakeBuffer)(64);
    RenderGraph graph("ConfigurationRejectsSync");
    const auto data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    int configure_calls = 0;
    int record_calls    = 0;
    int publish_calls   = 0;
    graph.AddRecordPass(
        "Write",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage)
                .SideEffect();
        },
        [&](CommandList&) {
            ++record_calls;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        [&](std::span<const RenderGraph::ExecutedPassInfo>, RHIRecordingSource&) {
            ++configure_calls;
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        },
        false,
        [&](Moer::Array<RHIRecordingSource>&&) {
            ++publish_calls;
        },
        RenderGraph::ActiveRecordingOptions{.enabled = true}
    );
    suite.Check(!executed, test_name, "blocking configuration unexpectedly succeeded");
    suite.Check(
        Contains(graph.GetCompileError(), "blocking RHI lifecycle call"),
        test_name,
        graph.GetCompileError()
    );
    suite.Check(
        configure_calls == 1 && record_calls == 0 && publish_calls == 0,
        test_name,
        "configuration rejection must precede publication and recording"
    );
}

void TestRecordingPublisherCannotMutatePendingSource(TestSuite& suite) {
    constexpr std::string_view mutation_names[] = {
        "seal CommandList",
        "record command",
    };
    for (int mutation = 0; mutation < 2; ++mutation) {
        const std::string test_name =
            "recording publisher cannot " +
            std::string(mutation_names[mutation]);
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph("PublisherMutationRejected");
        const auto data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferBoundary::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int record_calls  = 0;
        int publish_calls = 0;
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferAccessType::Storage)
                    .SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            data,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            {},
            false,
            [&](Moer::Array<RHIRecordingSource>&& sources) {
                ++publish_calls;
                auto& source = sources.front();
                switch (mutation) {
                    case 0: {
                        [[maybe_unused]] CmdSubmit discarded =
                            source.command_list->Submit();
                        break;
                    }
                    case 1:
                        source.command_list->AddCustomCommand(
                            MakeUnique<ProbeCommand>(505),
                            "IllegalPublisherCommand"
                        );
                        break;
                }
            },
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        );
        suite.Check(!executed, test_name, "publisher mutation unexpectedly succeeded");
        suite.Check(
            Contains(
                graph.GetCompileError(),
                mutation == 0 ?
                    "Submit is forbidden while graph-managed recording is active" :
                    "publisher mutated pending source"
            ),
            test_name,
            graph.GetCompileError()
        );
        suite.Check(
            publish_calls == 1 && record_calls == 0,
            test_name,
            "publisher mutation must fail before producer dispatch"
        );
    }
}

void TestActiveRecordingGroupsCommitAtomically(TestSuite& suite, bool split_stream) {
    const std::string test_name = split_stream ? "active recording groups commit atomically" :
                                                "shared active stream fails atomically";
    BufferRef buffer = MoerNew(FakeBuffer)(64);
    RenderGraph graph("ActiveRecordingTransaction");
    const auto data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    int first_record_calls  = 0;
    int second_record_calls = 0;
    graph.AddRecordPass(
        "FirstWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferAccessType::Storage)
                .SideEffect();
        },
        [&](CommandList&) {
            ++first_record_calls;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "SecondReadFails",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferAccessType::ShaderRead)
                .SideEffect();
            if (split_stream) { builder.TranslateSerialControl(); }
        },
        [&](CommandList&) {
            ++second_record_calls;
            throw std::runtime_error("injected second-group failure");
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        data,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    Moer::Array<RHIRecordingSource> published{};
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            for (auto& source : sources) { published.emplace_back(std::move(source)); }
        },
        RenderGraph::ActiveRecordingOptions{.enabled = true}
    );
    suite.Check(!executed, test_name, "failing graph unexpectedly committed");
    suite.Check(
        Contains(graph.GetCompileError(), "injected second-group failure"),
        test_name,
        graph.GetCompileError()
    );
    suite.Check(
        first_record_calls == 1 && second_record_calls == 1 &&
            published.size() == (split_stream ? 2u : 1u),
        test_name,
        "the expected recording streams were not admitted and joined"
    );

    RHIRecordingGateView shared_commit{};
    for (size_t index = 0; index < published.size(); ++index) {
        auto& source = published[index];
        suite.Check(
            static_cast<bool>(source.commit),
            test_name,
            "published source is missing its graph commit gate"
        );
        if (!source.commit) {
            continue;
        }
        if (!shared_commit) {
            shared_commit = source.commit;
        }
        suite.Check(
            source.commit == shared_commit &&
            source.commit.Status() ==
                    ERHIRecordingStatus::Failed,
            test_name,
            "recording groups did not share one failed transaction"
        );
        suite.Check(source.completion.Status() ==
            (split_stream && index == 0 ? ERHIRecordingStatus::Succeeded : ERHIRecordingStatus::Failed),
            test_name, "each producer gate must describe its own completed recording outcome");
        auto cleanup =
            source.command_list->DrainOrdinaryCallbacksForRejection();
        for (auto& callback : cleanup) {
            if (callback) {
                callback();
            }
        }
    }
}

void TestSerialFailureStopsLaterRecordingStreams(TestSuite& suite) {
    constexpr std::string_view test_name = "serial failure abandons later recording streams";
    RenderGraph graph("SerialFailureStopsLaterStreams");
    int first_calls = 0, later_calls = 0;
    graph.AddRecordPass("FirstFails", [](RenderGraph::PassBuilder& pass) {
        pass.SideEffect().TranslateSerialControl();
    }, [&](CommandList&) {
        ++first_calls;
        throw std::runtime_error("injected first-stream failure");
    }, RenderGraph::PassRecordPolicy::Serial);
    for (const auto name : {"LaterFirst", "LaterSecond"}) {
        graph.AddRecordPass(name, [](RenderGraph::PassBuilder& pass) { pass.SideEffect(); },
            [&](CommandList&) { ++later_calls; }, RenderGraph::PassRecordPolicy::Serial);
    }
    Moer::Array<RHIRecordingSource> published;
    bool all_published_pending = true;
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteFrontendRecordingPlan({}, false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            for (auto& source : sources) {
                all_published_pending &= source.completion.Status() == ERHIRecordingStatus::Pending &&
                    source.commit && source.commit.Status() == ERHIRecordingStatus::Pending;
                published.emplace_back(std::move(source));
            }
        }, {.enabled = true});
    suite.Check(!executed && Contains(graph.GetCompileError(), "injected first-stream failure"),
        test_name, graph.GetCompileError());
    suite.Check(first_calls == 1 && later_calls == 0, test_name,
        "a failing serial stream must prevent callbacks in all later streams");
    suite.Check(published.size() == 2 && all_published_pending, test_name,
        "both pending streams must be admitted before their producer outcomes are known");
    for (const auto& source : published) {
        suite.Check(source.completion.Status() == ERHIRecordingStatus::Failed && source.commit &&
            source.commit == published.front().commit && source.commit.Status() == ERHIRecordingStatus::Failed,
            test_name, "failed and abandoned producers must all terminate under one failed graph commit");
    }
}

void TestActiveRecordingLifetimeOutlivesUserCallbacks(TestSuite& suite) {
    constexpr std::string_view test_name =
        "active recording lifetime outlives ordinary and success callbacks";
    auto destroyed = std::make_shared<bool>(false);
    std::vector<int> callback_order{};
    Moer::Array<RHIRecordingSource> published{};

    {
        BufferRef buffer = MoerNew(LifetimeProbeBuffer)(destroyed);
        Buffer* raw_buffer = buffer.Get();
        RenderGraph graph("RecordingLifetimeTail");
        const auto data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferBoundary::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferAccessType::Storage)
                    .SideEffect();
            },
            [&](CommandList& commands) {
                commands.AddCallback([&, raw_buffer, destroyed] {
                    suite.Check(
                        !*destroyed && raw_buffer->GetByteSize() == 64,
                        test_name,
                        "resource died before an ordinary user callback"
                    );
                    callback_order.push_back(1);
                });
                commands.AddSuccessCallback([&, raw_buffer, destroyed] {
                    suite.Check(
                        !*destroyed && raw_buffer->GetByteSize() == 64,
                        test_name,
                        "resource died before a success user callback"
                    );
                    callback_order.push_back(2);
                });
            },
            RenderGraph::PassRecordPolicy::Serial
        );
        graph.Export(
            data,
            RenderGraph::BufferAccessType::ShaderRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        suite.Check(
            graph.ExecuteFrontendRecordingPlan(
                {},
                false,
                [&](Moer::Array<RHIRecordingSource>&& sources) {
                    published = std::move(sources);
                },
                RenderGraph::ActiveRecordingOptions{.enabled = true}
            ),
            test_name,
            graph.GetCompileError()
        );
    }

    suite.Check(
        !*destroyed && published.size() == 1,
        test_name,
        "recording lifetime did not survive graph destruction"
    );
    if (published.size() != 1) {
        return;
    }
    CmdSubmit submit = published.front().command_list->Submit();
    submit.cmds.clear();
    submit.cached_args.clear();
    for (auto& callback : submit.callbacks) {
        callback();
    }
    for (auto& callback : submit.success_callbacks) {
        callback();
    }
    suite.Check(
        !*destroyed && callback_order == std::vector<int>{1, 2},
        test_name,
        "lifetime did not cover both callback classes in order"
    );
    submit.callbacks.clear();
    submit.success_callbacks.clear();
    submit.lifetime.reset();
    published.clear();
    suite.Check(
        *destroyed,
        test_name,
        "recording lifetime was retained after the completion payload was released"
    );
}

void TestCompiledChildGraphPreservesParentLeaseAndScope(TestSuite& suite) {
    constexpr std::string_view test_name = "compiled child graph preserves parent lease and scope";
    RenderGraph                parent("Parent");
    parent.AddRecordPass(
        "ParentPass",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect().TranslateSerialControl();
        },
        [&](CommandList& commands) {
            const auto generation = commands.GetSealGeneration();
            commands.PushScope("ParentScope");
            RenderGraph child("Child");
            child.AddRecordPass(
                "ChildPass",
                [](RenderGraph::PassBuilder& builder) {
                    builder.SideEffect();
                },
                [](CommandList& list) {
                    list.AddCustomCommand(MakeUnique<ProbeCommand>(707), "ChildBody");
                },
                RenderGraph::PassRecordPolicy::Serial
            );
            if (!child.Compile() || !child.RecordAndMergeFrontendCommands(commands, false)) {
                throw std::runtime_error(child.GetCompileError());
            }
            suite.Check(
                commands.GetSealGeneration() == generation, test_name, "child append must not seal its parent"
            );
            commands.PopScope();
        }, RenderGraph::PassRecordPolicy::Serial);
    Moer::Array<RHIRecordingSource> published;
    suite.Check(parent.Compile(), test_name, parent.GetCompileError());
    const bool success = parent.ExecuteFrontendRecordingPlan(
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            published = std::move(sources);
        },
        {.enabled = true, .state_ownership = ERHIResourceStateOwnership::Compiled}
    );
    suite.Check(success, test_name, parent.GetCompileError());
    if (!success || published.size() != 1) {
        return;
    }
    auto submission = published.front().command_list->Submit();
    suite.Check(
        submission.resource_state_ownership == ERHIResourceStateOwnership::Compiled &&
            submission.cmds.size() == 3 && AsProbe(submission.cmds[1]) &&
            AsProbe(submission.cmds[1])->Marker() == 707,
        test_name,
        "child commands must stay nested in the parent's complete compiled stream"
    );
}

} // namespace

int main() {
    TestSuite suite;
    TestCompiledChildGraphPreservesParentLeaseAndScope(suite);
    TestRecordPassMaterializesExplicitBeforeBodyAfter(suite);
    TestRecordPassMaterializesExplicitBeforeBodyAfter(suite, true);
    for (const bool merge : {false, true}) {
        for (const bool partial : {false, true}) {
            TestReadOnlyBoundariesNeedNoCommands(suite, merge, partial);
        }
        TestUnrepresentableReadOnlyRangeFails(suite, merge);
    }
    TestSameStateWriteBoundaryNeedsBarrier(suite, false);
    TestSameStateWriteBoundaryNeedsBarrier(suite, true);
    TestPresentationPublicationSurvivesNoOpFiltering(suite);
    TestPartialPhysicalRangesMaterialize(suite);
    TestUnsupportedInputsFailBeforeCallbacksOrPublish(suite);
    TestRecordingSourceSetupCannotCompleteProducerGate(suite);
    TestManagedRecordCallbackCannotSealOrDowngrade(suite);
    TestManagedRecordCallbackCannotSealOrDowngrade(suite, ERHIResourceStateOwnership::Compiled);
    TestTokenOnlyCompiledPassPreservesOwnership(suite);
    TestManagedRecordCallbackCannotMoveCommandList(suite);
    TestRecordProducerRejectsBlockingSync(suite);
    TestRecordingPublisherRejectsBlockingSync(suite);
    TestRecordingConfigurationRejectsBlockingSync(suite);
    TestRecordingPublisherCannotMutatePendingSource(suite);
    TestActiveRecordingGroupsCommitAtomically(suite, false);
    TestActiveRecordingGroupsCommitAtomically(suite, true);
    TestSerialFailureStopsLaterRecordingStreams(suite);

    TestActiveRecordingLifetimeOutlivesUserCallbacks(suite);

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraphActiveLowering: " << suite.FailureCount()
                  << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraphActiveLowering: all checks passed\n";
    return EXIT_SUCCESS;
}
