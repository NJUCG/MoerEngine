#include "rendergraph/RenderGraph.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rendergraph/RenderGraphResourcePool.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace Moer::Render;

class TestSuite {
public:
    void Check(bool condition, std::string_view test, std::string_view message) {
        if (condition) {
            return;
        }
        ++failures;
        std::cerr << "[FAIL] " << test << ": " << message << '\n';
    }

    [[nodiscard]] int FailureCount() const {
        return failures;
    }

private:
    int failures = 0;
};

class FakeTexture final : public Texture {
public:
    explicit FakeTexture(const RGTransientTextureDesc& desc) :
        Texture(MakeInfo(desc)) {}

    Moer::uint GetMipByteSize(Moer::uint) const override {
        return 4;
    }

    void SetName(std::string_view value) override {
        name.assign(value);
    }

private:
    static TextureInfo MakeInfo(const RGTransientTextureDesc& desc) {
        TextureInfo info{};
        info.dimension    = desc.dimension;
        info.usage        = desc.usage;
        info.format       = desc.format;
        info.extent       = {static_cast<int>(desc.extent.x), static_cast<int>(desc.extent.y)};
        info.depth        = static_cast<uint16_t>(desc.extent.z);
        info.array_size   = static_cast<uint16_t>(desc.PhysicalLayerCount());
        info.num_mips     = static_cast<uint8_t>(desc.mip_count);
        info.aspect_flags = desc.aspect_flags;
        return info;
    }

    std::string name{};
};

class FakeBuffer final : public Buffer {
public:
    explicit FakeBuffer(const RGTransientBufferDesc& desc) :
        Buffer(BufferInfo{
            desc.element_count,
            desc.stride,
            desc.usage,
            desc.format
        }) {}

    void SetName(std::string_view value) override {
        name.assign(value);
    }

private:
    std::string name{};
};

struct FactoryProbe {
    uint32_t textures = 0;
    uint32_t buffers  = 0;

    RenderGraphResourcePool MakePool(uint32_t retire_after_idle_frames = 3) {
        return RenderGraphResourcePool(
            retire_after_idle_frames,
            [this](std::string_view, const RGTransientTextureDesc& desc) {
                ++textures;
                return TextureRef{MoerNew(FakeTexture)(desc)};
            },
            [this](std::string_view, const RGTransientBufferDesc& desc) {
                ++buffers;
                return BufferRef{MoerNew(FakeBuffer)(desc)};
            }
        );
    }
};

const RGTransientBufferDesc kBufferDesc{
    .element_count = 64,
    .stride        = 4,
    .usage         = EBufferUsageFlags::TRANSFER_DST |
                     EBufferUsageFlags::TRANSFER_SRC,
};

const RGTransientTextureDesc kTextureDesc{
    .dimension     = ETextureDimension::TEX_2D,
    .extent        = Extent3D(16, 16, 1),
    .format        = PF_R8G8B8A8_UNORM,
    .usage         = ETextureUsageFlags::TRANSFER_DST |
                     ETextureUsageFlags::TRANSFER_SRC,
    .aspect_flags  = ETextureAspectFlags::COLOR,
    .mip_count     = 1,
    .array_size    = 1,
};

void InvokeCallbacks(Moer::Array<std::function<void()>>& callbacks) {
    for (auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
    callbacks.clear();
}

void CompleteSource(RHIRecordingSource& source) {
    CmdSubmit submit = source.command_list->Submit();
    InvokeCallbacks(submit.callbacks);
    InvokeCallbacks(submit.success_callbacks);
    source.command_list.reset();
}

void CompleteSources(Moer::Array<RHIRecordingSource>& sources) {
    for (auto& source : sources) {
        CompleteSource(source);
    }
    sources.clear();
}

void RejectSources(Moer::Array<RHIRecordingSource>& sources) {
    for (auto& source : sources) {
        auto callbacks =
            source.command_list->DrainOrdinaryCallbacksForRejection();
        InvokeCallbacks(callbacks);
        source.command_list.reset();
    }
    sources.clear();
}

void TestPoolReuseAndIdleRetirement(TestSuite& suite) {
    constexpr std::string_view test = "pool reuse and idle retirement";
    FactoryProbe probe{};
    auto         pool = probe.MakePool(1);

    BufferRef first = pool.AcquireBuffer("First", kBufferDesc);
    Buffer*   first_identity = first.Get();
    BufferRef concurrent = pool.AcquireBuffer("Concurrent", kBufferDesc);
    suite.Check(
        first_identity != concurrent.Get() && probe.buffers == 2,
        test,
        "an in-use allocation was reused"
    );

    first = {};
    BufferRef reused = pool.AcquireBuffer("Reused", kBufferDesc);
    suite.Check(
        reused.Get() == first_identity && probe.buffers == 2,
        test,
        "an idle descriptor-exact allocation was not reused"
    );

    concurrent = {};
    reused     = {};
    pool.Tick();
    suite.Check(pool.BufferCount() == 2, test, "pool retired resources one frame too early");
    pool.Tick();
    suite.Check(pool.BufferCount() == 0, test, "pool did not retire idle resources");
}

void TestActiveTransientLifetimeIsCompletionOwned(TestSuite& suite) {
    constexpr std::string_view test =
        "active transient lifetime is completion owned";
    FactoryProbe probe{};
    auto         pool = probe.MakePool();
    RenderGraphTransientAllocator allocator(pool);
    Buffer* recorded_identity = nullptr;
    Moer::Array<RHIRecordingSource> published{};

    RenderGraph graph("TransientCompletionOwnership");
    const auto transient = graph.CreateTransientBuffer("Scratch", kBufferDesc);
    graph.AddRecordPass(
        "WriteScratch",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                transient,
                RenderGraph::BufferState::TransferDestination
            ).SideEffect();
        },
        [&](CommandList&) {
            recorded_identity = graph.GetPhysicalBuffer(transient).Get();
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test, graph.GetCompileError());
    const auto& compiled_resource =
        graph.GetCompiledPlan().resources[transient.resource.index];
    suite.Check(
        compiled_resource.transient_slot !=
                RenderGraph::PassHandle::InvalidIndex &&
            compiled_resource.first_use == 0 &&
            compiled_resource.last_use == 0,
        test,
        "compiler did not publish a transient allocation slot/lifetime"
    );

    const bool executed = graph.ExecuteRecording(
        {},
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            published = std::move(sources);
        },
        RenderGraph::ActiveRecordingOptions{
            .enabled             = true,
            .transient_allocator = &allocator,
        }
    );
    suite.Check(executed, test, graph.GetCompileError());
    suite.Check(
        recorded_identity != nullptr && published.size() == 1,
        test,
        "transient pass did not receive its physical allocation"
    );
    suite.Check(
        !graph.GetPhysicalBuffer(transient).IsValid(),
        test,
        "non-exported graph ownership was not released after recording"
    );
    suite.Check(
        pool.BufferCount() == 1 && pool.AvailableBufferCount() == 0,
        test,
        "pool exposed an in-flight completion-owned allocation"
    );

    CompleteSources(published);
    suite.Check(
        pool.AvailableBufferCount() == 1,
        test,
        "completion callbacks did not return the allocation to the pool"
    );
    BufferRef reused = pool.AcquireBuffer("NextFrame", kBufferDesc);
    suite.Check(
        reused.Get() == recorded_identity && probe.buffers == 1,
        test,
        "the next completed frame did not reuse the physical allocation"
    );
}

void TestRejectedProducerRetiresThroughOrdinaryCallbacks(TestSuite& suite) {
    constexpr std::string_view test =
        "rejected producer retires through ordinary callbacks";
    FactoryProbe probe{};
    auto         pool = probe.MakePool();
    RenderGraphTransientAllocator allocator(pool);
    Moer::Array<RHIRecordingSource> published{};

    RenderGraph graph("TransientRejectedProducer");
    const auto transient = graph.CreateTransientBuffer("Scratch", kBufferDesc);
    graph.AddRecordPass(
        "FailScratch",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                transient,
                RenderGraph::BufferState::TransferDestination
            ).SideEffect();
        },
        [](CommandList&) {
            throw std::runtime_error("injected record failure");
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test, graph.GetCompileError());
    const bool executed = graph.ExecuteRecording(
        {},
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            published = std::move(sources);
        },
        RenderGraph::ActiveRecordingOptions{
            .enabled             = true,
            .transient_allocator = &allocator,
        }
    );
    suite.Check(!executed, test, "injected record failure unexpectedly committed");
    suite.Check(
        published.size() == 1 && pool.AvailableBufferCount() == 0,
        test,
        "failed producer lost completion ownership before rejection cleanup"
    );

    RejectSources(published);
    suite.Check(
        pool.AvailableBufferCount() == 1,
        test,
        "ordinary rejection callbacks did not retire the allocation"
    );
}

void TestUnusedTransientDoesNotAllocate(TestSuite& suite) {
    constexpr std::string_view test = "unused transient does not allocate";
    FactoryProbe probe{};
    auto         pool = probe.MakePool();
    RenderGraphTransientAllocator allocator(pool);
    Moer::Array<RHIRecordingSource> published{};

    RenderGraph graph("UnusedTransient");
    (void)graph.CreateTransientBuffer("Unused", kBufferDesc);
    BufferRef imported_physical = MoerNew(FakeBuffer)(kBufferDesc);
    const auto imported = graph.ImportBuffer(
        "Imported",
        imported_physical,
        RenderGraph::BufferDesc{.byte_size = kBufferDesc.ByteSize()}
    );
    graph.SetInitialState(
        imported,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.AddRecordPass(
        "SideEffectOnly",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                imported,
                RenderGraph::BufferState::TransferDestination
            ).SideEffect();
        },
        [](CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    graph.Export(
        imported,
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test, graph.GetCompileError());
    const bool executed = graph.ExecuteRecording(
        {},
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            published = std::move(sources);
        },
        RenderGraph::ActiveRecordingOptions{
            .enabled             = true,
            .transient_allocator = &allocator,
        }
    );
    suite.Check(executed, test, graph.GetCompileError());
    suite.Check(
        probe.buffers == 0 && pool.BufferCount() == 0,
        test,
        "unused transient resource was physically allocated"
    );
    CompleteSources(published);
}

void TestNonOverlappingBuffersAliasUntilEveryCompletion(TestSuite& suite) {
    constexpr std::string_view test =
        "non-overlapping buffers alias until every completion";
    FactoryProbe probe{};
    auto         pool = probe.MakePool();
    RenderGraphTransientAllocator allocator(pool);
    Moer::Array<RHIRecordingSource> published{};
    Buffer* first_identity  = nullptr;
    Buffer* second_identity = nullptr;

    RenderGraph graph("TransientBufferAlias");
    const auto first  = graph.CreateTransientBuffer("First", kBufferDesc);
    const auto second = graph.CreateTransientBuffer("Second", kBufferDesc);
    const auto first_pass = graph.AddRecordPass(
        "WriteFirst",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                first,
                RenderGraph::BufferState::TransferDestination
            ).SideEffect();
        },
        [&](CommandList&) {
            first_identity = graph.GetPhysicalBuffer(first).Get();
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    const auto second_pass = graph.AddRecordPass(
        "WriteSecond",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                second,
                RenderGraph::BufferState::TransferDestination
            ).SideEffect();
        },
        [&](CommandList&) {
            second_identity = graph.GetPhysicalBuffer(second).Get();
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test, graph.GetCompileError());
    if (!compiled) {
        return;
    }

    const auto& plan = graph.GetCompiledPlan();
    const auto first_slot =
        plan.resources[first.resource.index].transient_slot;
    const auto second_slot =
        plan.resources[second.resource.index].transient_slot;
    suite.Check(
        first_slot != RenderGraph::PassHandle::InvalidIndex &&
            first_slot == second_slot && plan.alias_boundaries.size() == 1,
        test,
        "compiler did not color non-overlapping descriptor-exact buffers into one slot"
    );

    if (!plan.alias_boundaries.empty()) {
        const auto& alias = plan.alias_boundaries.front();
        suite.Check(
                alias.predecessor_resource == first.resource &&
                alias.successor_resource == second.resource &&
                alias.primary_src_pass == first_pass &&
                alias.source_frontier ==
                    std::vector<RenderGraph::PassHandle>{first_pass} &&
                alias.dst_pass == second_pass &&
                alias.barrier_index < plan.barriers.size(),
            test,
            "compiled alias boundary does not name the expected resources and passes"
        );
        if (alias.barrier_index < plan.barriers.size()) {
            const auto& barrier = plan.barriers[alias.barrier_index];
            suite.Check(
                barrier.transient_alias &&
                    barrier.src_pass == first_pass &&
                    barrier.dst_pass == second_pass &&
                    barrier.before_state ==
                        RenderGraph::ResourceState::Buffer(
                            RenderGraph::BufferState::TransferDestination
                        ) &&
                    barrier.after_state ==
                        RenderGraph::ResourceState::Buffer(
                            RenderGraph::BufferState::TransferDestination
                        ) &&
                    barrier.memory_dependency &&
                    barrier.execution_dependency &&
                    !barrier.discard_previous_contents &&
                    barrier.sources.size() == 1,
                test,
                "alias boundary was not lowered to a predecessor-aware memory barrier"
            );
        }
    }

    const bool has_alias_edge = std::any_of(
        plan.edges.begin(),
        plan.edges.end(),
        [&](const RenderGraph::CompiledEdge& edge) {
            return edge.src == first_pass && edge.dst == second_pass &&
                   std::any_of(
                       edge.reasons.begin(),
                       edge.reasons.end(),
                       [](const RenderGraph::CompiledEdgeReason& reason) {
                           return reason.kind ==
                                  RenderGraph::EdgeReasonKind::TransientAlias;
                       }
                   );
        }
    );
    suite.Check(
        has_alias_edge,
        test,
        "alias reuse did not become a formal compiled dependency"
    );

    const bool executed = graph.ExecuteRecording(
        {},
        {},
        false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            published = std::move(sources);
        },
        RenderGraph::ActiveRecordingOptions{
            .enabled             = true,
            .transient_allocator = &allocator,
        }
    );
    suite.Check(executed, test, graph.GetCompileError());
    suite.Check(
        first_identity != nullptr && first_identity == second_identity &&
            probe.buffers == 1 && published.size() == 2,
        test,
        "logical aliases did not receive one physical buffer"
    );
    suite.Check(
        pool.AvailableBufferCount() == 0,
        test,
        "aliased allocation became reusable before completion"
    );

    if (published.size() == 2) {
        CompleteSource(published[0]);
        suite.Check(
            pool.AvailableBufferCount() == 0,
            test,
            "first owner completion released an allocation still owned by its alias"
        );
        CompleteSource(published[1]);
        suite.Check(
            pool.AvailableBufferCount() == 1,
            test,
            "allocation did not return after every aliased owner completed"
        );
    }
    published.clear();
}

void TestUnsafeAliasCasesRemainDistinct(TestSuite& suite) {
    constexpr std::string_view overlap_test =
        "overlapping transient lifetimes remain distinct";
    {
        FactoryProbe probe{};
        auto         pool = probe.MakePool();
        RenderGraphTransientAllocator allocator(pool);
        RenderGraph graph("OverlappingTransientBuffers");
        const auto first  = graph.CreateTransientBuffer("First", kBufferDesc);
        const auto second = graph.CreateTransientBuffer("Second", kBufferDesc);
        graph.AddRecordPass(
            "WriteFirst",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    first,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );
        graph.AddRecordPass(
            "WriteSecond",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    second,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );
        graph.AddRecordPass(
            "ReadFirst",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Read(
                    first,
                    RenderGraph::BufferState::TransferSource
                ).SideEffect();
            },
            [](CommandList&) {}
        );

        const bool compiled = graph.Compile();
        suite.Check(compiled, overlap_test, graph.GetCompileError());
        if (compiled) {
            const auto& plan = graph.GetCompiledPlan();
            suite.Check(
                plan.resources[first.resource.index].transient_slot !=
                    plan.resources[second.resource.index].transient_slot &&
                    plan.alias_boundaries.empty(),
                overlap_test,
                "overlapping lifetimes were assigned one physical slot"
            );
            std::string error{};
            suite.Check(
                allocator.Prepare(graph, error),
                overlap_test,
                error
            );
            suite.Check(
                graph.GetPhysicalBuffer(first).Get() !=
                        graph.GetPhysicalBuffer(second).Get() &&
                    pool.BufferCount() == 2,
                overlap_test,
                "allocator collapsed distinct overlapping slots"
            );
            allocator.ReleaseNonExported(graph);
        }
    }

    constexpr std::string_view descriptor_test =
        "incompatible transient descriptors remain distinct";
    {
        RenderGraph graph("IncompatibleTransientBuffers");
        auto larger_desc = kBufferDesc;
        larger_desc.element_count *= 2;
        const auto first  = graph.CreateTransientBuffer("First", kBufferDesc);
        const auto second = graph.CreateTransientBuffer("Second", larger_desc);
        graph.AddRecordPass(
            "WriteFirst",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    first,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );
        graph.AddRecordPass(
            "WriteSecond",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    second,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );

        const bool compiled = graph.Compile();
        suite.Check(compiled, descriptor_test, graph.GetCompileError());
        if (compiled) {
            const auto& plan = graph.GetCompiledPlan();
            suite.Check(
                plan.resources[first.resource.index].transient_slot !=
                    plan.resources[second.resource.index].transient_slot &&
                    plan.alias_boundaries.empty(),
                descriptor_test,
                "descriptor-incompatible resources shared a transient slot"
            );
        }
    }

    constexpr std::string_view export_test =
        "exported transient resources do not alias";
    {
        RenderGraph graph("ExportedTransientBuffer");
        const auto exported =
            graph.CreateTransientBuffer("Exported", kBufferDesc);
        const auto later = graph.CreateTransientBuffer("Later", kBufferDesc);
        graph.AddRecordPass(
            "WriteExported",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    exported,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );
        graph.AddRecordPass(
            "WriteLater",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    later,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );
        graph.Export(
            exported,
            RenderGraph::BufferState::TransferSource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );

        const bool compiled = graph.Compile();
        suite.Check(compiled, export_test, graph.GetCompileError());
        if (compiled) {
            const auto& plan = graph.GetCompiledPlan();
            suite.Check(
                plan.resources[exported.resource.index].transient_slot !=
                    plan.resources[later.resource.index].transient_slot &&
                    plan.alias_boundaries.empty(),
                export_test,
                "an exported resource was reused by a later transient"
            );
        }
    }

    constexpr std::string_view reference_test =
        "opaque transient references disable aliasing";
    {
        RenderGraph graph("ReferencedTransientBuffer");
        const auto referenced =
            graph.CreateTransientBuffer("Referenced", kBufferDesc);
        const auto later =
            graph.CreateTransientBuffer("Later", kBufferDesc);
        graph.AddRecordPass(
            "WriteReferenced",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    referenced,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );
        graph.AddRecordPass(
            "ReferenceOnly",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Reference(referenced).SideEffect();
            },
            [](CommandList&) {}
        );
        graph.AddRecordPass(
            "WriteLater",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    later,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );

        const bool compiled = graph.Compile();
        suite.Check(compiled, reference_test, graph.GetCompileError());
        if (compiled) {
            const auto& plan = graph.GetCompiledPlan();
            suite.Check(
                plan.resources[referenced.resource.index].transient_slot !=
                    plan.resources[later.resource.index].transient_slot &&
                    plan.alias_boundaries.empty(),
                reference_test,
                "opaque Reference lifetime was reused by a later transient"
            );
        }
    }
}

void TestNonOverlappingTexturesAlias(TestSuite& suite) {
    constexpr std::string_view test =
        "non-overlapping textures use one descriptor-exact allocation";
    FactoryProbe probe{};
    auto         pool = probe.MakePool();
    RenderGraphTransientAllocator allocator(pool);
    RenderGraph graph("TransientTextureAlias");
    const auto first  = graph.CreateTransientTexture("First", kTextureDesc);
    const auto second = graph.CreateTransientTexture("Second", kTextureDesc);
    graph.AddRecordPass(
        "WriteFirst",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                first,
                RenderGraph::TextureState::TransferDestination
            ).SideEffect();
        },
        [](CommandList&) {}
    );
    graph.AddRecordPass(
        "WriteSecond",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                second,
                RenderGraph::TextureState::TransferDestination
            ).SideEffect();
        },
        [](CommandList&) {}
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test, graph.GetCompileError());
    if (!compiled) {
        return;
    }
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.resources[first.resource.index].transient_slot ==
                plan.resources[second.resource.index].transient_slot &&
            plan.alias_boundaries.size() == 1,
        test,
        "compiler did not alias compatible whole textures"
    );
    std::string error{};
    suite.Check(allocator.Prepare(graph, error), test, error);
    suite.Check(
        graph.GetPhysicalTexture(first).Get() ==
                graph.GetPhysicalTexture(second).Get() &&
            probe.textures == 1,
        test,
        "allocator did not materialize the texture alias slot once"
    );
    allocator.ReleaseNonExported(graph);
}

void TestAliasBarrierCoversCompleteReaderFrontier(TestSuite& suite) {
    constexpr std::string_view test =
        "alias barrier covers complete reader frontier";
    RenderGraph graph("TransientAliasReaderFrontier");
    const auto first  = graph.CreateTransientBuffer("First", kBufferDesc);
    const auto second = graph.CreateTransientBuffer("Second", kBufferDesc);
    graph.AddRecordPass(
        "WriteFirst",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    first,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [](CommandList&) {}
    );
    const auto ray_reader = graph.AddRecordPass(
        "RayReadFirst",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::RayTracing
                   )
                .Read(
                    first,
                    RenderGraph::BufferState::ShaderResource
                )
                .SideEffect();
        },
        [](CommandList&) {}
    );
    const auto graphics_reader = graph.AddRecordPass(
        "GraphicsReadFirst",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Graphics
                   )
                .Read(
                    first,
                    RenderGraph::BufferState::ShaderResource
                )
                .SideEffect();
        },
        [](CommandList&) {}
    );
    const auto successor = graph.AddRecordPass(
        "WriteSecond",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    second,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [](CommandList&) {}
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test, graph.GetCompileError());
    if (!compiled) {
        return;
    }
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.alias_boundaries.size() == 1,
        test,
        "reader-frontier graph did not produce an alias boundary"
    );
    if (plan.alias_boundaries.empty()) {
        return;
    }
    const auto& alias = plan.alias_boundaries.front();
    suite.Check(
        alias.barrier_index < plan.barriers.size(),
        test,
        "alias boundary references an invalid barrier"
    );
    if (alias.barrier_index >= plan.barriers.size()) {
        return;
    }

    const auto& barrier = plan.barriers[alias.barrier_index];
    const auto has_source = [&](RenderGraph::PassHandle pass) {
        return std::any_of(
            barrier.sources.begin(),
            barrier.sources.end(),
            [&](const RenderGraph::CompiledBarrierSource& source) {
                return source.pass == pass;
            }
        );
    };
    suite.Check(
        barrier.sources.size() == 2 && has_source(ray_reader) &&
            has_source(graphics_reader) &&
            barrier.src_pass == graphics_reader,
        test,
        "alias barrier dropped a live reader scope from its source frontier"
    );

    const auto has_alias_edge = [&](RenderGraph::PassHandle source) {
        return std::any_of(
            plan.edges.begin(),
            plan.edges.end(),
            [&](const RenderGraph::CompiledEdge& edge) {
                return edge.src == source && edge.dst == successor &&
                       std::any_of(
                           edge.reasons.begin(),
                           edge.reasons.end(),
                           [](const RenderGraph::CompiledEdgeReason& reason) {
                               return reason.kind ==
                                      RenderGraph::EdgeReasonKind::TransientAlias;
                           }
                       );
            }
        );
    };
    suite.Check(
        has_alias_edge(ray_reader) && has_alias_edge(graphics_reader),
        test,
        "alias dependency edges do not cover every reader frontier source"
    );

    FactoryProbe probe{};
    auto         pool = probe.MakePool();
    RenderGraphTransientAllocator allocator(pool);
    std::string allocation_error{};
    suite.Check(
        allocator.Prepare(graph, allocation_error),
        test,
        allocation_error
    );
    RenderGraphLowering::LoweredPlan lowered{};
    std::string lowering_error{};
    suite.Check(
        RenderGraphLowering::Lower(graph, lowered, lowering_error),
        test,
        lowering_error
    );
    const auto before = lowered.Before(successor);
    const auto lowered_alias = std::find_if(
        before.begin(),
        before.end(),
        [](const RenderGraphLowering::LoweredInstruction& instruction) {
            return instruction.transient_alias;
        }
    );
    suite.Check(
        lowered_alias != before.end(),
        test,
        "lowering dropped the alias barrier"
    );
    if (lowered_alias != before.end()) {
        const uint32_t stages =
            static_cast<uint32_t>(lowered_alias->source.stages);
        const uint32_t ray_stage = static_cast<uint32_t>(
            ERHIPipelineStageFlags::PS_RAY_TRACING_SHADER
        );
        const uint32_t graphics_stage = static_cast<uint32_t>(
            ERHIPipelineStageFlags::PS_ALL_GRAPHICS
        );
        suite.Check(
            (stages & ray_stage) != 0 &&
                (stages & graphics_stage) != 0,
            test,
            "lowering did not merge every reader pipeline stage"
        );
    }
    allocator.ReleaseNonExported(graph);
}

void TestAliasChainUsesOneSlot(TestSuite& suite) {
    constexpr std::string_view test = "three-resource alias chain uses one slot";
    RenderGraph graph("TransientAliasChain");
    const auto first  = graph.CreateTransientBuffer("First", kBufferDesc);
    const auto second = graph.CreateTransientBuffer("Second", kBufferDesc);
    const auto third  = graph.CreateTransientBuffer("Third", kBufferDesc);
    const auto add_write = [&](std::string_view name, RenderGraph::BufferHandle buffer) {
        graph.AddRecordPass(
            name,
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    buffer,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );
    };
    add_write("WriteFirst", first);
    add_write("WriteSecond", second);
    add_write("WriteThird", third);

    const bool compiled = graph.Compile();
    suite.Check(compiled, test, graph.GetCompileError());
    if (!compiled) {
        return;
    }
    const auto& plan = graph.GetCompiledPlan();
    const uint32_t slot = plan.resources[first.resource.index].transient_slot;
    suite.Check(
        slot != RenderGraph::PassHandle::InvalidIndex &&
            plan.resources[second.resource.index].transient_slot == slot &&
            plan.resources[third.resource.index].transient_slot == slot &&
            plan.alias_boundaries.size() == 2,
        test,
        "compiler did not build the expected adjacent alias chain"
    );
}

void TestMainThreadReferenceRetainsCompletionLifetime(TestSuite& suite) {
    constexpr std::string_view test =
        "main-thread reference retains completion lifetime";
    FactoryProbe probe{};
    auto         pool = probe.MakePool();
    RenderGraphTransientAllocator allocator(pool);
    RenderGraph graph("MainThreadReferenceLifetime");
    const auto transient =
        graph.CreateTransientBuffer("Referenced", kBufferDesc);
    graph.AddPass(
        "WriteReferenced",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                transient,
                RenderGraph::BufferState::TransferDestination
            ).SideEffect();
        },
        [] {}
    );
    graph.AddPass(
        "ReferenceOnly",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Reference(transient).SideEffect();
        },
        [] {}
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test, graph.GetCompileError());
    CommandList main_commands(EQueueType::Graphics);
    const bool executed = compiled && graph.ExecuteRecording(
        {},
        {},
        false,
        {},
        RenderGraph::ActiveRecordingOptions{
            .enabled                  = true,
            .main_thread_command_list = &main_commands,
            .transient_allocator      = &allocator,
        }
    );
    suite.Check(executed, test, graph.GetCompileError());
    suite.Check(
        !graph.GetPhysicalBuffer(transient).IsValid() &&
            pool.BufferCount() == 1 &&
            pool.AvailableBufferCount() == 0,
        test,
        "reference-only MainThread pass did not transfer ownership to Completion"
    );
    if (executed) {
        CmdSubmit submit = main_commands.Submit();
        InvokeCallbacks(submit.callbacks);
        suite.Check(
            pool.AvailableBufferCount() == 0,
            test,
            "ordinary callbacks released a reference before the success tail"
        );
        InvokeCallbacks(submit.success_callbacks);
        suite.Check(
            pool.AvailableBufferCount() == 1,
            test,
            "success Completion did not retire the reference-only lifetime"
        );
    }
}

void TestTransientExecutionContractsAndRollback(TestSuite& suite) {
    constexpr std::string_view reference_producer_test =
        "referenced transient requires a producer";
    {
        RenderGraph graph("UnproducedReferencedTransient");
        const auto transient =
            graph.CreateTransientBuffer("Scratch", kBufferDesc);
        graph.AddRecordPass(
            "ReferenceOnly",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Reference(transient).SideEffect();
            },
            [](CommandList&) {}
        );
        const bool compiled = graph.Compile();
        suite.Check(
            !compiled &&
                graph.GetCompileError().find("no producer") !=
                    std::string::npos,
            reference_producer_test,
            "opaque Reference accepted an uninitialized transient"
        );
    }

    constexpr std::string_view disabled_test =
        "allocation-backed transient rejects inactive recording";
    {
        RenderGraph graph("InactiveTransientRecording");
        const auto transient =
            graph.CreateTransientBuffer("Scratch", kBufferDesc);
        graph.AddRecordPass(
            "WriteScratch",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    transient,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [](CommandList&) {}
        );
        const bool compiled = graph.Compile();
        suite.Check(compiled, disabled_test, graph.GetCompileError());
        const bool executed = compiled && graph.ExecuteRecording({});
        suite.Check(
            !executed &&
                graph.GetCompileError().find("active recording") !=
                    std::string::npos &&
                !graph.GetPhysicalBuffer(transient).IsValid(),
            disabled_test,
            "inactive recording exposed an unallocated transient"
        );
    }

    constexpr std::string_view serial_test =
        "allocation-backed transient rejects serial execute";
    {
        bool callback_ran = false;
        RenderGraph graph("SerialTransientExecute");
        const auto transient =
            graph.CreateTransientBuffer("Scratch", kBufferDesc);
        graph.AddPass(
            "WriteScratch",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(
                    transient,
                    RenderGraph::BufferState::TransferDestination
                ).SideEffect();
            },
            [&] { callback_ran = true; }
        );
        const bool compiled = graph.Compile();
        suite.Check(compiled, serial_test, graph.GetCompileError());
        const bool executed = compiled && graph.Execute();
        suite.Check(
            !executed && !callback_ran &&
                graph.GetCompileError().find("active ExecuteRecording") !=
                    std::string::npos,
            serial_test,
            "serial Execute ran an allocation-backed transient callback"
        );
    }

    constexpr std::string_view rollback_test =
        "lowering failure rolls back transient bindings";
    {
        FactoryProbe probe{};
        auto         pool = probe.MakePool();
        RenderGraphTransientAllocator allocator(pool);
        RenderGraph graph("TransientLoweringRollback");
        const auto transient =
            graph.CreateTransientBuffer("ComputeScratch", kBufferDesc);
        graph.AddRecordPass(
            "ComputeWrite",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Compute,
                           RenderGraph::PipelineType::Compute
                       )
                    .Write(
                        transient,
                        RenderGraph::BufferState::UnorderedAccess
                    )
                    .SideEffect();
            },
            [](CommandList&) {}
        );
        const bool compiled = graph.Compile();
        suite.Check(compiled, rollback_test, graph.GetCompileError());
        const bool executed = compiled && graph.ExecuteRecording(
            {},
            {},
            false,
            {},
            RenderGraph::ActiveRecordingOptions{
                .enabled             = true,
                .transient_allocator = &allocator,
            }
        );
        suite.Check(
            !executed &&
                graph.GetCompileError().find("Graphics") !=
                    std::string::npos &&
                !graph.GetPhysicalBuffer(transient).IsValid() &&
                pool.BufferCount() == 1 &&
                pool.AvailableBufferCount() == 1,
            rollback_test,
            "failure after Prepare retained a non-exported physical binding"
        );
    }
}

} // namespace

int main() {
    TestSuite suite{};
    TestPoolReuseAndIdleRetirement(suite);
    TestActiveTransientLifetimeIsCompletionOwned(suite);
    TestRejectedProducerRetiresThroughOrdinaryCallbacks(suite);
    TestUnusedTransientDoesNotAllocate(suite);
    TestNonOverlappingBuffersAliasUntilEveryCompletion(suite);
    TestUnsafeAliasCasesRemainDistinct(suite);
    TestNonOverlappingTexturesAlias(suite);
    TestAliasBarrierCoversCompleteReaderFrontier(suite);
    TestAliasChainUsesOneSlot(suite);
    TestMainThreadReferenceRetainsCompletionLifetime(suite);
    TestTransientExecutionContractsAndRollback(suite);

    if (suite.FailureCount() != 0) {
        std::cerr << suite.FailureCount()
                  << " render graph resource pool contract(s) failed\n";
        return 1;
    }
    std::cout << "Render graph resource pool contracts passed\n";
    return 0;
}
