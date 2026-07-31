#include "rendergraph/RenderGraphPassParameters.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rhi/RHIResource.h"
#include "taskgraph/TaskSystem.h"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

using Moer::Render::Buffer;
using Moer::Render::BufferInfo;
using Moer::Render::BufferRef;
using Moer::Render::CommandList;
using Moer::Render::RenderGraph;
using Moer::Render::RenderGraphLowering;
using Moer::Render::Texture;
using Moer::Render::TextureInfo;
using Moer::Render::TextureRef;

class TestSuite {
public:
    void Check(
        bool             condition,
        std::string_view test_name,
        std::string_view expectation
    ) {
        if (condition) {
            return;
        }
        ++failure_count;
        std::cerr << "[FAIL] " << test_name << ": " << expectation << '\n';
    }

    [[nodiscard]] int FailureCount() const {
        return failure_count;
    }

private:
    int failure_count = 0;
};

[[nodiscard]] bool Contains(std::string_view text, std::string_view fragment) {
    return text.find(fragment) != std::string_view::npos;
}

class FakeTexture final : public Texture {
public:
    FakeTexture(uint8_t mips, uint16_t layers) :
        Texture(MakeInfo(mips, layers)) {}

    Moer::uint GetMipByteSize(Moer::uint) const override {
        return 4;
    }

    void SetName(std::string_view) override {}

private:
    static TextureInfo MakeInfo(uint8_t mips, uint16_t layers) {
        TextureInfo info{};
        info.usage = ETextureUsageFlags::SAMPLED |
                     ETextureUsageFlags::UNORDERED_ACCESS |
                     ETextureUsageFlags::COLOR_ATTACHMENT |
                     ETextureUsageFlags::TRANSFER_SRC |
                     ETextureUsageFlags::TRANSFER_DST;
        info.extent       = {64, 64};
        info.num_mips     = mips;
        info.array_size   = layers;
        info.aspect_flags = ETextureAspectFlags::COLOR;
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

struct ProducerParameters {
    DEFINE_RG_TEXTURE_ACCESS(
        texture,
        RenderGraph::AccessMode::Write,
        RenderGraph::TextureState::TransferDestination
    );
    DEFINE_RG_BUFFER_ACCESS(
        buffer,
        RenderGraph::AccessMode::Write,
        RenderGraph::BufferState::TransferDestination
    );
    DEFINE_RG_TOKEN_ACCESS(token, RenderGraph::AccessMode::Write);

    DEFINE_RG_PARAMETER_ACCESS(texture, buffer, token);
};

struct NestedTextureParameters {
    DEFINE_RG_TEXTURE_ACCESS(
        texture,
        RenderGraph::AccessMode::Read,
        RenderGraph::TextureState::Sampled
    );

    DEFINE_RG_PARAMETER_ACCESS(texture);
};

struct ConsumerParameters {
    DEFINE_RG_NESTED_PARAMETER(NestedTextureParameters, nested);
    DEFINE_RG_BUFFER_ACCESS_ARRAY(buffers);
    DEFINE_RG_OPTIONAL_TOKEN_ACCESS(token, RenderGraph::AccessMode::Read);
    DEFINE_RG_OPTIONAL_TEXTURE_ACCESS(
        absent_texture,
        RenderGraph::AccessMode::Read,
        RenderGraph::TextureState::Sampled
    );

    DEFINE_RG_PARAMETER_ACCESS(nested, buffers, token, absent_texture);
};

struct PlanResult {
    bool        compiled = false;
    bool        lowered  = false;
    std::string error{};
    std::string graph_dump{};
    std::string lowered_dump{};
    RenderGraph::PassExecutionClass consumer_execution =
        RenderGraph::PassExecutionClass::MainThread;
    Moer::Render::ERHITranslateExecutionClass consumer_translate =
        Moer::Render::ERHITranslateExecutionClass::Parallel;
    uint32_t               consumer_workload = 0;
    RenderGraph::QueueRole consumer_queue = RenderGraph::QueueRole::None;
};

PlanResult BuildPlan(
    bool              parameterized,
    const TextureRef& physical_texture,
    const BufferRef&  physical_buffer
) {
    RenderGraph graph(
        "ParameterAccessEquivalence",
        RenderGraph::QueueTopology::DedicatedQueues()
    );
    const auto texture = graph.ImportTexture(
        "Texture",
        physical_texture,
        RenderGraph::TextureDesc{
            .mip_count   = 4,
            .layer_count = 2,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    const auto buffer = graph.ImportBuffer(
        "Buffer",
        physical_buffer,
        RenderGraph::BufferDesc{.byte_size = 256}
    );
    const auto token = graph.CreateTransientToken("Token");
    const RenderGraph::TextureRange texture_range{
        .aspects     = RenderGraph::TextureAspect::Color,
        .mip_first   = 1,
        .mip_count   = 2,
        .layer_first = 1,
        .layer_count = 1,
    };
    const RenderGraph::BufferRange buffer_range{.offset = 32, .size = 96};
    const RenderGraph::BufferRange buffer_read_range{.offset = 32, .size = 48};
    const RenderGraph::BufferRange buffer_read_write_range{
        .offset = 80,
        .size   = 48,
    };

    graph.SetInitialState(
        texture,
        RenderGraph::TextureState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        texture_range
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        buffer_range
    );

    if (parameterized) {
        ProducerParameters parameters{};
        parameters.texture.resource = texture;
        parameters.texture.range    = texture_range;
        parameters.buffer.resource  = buffer;
        parameters.buffer.range     = buffer_range;
        parameters.token            = token;
        graph.AddRecordPass(
            "Producer",
            std::move(parameters),
            [](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Copy,
                           RenderGraph::PipelineType::Copy
                       )
                    .SideEffect();
            },
            [](CommandList&, const ProducerParameters&) {},
            RenderGraph::PassExecutionClass::ParallelRecordEligible,
            3
        );
    } else {
        graph.AddRecordPass(
            "Producer",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Copy,
                           RenderGraph::PipelineType::Copy
                       )
                    .SideEffect()
                    .Write(
                        texture,
                        RenderGraph::TextureState::TransferDestination,
                        texture_range
                    )
                    .Write(
                        buffer,
                        RenderGraph::BufferState::TransferDestination,
                        buffer_range
                    )
                    .Write(token);
            },
            [](CommandList&) {},
            RenderGraph::PassExecutionClass::ParallelRecordEligible,
            3
        );
    }

    if (parameterized) {
        ConsumerParameters parameters{};
        parameters.nested.texture.resource = texture;
        parameters.nested.texture.range    = texture_range;
        parameters.buffers.emplace_back(
            Moer::Render::RGBufferAccessElement::Read(
                buffer,
                RenderGraph::BufferState::ShaderResource,
                buffer_read_range
            )
        );
        parameters.buffers.emplace_back(
            Moer::Render::RGBufferAccessElement::ReadWrite(
                buffer,
                RenderGraph::BufferState::UnorderedAccess,
                buffer_read_write_range
            )
        );
        parameters.token.emplace(token);
        graph.AddRecordPass(
            "Consumer",
            std::move(parameters),
            [](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Compute,
                           RenderGraph::PipelineType::Compute
                       )
                    .SideEffect()
                    .TranslateSerialControl();
            },
            [](CommandList&, const ConsumerParameters&) {},
            RenderGraph::PassExecutionClass::ParallelRecordEligible,
            11
        );
    } else {
        graph.AddRecordPass(
            "Consumer",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Compute,
                           RenderGraph::PipelineType::Compute
                       )
                    .SideEffect()
                    .TranslateSerialControl()
                    .Read(
                        texture,
                        RenderGraph::TextureState::Sampled,
                        texture_range
                    )
                    .Read(
                        buffer,
                        RenderGraph::BufferState::ShaderResource,
                        buffer_read_range
                    )
                    .ReadWrite(
                        buffer,
                        RenderGraph::BufferState::UnorderedAccess,
                        buffer_read_write_range
                    )
                    .Read(token);
            },
            [](CommandList&) {},
            RenderGraph::PassExecutionClass::ParallelRecordEligible,
            11
        );
    }

    graph.Export(
        texture,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Read,
        texture_range
    );
    graph.Export(
        buffer,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Read,
        buffer_range
    );

    PlanResult result{};
    result.compiled = graph.Compile();
    if (!result.compiled) {
        result.error = graph.GetCompileError();
        return result;
    }
    result.graph_dump = graph.Dump();
    const auto& plan  = graph.GetCompiledPlan();
    if (plan.recording_batches.size() == 2) {
        result.consumer_execution = plan.recording_batches[1].execution;
        result.consumer_translate =
            plan.recording_batches[1].translate_execution_class;
        result.consumer_workload = plan.recording_batches[1].workload;
        result.consumer_queue    = plan.recording_batches[1].queue.role;
    }

    RenderGraphLowering::LoweredPlan lowered{};
    result.lowered = RenderGraphLowering::Lower(graph, lowered, result.error);
    if (result.lowered) {
        result.lowered_dump = lowered.Dump();
    }
    return result;
}

void TestParameterizedAccessMatchesManualPlan(TestSuite& suite) {
    constexpr std::string_view test_name =
        "parameterized access matches manual plan";
    TextureRef texture = MoerNew(FakeTexture)(4, 2);
    BufferRef  buffer  = MoerNew(FakeBuffer)(256);

    const PlanResult manual = BuildPlan(false, texture, buffer);
    const PlanResult dsl    = BuildPlan(true, texture, buffer);

    suite.Check(manual.compiled, test_name, manual.error);
    suite.Check(dsl.compiled, test_name, dsl.error);
    suite.Check(manual.lowered, test_name, manual.error);
    suite.Check(dsl.lowered, test_name, dsl.error);
    suite.Check(
        manual.graph_dump == dsl.graph_dump,
        test_name,
        "typed accesses, edges, versions, barriers, waves and batches must be identical"
    );
    suite.Check(
        manual.lowered_dump == dsl.lowered_dump,
        test_name,
        "partial-range and cross-queue lowering must be identical"
    );
    suite.Check(
        dsl.consumer_execution ==
                RenderGraph::PassExecutionClass::ParallelRecordEligible &&
            dsl.consumer_translate ==
                Moer::Render::ERHITranslateExecutionClass::SerialControl &&
            dsl.consumer_workload == 11 &&
            dsl.consumer_queue == RenderGraph::QueueRole::Compute,
        test_name,
        "parameter expansion must preserve explicit recording and queue policy"
    );
}

struct RequiredTextureParameters {
    DEFINE_RG_TEXTURE_ACCESS(
        texture,
        RenderGraph::AccessMode::Read,
        RenderGraph::TextureState::Sampled
    );
    DEFINE_RG_PARAMETER_ACCESS(texture);
};

struct OptionalTextureParameters {
    DEFINE_RG_OPTIONAL_TEXTURE_ACCESS(
        texture,
        RenderGraph::AccessMode::Read,
        RenderGraph::TextureState::Sampled
    );
    DEFINE_RG_PARAMETER_ACCESS(texture);
};

void TestRequiredAndOptionalHandleContracts(TestSuite& suite) {
    constexpr std::string_view test_name =
        "required and optional parameter handles";

    RenderGraph required("RequiredInvalid");
    required.AddRecordPass(
        "Required",
        RequiredTextureParameters{},
        [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); },
        [](CommandList&, const RequiredTextureParameters&) {}
    );
    suite.Check(
        !required.Compile() &&
            Contains(required.GetCompileError(), "declared an invalid resource"),
        test_name,
        "a required invalid handle must fail closed"
    );

    RenderGraph absent("OptionalAbsent");
    absent.AddRecordPass(
        "Absent",
        OptionalTextureParameters{},
        [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); },
        [](CommandList&, const OptionalTextureParameters&) {}
    );
    suite.Check(
        absent.Compile(),
        test_name,
        absent.GetCompileError()
    );

    OptionalTextureParameters engaged_parameters{};
    engaged_parameters.texture.emplace(RenderGraph::TextureHandle{});
    RenderGraph engaged("OptionalEngagedInvalid");
    engaged.AddRecordPass(
        "Engaged",
        std::move(engaged_parameters),
        [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); },
        [](CommandList&, const OptionalTextureParameters&) {}
    );
    suite.Check(
        !engaged.Compile() &&
            Contains(engaged.GetCompileError(), "declared an invalid resource"),
        test_name,
        "an engaged optional access must retain required-handle validation"
    );
}

void TestNullableRecordFailsAtDeclaration(TestSuite& suite) {
    constexpr std::string_view test_name =
        "nullable parameterized record callback";
    using NullableRecord =
        std::function<void(CommandList&, const OptionalTextureParameters&)>;

    bool        setup_called = false;
    RenderGraph empty_function("EmptyRecordFunction");
    const auto empty_function_pass = empty_function.AddRecordPass(
        "EmptyFunction",
        OptionalTextureParameters{},
        [&](RenderGraph::PassBuilder&) { setup_called = true; },
        NullableRecord{}
    );
    suite.Check(
        !empty_function_pass.IsValid() && !setup_called &&
            !empty_function.Compile() &&
            Contains(empty_function.GetCompileError(), "no record callback"),
        test_name,
        "an empty std::function must preserve the existing declaration-time rejection"
    );

    using NullableFunctionPointer =
        void (*)(CommandList&, const OptionalTextureParameters&);
    NullableFunctionPointer null_record = nullptr;
    RenderGraph             null_function("NullRecordFunction");
    const auto null_function_pass = null_function.AddRecordPass(
        "NullFunction",
        OptionalTextureParameters{},
        [](RenderGraph::PassBuilder&) {},
        null_record
    );
    suite.Check(
        !null_function_pass.IsValid() && !null_function.Compile() &&
            Contains(null_function.GetCompileError(), "no record callback"),
        test_name,
        "a null function pointer must fail before it can reach std::invoke"
    );
}

struct LifetimeProbe {
    explicit LifetimeProbe(std::atomic<int>& destruction_count) :
        destruction_count(destruction_count) {}

    ~LifetimeProbe() {
        destruction_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int>& destruction_count;
};

struct MoveOnlyParameters {
    Moer::Render::RGTokenAccess<RenderGraph::AccessMode::Write> token{};
    std::unique_ptr<LifetimeProbe>                              probe{};
    std::atomic<const void*>*                                  declared_address{};
    int                                                        payload = 0;

    MoveOnlyParameters() = default;
    MoveOnlyParameters(const MoveOnlyParameters&) = delete;
    MoveOnlyParameters& operator=(const MoveOnlyParameters&) = delete;
    MoveOnlyParameters(MoveOnlyParameters&&) = default;
    MoveOnlyParameters& operator=(MoveOnlyParameters&&) = default;

    void DeclareRGAccess(
        Moer::Render::RGParameterAccessCollector& collector
    ) const {
        declared_address->store(this, std::memory_order_release);
        collector.Add(token);
    }
};

void TestMoveOnlyParameterLifetimeAndParallelRecord(TestSuite& suite) {
    constexpr std::string_view test_name =
        "move-only parameter lifetime and parallel record";
    std::atomic<int>         destruction_count{0};
    std::atomic<const void*> declared_address{nullptr};
    std::atomic<const void*> recorded_address{nullptr};
    std::atomic<int>         record_count{0};
    std::atomic<bool>        recorded_on_worker{false};
    const std::thread::id    caller_thread = std::this_thread::get_id();

    {
        RenderGraph graph("MoveOnlyParameters");
        const auto  token = graph.CreateTransientToken("Token");
        MoveOnlyParameters parameters{};
        parameters.token            = token;
        parameters.probe            = std::make_unique<LifetimeProbe>(destruction_count);
        parameters.declared_address = &declared_address;
        parameters.payload          = 42;

        graph.AddRecordPass(
            "Record",
            std::move(parameters),
            [](RenderGraph::PassBuilder& builder) {
                builder.SideEffect();
            },
            [&](CommandList&, const MoveOnlyParameters& immutable_parameters) {
                recorded_on_worker.store(
                    std::this_thread::get_id() != caller_thread,
                    std::memory_order_release
                );
                recorded_address.store(
                    &immutable_parameters,
                    std::memory_order_release
                );
                if (immutable_parameters.payload == 42 &&
                    immutable_parameters.probe != nullptr) {
                    record_count.fetch_add(1, std::memory_order_relaxed);
                }
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible,
            7
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        size_t published_source_count = 0;
        Moer::TaskSystem::Init();
        const bool executed = graph.ExecuteRecording(
            {},
            {},
            true,
            [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
                published_source_count += sources.size();
            }
        );
        Moer::TaskSystem::ShutDown();

        suite.Check(executed, test_name, graph.GetCompileError());
        suite.Check(
            record_count.load(std::memory_order_relaxed) == 1 &&
                published_source_count == 1,
            test_name,
            "the graph-owned parameters must reach the managed record callback exactly once"
        );
        suite.Check(
            recorded_on_worker.load(std::memory_order_acquire),
            test_name,
            "ParallelRecordEligible parameters must be consumed on a task worker"
        );
        suite.Check(
            declared_address.load(std::memory_order_acquire) ==
                recorded_address.load(std::memory_order_acquire),
            test_name,
            "declaration and recording must observe the same immutable parameter object"
        );
        suite.Check(
            destruction_count.load(std::memory_order_relaxed) == 0,
            test_name,
            "the parameter payload must remain alive through recording"
        );
    }

    suite.Check(
        destruction_count.load(std::memory_order_relaxed) == 1,
        test_name,
        "the graph-owned move-only parameter payload must be destroyed exactly once"
    );
}

} // namespace

int main() {
    TestSuite suite;
    TestParameterizedAccessMatchesManualPlan(suite);
    TestRequiredAndOptionalHandleContracts(suite);
    TestNullableRecordFailsAtDeclaration(suite);
    TestMoveOnlyParameterLifetimeAndParallelRecord(suite);

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraphParameterAccess: "
                  << suite.FailureCount() << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraphParameterAccess: all checks passed\n";
    return EXIT_SUCCESS;
}
