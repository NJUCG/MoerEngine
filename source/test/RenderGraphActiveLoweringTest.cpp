#include "rendergraph/RenderGraph.h"
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

void TestRecordPassMaterializesExplicitBeforeBodyAfter(TestSuite& suite) {
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.SetInitialState(
        color,
        RenderGraph::TextureState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddRecordPass(
        "WriteBoth",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                .Write(color, RenderGraph::TextureState::RenderTarget)
                .SideEffect();
        },
        [](CommandList& commands) {
            commands.AddCustomCommand(
                MakeUnique<ProbeCommand>(101),
                "ActiveLoweringBody"
            );
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    Moer::Array<RHIRecordingSource> published{};
    const bool executed = graph.ExecuteRecording(
        {},
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            for (auto& source : sources) {
                published.emplace_back(std::move(source));
            }
        },
        RenderGraph::ActiveRecordingOptions{.enabled = true}
    );
    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        published.size() == 1 &&
            published.front().completion.Status() == ERHIRecordingStatus::Succeeded,
        test_name,
        "one completed recording source must be published"
    );
    if (published.size() != 1) {
        return;
    }

    CmdSubmit submit = published.front().command_list->Submit();
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

void TestSameStateSecondReadMaterializesStateSeed(TestSuite& suite) {
    constexpr std::string_view test_name =
        "same-state second read materializes a state seed";

    BufferRef buffer = MoerNew(FakeBuffer)(256);
    RenderGraph graph("ActiveReadSeed");
    const auto  data = graph.ImportBuffer(
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
    for (int marker : {1, 2}) {
        graph.AddRecordPass(
            marker == 1 ? "FirstRead" : "SecondRead",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Read(data, RenderGraph::BufferState::ShaderResource).SideEffect();
            },
            [marker](CommandList& commands) {
                commands.AddCustomCommand(
                    MakeUnique<ProbeCommand>(marker),
                    marker == 1 ? "FirstReadBody" : "SecondReadBody"
                );
            },
            RenderGraph::PassExecutionClass::SerialRecord
        );
    }
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    Moer::Array<RHIRecordingSource> published{};
    const bool executed = graph.ExecuteRecording(
        {},
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            for (auto& source : sources) {
                published.emplace_back(std::move(source));
            }
        },
        RenderGraph::ActiveRecordingOptions{.enabled = true}
    );
    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        published.size() == 2 &&
            std::all_of(
                published.begin(),
                published.end(),
                [](const RHIRecordingSource& source) {
                    return source.completion.Status() ==
                           ERHIRecordingStatus::Succeeded;
                }
            ),
        test_name,
        "both independent read sources must complete"
    );
    if (published.size() != 2) {
        return;
    }

    CmdSubmit second_submit = published[1].command_list->Submit();
    suite.Check(
        second_submit.HasExplicitResourceStateOwnership(),
        test_name,
        "state-seeded read submit must use Explicit ownership"
    );
    const auto body = std::find_if(
        second_submit.cmds.begin(),
        second_submit.cmds.end(),
        [](const auto& command) {
            const auto* probe = AsProbe(command);
            return probe != nullptr && probe->Marker() == 2;
        }
    );
    suite.Check(
        body != second_submit.cmds.end() && body != second_submit.cmds.begin(),
        test_name,
        "the second read body must follow a materialized state seed"
    );
    if (body == second_submit.cmds.end() || body == second_submit.cmds.begin()) {
        return;
    }
    const auto* seed = AsBarrier(*(body - 1));
    suite.Check(
        seed != nullptr && seed->ExplicitBuffers().size() == 1,
        test_name,
        "the command immediately before the second read must be an explicit buffer seed"
    );
    if (seed != nullptr && seed->ExplicitBuffers().size() == 1) {
        const auto& explicit_seed = seed->ExplicitBuffers().front();
        suite.Check(
            explicit_seed.src_state == explicit_seed.dst_state &&
                explicit_seed.handle == reinterpret_cast<Moer::uint64>(buffer.Get()) &&
                explicit_seed.offset == 0 && explicit_seed.byte_size == 256,
            test_name,
            "state seed must adopt the same ShaderResource state over the full buffer"
        );
    }
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
    const bool executed = graph.ExecuteRecording(
        {},
        [&](const RenderGraph::ExecutedPassInfo&, RHIRecordingSource&) {
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
            "Automatic access fails before callbacks or publication";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph("ActiveAutomaticFailClosed");
        const auto  data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        int record_calls    = 0;
        int configure_calls = 0;
        int publish_calls   = 0;
        graph.AddRecordPass(
            "AutomaticRead",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Read(data).SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassExecutionClass::SerialRecord
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
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
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int record_calls    = 0;
        int configure_calls = 0;
        int publish_calls   = 0;
        graph.AddRecordPass(
            "RawWrite",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                    .SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassExecutionClass::SerialRecord
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
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
            "partial buffer bridge fails before callbacks or publication";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph("ActivePartialBufferFailClosed");
        const auto  data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        const RenderGraph::BufferRange range{.offset = 0, .size = 32};
        graph.SetInitialState(
            data,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None,
            range
        );
        int record_calls    = 0;
        int configure_calls = 0;
        int publish_calls   = 0;
        graph.AddRecordPass(
            "PartialWrite",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                           data,
                           RenderGraph::BufferState::UnorderedAccess,
                           range
                       )
                    .SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassExecutionClass::SerialRecord
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read,
            range
        );
        ExpectActiveFailClosed(
            suite,
            graph,
            test_name,
            "partial buffer state",
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
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int record_calls    = 0;
        int configure_calls = 0;
        int publish_calls   = 0;
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                    .SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassExecutionClass::SerialRecord
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteRecording(
            {},
            [&](const RenderGraph::ExecutedPassInfo&, RHIRecordingSource& source) {
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
                RenderGraph::BufferState::Undefined,
                RenderGraph::QueueRole::None,
                RenderGraph::AccessMode::None
            );
            int record_calls    = 0;
            int configure_calls = 0;
            int publish_calls   = 0;
            graph.AddRecordPass(
                "Write",
                [=](RenderGraph::PassBuilder& builder) {
                    builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                        .SideEffect();
                },
                [&](CommandList&) {
                    ++record_calls;
                },
                RenderGraph::PassExecutionClass::SerialRecord
            );
            graph.Export(
                data,
                RenderGraph::BufferState::ShaderResource,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );

            suite.Check(graph.Compile(), test_name, graph.GetCompileError());
            const bool executed = graph.ExecuteRecording(
                {},
                [&](const RenderGraph::ExecutedPassInfo&, RHIRecordingSource& source) {
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

void TestManagedRecordCallbackCannotSealOrDowngrade(TestSuite& suite) {
    for (const auto execution :
         {RenderGraph::PassExecutionClass::SerialRecord,
          RenderGraph::PassExecutionClass::ParallelRecordEligible}) {
        for (const bool seal : {false, true}) {
            const std::string test_name =
                std::string(
                    execution == RenderGraph::PassExecutionClass::SerialRecord ?
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
                RenderGraph::BufferState::Undefined,
                RenderGraph::QueueRole::None,
                RenderGraph::AccessMode::None
            );
            int record_calls  = 0;
            int publish_calls = 0;
            graph.AddRecordPass(
                "Write",
                [=](RenderGraph::PassBuilder& builder) {
                    builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
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
                RenderGraph::BufferState::ShaderResource,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );

            suite.Check(graph.Compile(), test_name, graph.GetCompileError());
            const bool executed = graph.ExecuteRecording(
                {},
                {},
                true,
                [&](Moer::Array<RHIRecordingSource>&&) {
                    ++publish_calls;
                },
                RenderGraph::ActiveRecordingOptions{.enabled = true}
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

void TestManagedRecordCallbackCannotMoveCommandList(TestSuite& suite) {
    for (const auto execution :
         {RenderGraph::PassExecutionClass::SerialRecord,
          RenderGraph::PassExecutionClass::ParallelRecordEligible}) {
        for (const bool move_construct : {false, true}) {
            const std::string test_name =
                std::string(
                    execution == RenderGraph::PassExecutionClass::SerialRecord ?
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
                RenderGraph::BufferState::Undefined,
                RenderGraph::QueueRole::None,
                RenderGraph::AccessMode::None
            );
            int record_calls  = 0;
            int publish_calls = 0;
            graph.AddRecordPass(
                "Write",
                [=](RenderGraph::PassBuilder& builder) {
                    builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
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
                RenderGraph::BufferState::ShaderResource,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );

            suite.Check(graph.Compile(), test_name, graph.GetCompileError());
            const bool executed = graph.ExecuteRecording(
                {},
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
         {RenderGraph::PassExecutionClass::SerialRecord,
          RenderGraph::PassExecutionClass::ParallelRecordEligible}) {
        const std::string test_name =
            std::string(
                execution == RenderGraph::PassExecutionClass::SerialRecord ?
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
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int record_calls  = 0;
        int publish_calls = 0;
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
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
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteRecording(
            {},
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    int record_calls  = 0;
    int publish_calls = 0;
    graph.AddRecordPass(
        "Write",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                .SideEffect();
        },
        [&](CommandList&) {
            ++record_calls;
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteRecording(
        {},
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    int configure_calls = 0;
    int record_calls    = 0;
    int publish_calls   = 0;
    graph.AddRecordPass(
        "Write",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                .SideEffect();
        },
        [&](CommandList&) {
            ++record_calls;
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteRecording(
        {},
        [&](const RenderGraph::ExecutedPassInfo&, RHIRecordingSource&) {
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
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int record_calls  = 0;
        int publish_calls = 0;
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                    .SideEffect();
            },
            [&](CommandList&) {
                ++record_calls;
            },
            RenderGraph::PassExecutionClass::SerialRecord
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteRecording(
            {},
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

void TestActiveRecordingGroupsCommitAtomically(TestSuite& suite) {
    constexpr std::string_view test_name =
        "active recording groups commit atomically";
    BufferRef buffer = MoerNew(FakeBuffer)(64);
    RenderGraph graph("ActiveRecordingTransaction");
    const auto data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    int first_record_calls  = 0;
    int second_record_calls = 0;
    graph.AddRecordPass(
        "FirstWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                .SideEffect();
        },
        [&](CommandList&) {
            ++first_record_calls;
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );
    graph.AddRecordPass(
        "SecondReadFails",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferState::ShaderResource)
                .SideEffect();
        },
        [&](CommandList&) {
            ++second_record_calls;
            throw std::runtime_error("injected second-group failure");
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    Moer::Array<Moer::Array<RHIRecordingSource>> published_groups{};
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteRecording(
        {},
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            published_groups.emplace_back(std::move(sources));
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
            published_groups.size() == 2,
        test_name,
        "both recording groups were not admitted and joined"
    );

    RHIRecordingGateView shared_commit{};
    for (auto& group : published_groups) {
        suite.Check(
            group.size() == 1 && group.front().commit,
            test_name,
            "published source is missing its graph commit gate"
        );
        if (group.empty() || !group.front().commit) {
            continue;
        }
        if (!shared_commit) {
            shared_commit = group.front().commit;
        }
        suite.Check(
            group.front().commit == shared_commit &&
            group.front().commit.Status() ==
                    ERHIRecordingStatus::Failed,
            test_name,
            "recording groups did not share one failed transaction"
        );
        auto cleanup =
            group.front().command_list->DrainOrdinaryCallbacksForRejection();
        for (auto& callback : cleanup) {
            if (callback) {
                callback();
            }
        }
    }
}

void TestActiveMixedMainThreadAndRecordFailsBeforeCallbacks(TestSuite& suite) {
    constexpr std::string_view test_name =
        "active mixed MainThread and record passes fail closed";
    BufferRef buffer = MoerNew(FakeBuffer)(64);
    CommandList caller_commands(EQueueType::Graphics);
    RenderGraph graph("MixedMainAndRecordRejected");
    const auto data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 64}
    );
    const auto token = graph.CreateTransientToken("MainToRecordToken");
    graph.SetInitialState(
        data,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    int main_calls    = 0;
    int record_calls  = 0;
    int publish_calls = 0;
    const auto main_pass = graph.AddPass(
        "MainWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                .Write(token)
                .SideEffect();
        },
        [&] {
            ++main_calls;
        }
    );
    graph.AddRecordPass(
        "TokenOnlyRecord",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(token)
                .DependsOn(main_pass)
                .SideEffect();
        },
        [&](CommandList&) {
            ++record_calls;
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool executed = graph.ExecuteRecording(
        {},
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&&) {
            ++publish_calls;
        },
        RenderGraph::ActiveRecordingOptions{
            .enabled                  = true,
            .main_thread_command_list = &caller_commands,
        }
    );
    suite.Check(!executed, test_name, "mixed active execution unexpectedly succeeded");
    suite.Check(
        Contains(graph.GetCompileError(), "does not yet support mixing"),
        test_name,
        graph.GetCompileError()
    );
    suite.Check(
        main_calls == 0 && record_calls == 0 && publish_calls == 0 &&
            caller_commands.IsEmpty(),
        test_name,
        "mixed-path rejection must precede callbacks and materialization"
    );
}

void TestActiveMainThreadCallbackCannotSealOrDowngrade(TestSuite& suite) {
    for (const bool seal : {false, true}) {
        const std::string test_name =
            std::string("active MainThread callback cannot ") +
            (seal ? "seal" : "downgrade ownership");
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        CommandList caller_commands(EQueueType::Graphics);
        RenderGraph graph("MainThreadMutationRejected");
        const auto data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int execute_calls = 0;
        graph.AddPass(
            "MainWrite",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                    .SideEffect();
            },
            [&] {
                ++execute_calls;
                if (seal) {
                    [[maybe_unused]] CmdSubmit discarded =
                        caller_commands.Submit();
                } else {
                    caller_commands.SetResourceStateOwnership(
                        ERHIResourceStateOwnership::BackendTracked
                    );
                }
            }
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteRecording(
            {},
            {},
            false,
            {},
            RenderGraph::ActiveRecordingOptions{
                .enabled                  = true,
                .main_thread_command_list = &caller_commands,
            }
        );
        suite.Check(!executed, test_name, "MainThread mutation unexpectedly succeeded");
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
            execute_calls == 1,
            test_name,
            "MainThread mutation callback was not executed exactly once"
        );
        suite.Check(
            caller_commands.IsEmpty() &&
                !caller_commands.HasExplicitResourceStateOwnership(),
            test_name,
            "rejected MainThread commands remained GPU-submittable"
        );
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
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddRecordPass(
            "Write",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
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
            RenderGraph::PassExecutionClass::SerialRecord
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        suite.Check(
            graph.ExecuteRecording(
                {},
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
    published.clear();
    suite.Check(
        *destroyed,
        test_name,
        "recording lifetime was retained after all callback payloads were released"
    );
}

void TestActiveMainThreadExceptionIsReportedAndDrained(TestSuite& suite) {
    constexpr std::string_view test_name =
        "active MainThread exception is reported and drained";
    BufferRef buffer = MoerNew(FakeBuffer)(64);
    CommandList caller_commands(EQueueType::Graphics);
    RenderGraph graph("MainThreadException");
    const auto data = graph.ImportBuffer(
        "Data",
        buffer,
        RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        data,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddPass(
        "ThrowingMain",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                .SideEffect();
        },
        [] {
            throw std::runtime_error("injected main failure");
        }
    );
    graph.Export(
        data,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    bool escaped = false;
    bool executed = false;
    try {
        executed = graph.ExecuteRecording(
            {},
            {},
            false,
            {},
            RenderGraph::ActiveRecordingOptions{
                .enabled                  = true,
                .main_thread_command_list = &caller_commands,
            }
        );
    } catch (...) {
        escaped = true;
    }
    suite.Check(
        !escaped && !executed &&
            Contains(graph.GetCompileError(), "injected main failure"),
        test_name,
        graph.GetCompileError()
    );
    suite.Check(
        caller_commands.IsEmpty() &&
            !caller_commands.HasExplicitResourceStateOwnership(),
        test_name,
        "failed MainThread stream remained GPU-submittable"
    );
}

void TestMainThreadPhysicalPassRequiresAndUsesCallerList(TestSuite& suite) {
    {
        constexpr std::string_view test_name =
            "main-thread physical pass requires caller CommandList";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        RenderGraph graph("ActiveMainThreadMissingList");
        const auto  data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int execute_calls = 0;
        int observer_calls = 0;
        graph.AddPass(
            "MainWrite",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                    .SideEffect();
            },
            [&] {
                ++execute_calls;
            }
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteRecording(
            [&](const RenderGraph::ExecutedPassInfo&) {
                ++observer_calls;
            },
            {},
            false,
            {},
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        );
        suite.Check(!executed, test_name, "execution unexpectedly succeeded without a list");
        suite.Check(
            Contains(graph.GetCompileError(), "caller-owned CommandList"),
            test_name,
            graph.GetCompileError()
        );
        suite.Check(
            execute_calls == 0 && observer_calls == 0,
            test_name,
            "missing-list validation must precede the main-thread callback and observer"
        );
    }

    {
        constexpr std::string_view test_name =
            "active MainThread physical pass defers submission until graph return";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        CommandList caller_commands(EQueueType::Graphics);
        RenderGraph graph("ActiveMainThreadObserverRejected");
        const auto data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        int execute_calls  = 0;
        int observer_calls = 0;
        graph.AddPass(
            "MainWrite",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                    .SideEffect();
            },
            [&] {
                ++execute_calls;
            }
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteRecording(
            [&](const RenderGraph::ExecutedPassInfo&) {
                ++observer_calls;
            },
            {},
            false,
            {},
            RenderGraph::ActiveRecordingOptions{
                .enabled                  = true,
                .main_thread_command_list = &caller_commands,
            }
        );
        suite.Check(!executed, test_name, "per-pass active submission was accepted");
        suite.Check(
            Contains(graph.GetCompileError(), "remain unsealed"),
            test_name,
            graph.GetCompileError()
        );
        suite.Check(
            execute_calls == 0 && observer_calls == 0 &&
                caller_commands.IsEmpty(),
            test_name,
            "observer restriction must be validated before materialization"
        );
    }

    {
        constexpr std::string_view test_name =
            "main-thread physical pass materializes into caller CommandList";
        BufferRef buffer = MoerNew(FakeBuffer)(64);
        CommandList caller_commands(EQueueType::Graphics);
        RenderGraph graph("ActiveMainThreadCallerList");
        const auto  data = graph.ImportBuffer(
            "Data",
            buffer,
            RenderGraph::BufferDesc{.byte_size = 64}
        );
        graph.SetInitialState(
            data,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        graph.AddPass(
            "MainWrite",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(data, RenderGraph::BufferState::UnorderedAccess)
                    .SideEffect();
            },
            [&] {
                caller_commands.AddCustomCommand(
                    MakeUnique<ProbeCommand>(303),
                    "MainThreadBody"
                );
            }
        );
        graph.Export(
            data,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteRecording(
            {},
            {},
            false,
            {},
            RenderGraph::ActiveRecordingOptions{
                .enabled                  = true,
                .main_thread_command_list = &caller_commands,
            }
        );
        suite.Check(executed, test_name, graph.GetCompileError());

        CmdSubmit submit = caller_commands.Submit();
        suite.Check(
            submit.HasExplicitResourceStateOwnership() && submit.cmds.size() == 3,
            test_name,
            "caller list must seal an Explicit before-body-after stream"
        );
        if (submit.cmds.size() == 3) {
            const auto* before = AsBarrier(submit.cmds[0]);
            const auto* body   = AsProbe(submit.cmds[1]);
            const auto* after  = AsBarrier(submit.cmds[2]);
            suite.Check(
                before != nullptr && before->ExplicitBuffers().size() == 1 &&
                    body != nullptr && body->Marker() == 303 &&
                    after != nullptr && after->ExplicitBuffers().size() == 1,
                test_name,
                "main-thread barriers must be materialized around its callback body"
            );
        }
    }
}

} // namespace

int main() {
    TestSuite suite;
    TestRecordPassMaterializesExplicitBeforeBodyAfter(suite);
    TestSameStateSecondReadMaterializesStateSeed(suite);
    TestUnsupportedInputsFailBeforeCallbacksOrPublish(suite);
    TestRecordingSourceSetupCannotCompleteProducerGate(suite);
    TestManagedRecordCallbackCannotSealOrDowngrade(suite);
    TestManagedRecordCallbackCannotMoveCommandList(suite);
    TestRecordProducerRejectsBlockingSync(suite);
    TestRecordingPublisherRejectsBlockingSync(suite);
    TestRecordingConfigurationRejectsBlockingSync(suite);
    TestRecordingPublisherCannotMutatePendingSource(suite);
    TestActiveRecordingGroupsCommitAtomically(suite);
    TestActiveMixedMainThreadAndRecordFailsBeforeCallbacks(suite);
    TestActiveMainThreadCallbackCannotSealOrDowngrade(suite);
    TestActiveRecordingLifetimeOutlivesUserCallbacks(suite);
    TestActiveMainThreadExceptionIsReportedAndDrained(suite);
    TestMainThreadPhysicalPassRequiresAndUsesCallerList(suite);

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraphActiveLowering: " << suite.FailureCount()
                  << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraphActiveLowering: all checks passed\n";
    return EXIT_SUCCESS;
}
