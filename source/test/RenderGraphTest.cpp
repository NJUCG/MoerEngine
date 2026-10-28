#include "rendergraph/RenderGraph.h"
#include "rendergraph/RenderGraphLowering.h"
#include "renderer/common/UiFrameGraphPass.h"
#include "rhi/RHIGpuScope.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIQuery.h"
#include "rhi/plugin/NrdPlugin.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Moer::Render {

// Token absence, unresolved state and undefined GPU contents remain distinct.
static_assert(RenderGraph::GetStateKind({}) == RenderGraph::ResourceKind::Token);
static_assert(RenderGraph::IsUnspecified({}) && !RenderGraph::IsUndefined({}));
static_assert(RenderGraph::IsUnspecified(RenderGraph::TextureAccessType::Unspecified));
static_assert(RenderGraph::IsUnspecified(RenderGraph::BufferAccessType::Unspecified));
static_assert(RenderGraph::IsUndefined(RenderGraph::TextureBoundary::Undefined));
static_assert(RenderGraph::IsUndefined(RenderGraph::BufferBoundary::Undefined));
static_assert(
    RenderGraph::ResourceState{RenderGraph::TextureAccessType::TransferSource} !=
    RenderGraph::ResourceState{RenderGraph::BufferAccessType::TransferSource}
);

class UiFrameGraphPassTestAccess {
public:
    [[nodiscard]] static bool
    BeginRecording(const UiFrameGraphPass::PreparedFrame& frame) {
        return frame.BeginRecording();
    }

    static void FinishRecording(
        const UiFrameGraphPass::PreparedFrame& frame
    ) {
        frame.FinishRecording();
    }

    [[nodiscard]] static bool AddPassesWithComposeRecorder(
        RenderGraph&                          graph,
        std::function<void(CommandList&)>     compose_recorder,
        const UiFrameGraphPass::PreparedFrameRef& frame,
        RenderGraph::TextureHandle            scene_color_handle,
        const TextureRef&                     scene_color,
        const TextureRef&                     main_output,
        RenderGraph::TokenHandle              presentation_ready,
        UiFrameGraphPass::GraphPasses&        passes
    ) {
        return UiFrameGraphPass::AddPassesWithComposeRecorder(
            graph,
            std::move(compose_recorder),
            frame,
            scene_color_handle,
            scene_color,
            main_output,
            presentation_ready,
            passes
        );
    }

};

} // namespace Moer::Render

namespace {

using Moer::Render::RenderGraph;

class TestSuite {
public:
    void Check(bool condition, std::string_view test_name, std::string_view expectation) {
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

[[nodiscard]] Moer::Render::GpuScopeStreamConfig
RenderGraphGpuScopeConfig() {
    return Moer::Render::GpuScopeStreamConfig{
        .max_resident_frames = 4,
        .max_pending_frames = 4,
        .max_resident_scopes = 32,
        .max_scopes_per_frame = 16,
        .max_sources_per_frame = 16,
        .max_scope_name_bytes = 128,
        .max_error_reason_bytes = 128,
    };
}

[[nodiscard]] Moer::Render::TimestampQueryResult
RenderGraphTimestamp(uint64_t begin_tick, uint64_t end_tick) {
    return Moer::Render::TimestampQueryResult{
        .begin_tick = begin_tick,
        .end_tick = end_tick,
        .valid_bits = 64,
        .tick_period_ns = 1.0,
        .duration_ns = static_cast<double>(end_tick - begin_tick),
    };
}

[[nodiscard]] bool ResolveRenderGraphTimestamp(
    Moer::Render::CmdSubmit& submit,
    uint64_t                 begin_tick,
    uint64_t                 end_tick
) {
    return submit.query_tokens.size() == 1 &&
           Moer::Render::QueryBackendAccess::ResolveTimestamp(
               submit.query_tokens.front(),
               RenderGraphTimestamp(begin_tick, end_tick)
           );
}

void ReleaseRenderGraphSubmit(Moer::Render::CmdSubmit& submit) {
    submit.callbacks.clear();
    submit.query_tokens.clear();
}

[[nodiscard]] bool HasEdgeReason(
    const RenderGraph::CompiledPlan& plan,
    RenderGraph::PassHandle          src,
    RenderGraph::PassHandle          dst,
    RenderGraph::EdgeReasonKind      kind,
    RenderGraph::ResourceHandle      resource = {}
) {
    for (const auto& edge : plan.edges) {
        if (edge.src != src || edge.dst != dst) {
            continue;
        }
        for (const auto& reason : edge.reasons) {
            if (reason.kind == kind && (!resource.IsValid() || reason.resource == resource)) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool DependencyLevelContains(
    const RenderGraph::CompiledDependencyLevel& level,
    RenderGraph::PassHandle                     pass
) {
    return std::find(level.passes.begin(), level.passes.end(), pass) !=
           level.passes.end();
}

[[nodiscard]] const RenderGraph::CompiledBarrier* FindBarrier(
    const RenderGraph::CompiledPlan& plan,
    RenderGraph::ResourceHandle      resource,
    RenderGraph::PassHandle          src,
    RenderGraph::PassHandle          dst
) {
    const auto found = std::find_if(
        plan.barriers.begin(),
        plan.barriers.end(),
        [&](const RenderGraph::CompiledBarrier& barrier) {
            return barrier.resource == resource && barrier.src_pass == src && barrier.dst_pass == dst;
        }
    );
    return found == plan.barriers.end() ? nullptr : &*found;
}

[[nodiscard]] const RenderGraph::CompiledAccess* FindAccess(
    const RenderGraph::CompiledPlan& plan,
    RenderGraph::PassHandle          pass,
    RenderGraph::ResourceHandle      resource
) {
    const auto found = std::find_if(
        plan.accesses.begin(),
        plan.accesses.end(),
        [&](const RenderGraph::CompiledAccess& access) {
            return access.pass == pass && access.resource == resource;
        }
    );
    return found == plan.accesses.end() ? nullptr : &*found;
}

[[nodiscard]] bool HasBarrierSource(
    const RenderGraph::CompiledBarrier& barrier,
    RenderGraph::PassHandle             pass,
    RenderGraph::AccessMode             access
) {
    return std::any_of(
        barrier.sources.begin(),
        barrier.sources.end(),
        [&](const RenderGraph::CompiledBarrierSource& source) {
            return source.pass == pass && source.access == access;
        }
    );
}

// Logical graph tests record owned streams without submitting native GPU work.
bool RecordGraphForTest(RenderGraph& graph) {
    return graph.ExecuteFrontendRecordingPlan(
        {}, false, [](Moer::Array<Moer::Render::RHIRecordingSource>&&) {}
    );
}

void TestStableSerialCallbackOrder(TestSuite& suite) {
    constexpr std::string_view test_name = "stable serial callback order";
    RenderGraph                graph("StableSerial");
    std::vector<int>           callback_order;

    graph.AddRecordPass(
        "First",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&](Moer::Render::CommandList&) {
            callback_order.push_back(1);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "Second",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&](Moer::Render::CommandList&) {
            callback_order.push_back(2);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "Third",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&](Moer::Render::CommandList&) {
            callback_order.push_back(3);
        },
        RenderGraph::PassRecordPolicy::Serial
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const bool executed = compiled && RecordGraphForTest(graph);
    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        callback_order == std::vector<int>{1, 2, 3},
        test_name,
        "callbacks must execute exactly once in declaration order"
    );
}

void TestMergedRecordingSharesStagingStreamAndPreservesSubmitOrder(
    TestSuite& suite
) {
    constexpr std::string_view test_name =
        "merged serial recording shares a staging stream and preserves submit order";
    RenderGraph                 graph("MergedRecording");
    std::vector<int>            callback_order;
    std::array<Moer::Render::CommandList*, 2> observed_command_lists{};

    graph.AddRecordPass(
        "First",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&](Moer::Render::CommandList& command_list) {
            observed_command_lists[0] = &command_list;
            command_list.AddCallback(
                [&callback_order] { callback_order.push_back(1); }
            );
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "Second",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&](Moer::Render::CommandList& command_list) {
            observed_command_lists[1] = &command_list;
            command_list.AddCallback(
                [&callback_order] { callback_order.push_back(2); }
            );
        },
        RenderGraph::PassRecordPolicy::Serial
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    Moer::Render::CommandList command_list(
        Moer::Render::EQueueType::Graphics
    );
    const bool executed =
        compiled && graph.RecordAndMergeFrontendCommands(command_list);
    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        observed_command_lists[0] != nullptr &&
            observed_command_lists[1] != nullptr &&
            observed_command_lists[0] == observed_command_lists[1] &&
            observed_command_lists[0] != &command_list &&
            observed_command_lists[1] != &command_list,
        test_name,
        "serial callbacks must share a staging CommandList distinct from the destination"
    );
    suite.Check(
        !command_list.IsEmpty(),
        test_name,
        "recorded payloads must be merged into the caller-owned CommandList"
    );

    Moer::Render::CmdSubmit merged_submit = command_list.Submit();
    suite.Check(
        merged_submit.segments.size() == 1,
        test_name,
        "the merged destination must produce one submit segment"
    );
    for (auto& callback : merged_submit.callbacks) {
        if (callback) {
            callback();
        }
    }
    merged_submit.callbacks.clear();
    suite.Check(
        callback_order == std::vector<int>{1, 2},
        test_name,
        "frontend callback payload must be concatenated in compiled order"
    );
    suite.Check(
        !graph.RecordAndMergeFrontendCommands(command_list),
        test_name,
        "merged execution must preserve the graph one-shot contract"
    );
}

void TestMergedRecordingFailureLeavesDestinationUntouched(TestSuite& suite, bool compiled_ownership = false) {
    constexpr std::string_view test_name =
        "merged recording failure leaves destination untouched";
    RenderGraph graph("MergedRecordingFailure");

    graph.AddRecordPass(
        "RecordedPrefix",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList& command_list) {
            command_list.AddCallback([] {});
        },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    graph.AddRecordPass(
        "FailingPass",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {
            throw std::runtime_error("injected recording failure");
        },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    Moer::Render::CommandList destination(
        Moer::Render::EQueueType::Graphics
    );
    if (compiled_ownership) {
        destination.SetResourceStateOwnership(Moer::Render::ERHIResourceStateOwnership::Compiled);
    }
    int preserved_callbacks = 0;
    destination.AddCallback([&] { ++preserved_callbacks; });
    const bool executed =
        compiled && graph.RecordAndMergeFrontendCommands(destination, false);
    suite.Check(
        !executed,
        test_name,
        "a failed record callback must fail the whole frontend dispatch group"
    );
    suite.Check(
        !destination.IsEmpty(),
        test_name,
        "a failed graph must not clear pre-existing destination commands"
    );
    suite.Check(
        graph.GetCompileError().find("FailingPass") != std::string::npos,
        test_name,
        "the failing pass name must be preserved in the diagnostic"
    );
    auto submit = destination.Submit();
    suite.Check(submit.callbacks.size() == 1, test_name,
                "a failed shared stream must not append its recorded prefix");
    for (auto& callback : submit.callbacks) { callback(); }
    suite.Check(preserved_callbacks == 1, test_name, "destination callback was lost or duplicated");
}

void TestFrontendCommandListMergeRebasesCachedArguments(TestSuite& suite) {
    constexpr std::string_view test_name =
        "frontend CommandList merge rebases cached arguments";
    using namespace Moer::Render;

    CommandList destination(EQueueType::Graphics);
    CommandList source(EQueueType::Graphics);
    static_cast<void>(destination.RegisterArgs(ArrayArguments{}));
    const ArrayArgReference source_args =
        source.RegisterArgs(ArrayArguments{});
    PipelineHandle pipeline{};
    source.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::SerialControl
    );
    source.AddCustomCommand(
        Moer::MakeUnique<DispatchCmd>(
            TShaderArgArray(source_args),
            pipeline,
            Moer::uint3(1, 1, 1),
            ProfileSection("Other")
        ),
        "CachedDispatch"
    );

    destination.AppendRecorded(std::move(source));
    CmdSubmit submit = destination.Submit();
    suite.Check(
        submit.cached_args.size() == 2 && submit.cmds.size() == 1 &&
            submit.translate_execution_class ==
                ERHITranslateExecutionClass::SerialControl,
        test_name,
        "cache, command, and stronger translation policy must survive the merge"
    );
    if (submit.cached_args.size() == 2 && submit.cmds.size() == 1) {
        const auto& dispatch =
            static_cast<const DispatchCmd&>(*submit.cmds.front());
        suite.Check(
            &dispatch.Args(submit.cached_args) == &submit.cached_args[1],
            test_name,
            "source-local cached argument index must be rebased"
        );
    }
}

void TestImportedAliasIdentityAndDump(TestSuite& suite) {
    constexpr std::string_view test_name = "imported resource alias";
    RenderGraph                graph("AliasIdentity");
    int                        physical_resource = 0;

    const auto texture = graph.ImportTexture(
        "SceneColor", &physical_resource, RenderGraph::TextureDesc{.mip_count = 4, .layer_count = 1}
    );
    const auto texture_view = graph.ImportTexture(
        "SceneColorView", &physical_resource, RenderGraph::TextureDesc{.mip_count = 4, .layer_count = 1}
    );
    suite.Check(
        texture == texture_view, test_name, "aliases of one physical identity must return one handle"
    );

    const auto repeated = graph.ImportTexture(
        "SceneColor", &physical_resource, RenderGraph::TextureDesc{.mip_count = 4, .layer_count = 1}
    );
    suite.Check(
        texture == repeated, test_name, "repeated imports of the same named object must be idempotent"
    );

    graph.AddRecordPass(
        "ReadSceneColor",
        [texture_view](RenderGraph::PassBuilder& builder) {
            builder.Read(texture_view);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const std::string dump = graph.Dump();
    suite.Check(Contains(dump, "resources=1"), test_name, "an alias must not create a second resource");
    suite.Check(
        Contains(dump, "SceneColor kind=texture") && Contains(dump, "aliases=[SceneColorView]"),
        test_name,
        "dump must report the canonical resource and its alias"
    );
}

void TestResourceNameAndIdentityNamespaces(TestSuite& suite) {
    constexpr std::string_view test_name = "resource name and identity namespaces";
    int physical_a = 0;
    int physical_b = 0;
    for (bool alias_second : {false, true}) {
        RenderGraph graph("NameCollision");
        const auto first = graph.Import("First", RenderGraph::ResourceKind::Texture, &physical_a);
        const auto second = graph.Import("Second", RenderGraph::ResourceKind::Texture, &physical_b);
        const auto* identity = alias_second ? &physical_b : &physical_a;
        const auto alias = graph.Import("Alias", RenderGraph::ResourceKind::Texture, identity);
        suite.Check(alias == (alias_second ? second : first), test_name,
                    "a new alias must preserve resource identity");
        suite.Check(!graph.Import(alias_second ? "First" : "Second",
                                  RenderGraph::ResourceKind::Texture, identity).IsValid(),
                    test_name, "an existing physical identity must not hide another resource's name");
        suite.Check(!graph.CreateTransient("Alias", RenderGraph::ResourceKind::Texture).IsValid(),
                    test_name, "an imported alias reserves its name against transient resources");
        suite.Check(!graph.Import("Alias", RenderGraph::ResourceKind::Texture,
                                  alias_second ? &physical_a : &physical_b).IsValid(),
                    test_name, "aliases must reject a different physical identity");
    }
    RenderGraph graph("NullIdentities");
    const auto first = graph.ImportToken("First");
    const auto second = graph.ImportToken("Second");
    suite.Check(first != second, test_name, "null physical identities must not alias unrelated resources");
    suite.Check(!graph.ImportToken("First").IsValid(), test_name, "a null identity cannot justify name reuse");
    graph.CreateTransient("Transient", RenderGraph::ResourceKind::Buffer);
    suite.Check(!graph.Import("Transient", RenderGraph::ResourceKind::Buffer, &physical_a).IsValid(),
                test_name, "transient names share the declaration namespace");
}

[[nodiscard]] std::string BuildHazardDump(TestSuite& suite, std::string_view test_name) {
    RenderGraph graph("Hazards");
    int         physical_buffer = 0;
    const auto  shared          = graph.Import("Shared", RenderGraph::ResourceKind::Buffer, &physical_buffer);

    graph.AddRecordPass(
        "Produce",
        [shared](RenderGraph::PassBuilder& builder) {
            builder.Write(shared);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "Consume",
        [shared](RenderGraph::PassBuilder& builder) {
            builder.Read(shared);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "Overwrite",
        [shared](RenderGraph::PassBuilder& builder) {
            builder.Write(shared);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    return graph.Dump();
}

void TestDependencyLevelOrdering(TestSuite& suite) {
    constexpr std::string_view test_name = "dependency levels preserve fan-in depth and declaration order";
    RenderGraph graph("DependencyLevels");
    const auto add_pass = [&](std::string_view name, std::initializer_list<RenderGraph::PassHandle> dependencies) {
        return graph.AddRecordPass(
            name,
            [dependencies](RenderGraph::PassBuilder& builder) {
                for (const auto dependency : dependencies) {
                    builder.DependsOn(dependency);
                }
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::ParallelAllowed
        );
    };
    const auto root = add_pass("Root", {});
    const auto independent = add_pass("Independent", {});
    const auto chain = add_pass("Chain", {root});
    const auto fan_in = add_pass("FanIn", {chain, independent});
    const auto short_branch = add_pass("ShortBranch", {root});
    const auto join = add_pass("Join", {fan_in, short_branch});
    const auto late_independent = add_pass("LateIndependent", {});
    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    if (!compiled) {
        return;
    }
    const auto& plan = graph.GetCompiledPlan();
    const std::vector<RenderGraph::PassHandle> declaration_order{
        root, independent, chain, fan_in, short_branch, join, late_independent
    };
    suite.Check(plan.execution_order == declaration_order, test_name,
                "newly ready lower indices must precede older ready higher indices");
    const std::vector<std::vector<RenderGraph::PassHandle>> expected_levels{
        {root, independent, late_independent}, {chain, short_branch}, {fan_in}, {join}
    };
    suite.Check(plan.dependency_levels.size() == expected_levels.size(), test_name,
                "fan-in must wait for its deepest predecessor");
    for (size_t index = 0; index < std::min(plan.dependency_levels.size(), expected_levels.size()); ++index) {
        suite.Check(plan.dependency_levels[index].passes == expected_levels[index], test_name,
                    "each dependency wave must preserve declaration order");
    }
}

void TestExplicitDependencyOrderRejections(TestSuite& suite) {
    for (const bool cycle : {false, true}) {
        const std::string_view test_name = cycle ?
            "explicit dependency cycle rejection" : "reverse declaration dependency rejection";
        RenderGraph graph(cycle ? "DependencyCycle" : "ReverseDependency");
        std::optional<RenderGraph::PassBuilder> first_builder;
        int callback_count = 0;
        const auto record = [&](Moer::Render::CommandList&) { ++callback_count; };
        const auto first = graph.AddRecordPass(
            "First",
            [&](RenderGraph::PassBuilder& builder) { first_builder.emplace(builder); },
            record,
            RenderGraph::PassRecordPolicy::Serial
        );
        const auto second = graph.AddRecordPass(
            "Second",
            [=](RenderGraph::PassBuilder& builder) {
                if (cycle) {
                    builder.DependsOn(first);
                }
            },
            record,
            RenderGraph::PassRecordPolicy::Serial
        );
        // A copied public builder still refers to the live graph declaration.
        // Add the backward edge only after both real pass handles exist.
        first_builder->DependsOn(second);

        const std::string_view expected_error = cycle ?
            "pass dependency cycle detected" :
            "serial declaration order cannot satisfy a compiled dependency";
        for (uint32_t attempt = 0; attempt < 2; ++attempt) {
            const bool compiled = graph.Compile();
            suite.Check(!compiled && !graph.IsCompiled(), test_name,
                        "an unsupported dependency order must not publish an executable plan");
            suite.Check(Contains(graph.GetCompileError(), expected_error), test_name,
                        "a cycle and an acyclic backward dependency must retain distinct diagnostics");
        }
        const bool recorded = RecordGraphForTest(graph);
        suite.Check(!recorded && callback_count == 0, test_name,
                    "a rejected graph must not execute recording callbacks");
    }
}

void TestHazardsAndDeterministicDump(TestSuite& suite) {
    constexpr std::string_view test_name   = "hazards and deterministic dump";
    const std::string          first_dump  = BuildHazardDump(suite, test_name);
    const std::string          second_dump = BuildHazardDump(suite, test_name);

    suite.Check(
        first_dump == second_dump, test_name, "equivalent graph declarations must produce identical dumps"
    );
    suite.Check(
        Contains(first_dump, "Produce -> Consume reasons=[RAW:Shared@"),
        test_name,
        "dump must report the RAW dependency"
    );
    suite.Check(
        Contains(first_dump, "Consume -> Overwrite reasons=[WAR:Shared@"),
        test_name,
        "dump must report the WAR dependency"
    );
    suite.Check(
        Contains(first_dump, "Produce -> Overwrite reasons=[WAW:Shared@"),
        test_name,
        "dump must report the WAW dependency"
    );
}

void TestTransientReadBeforeProducerFailsWithoutCallbacks(TestSuite& suite) {
    constexpr std::string_view test_name = "transient first read rejection";
    RenderGraph                graph("TransientFirstRead");
    const auto transient      = graph.CreateTransient("Scratch", RenderGraph::ResourceKind::Texture);
    int        callback_count = 0;

    graph.AddRecordPass(
        "InvalidRead",
        [transient](RenderGraph::PassBuilder& builder) {
            builder.Read(transient);
        },
        [&](Moer::Render::CommandList&) {
            ++callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(
        !graph.Compile(), test_name, "Compile must reject a transient read before its first producer"
    );
    suite.Check(
        Contains(graph.GetCompileError(), "read before its first producer"),
        test_name,
        "Compile must provide a useful first-read diagnostic"
    );
    suite.Check(!RecordGraphForTest(graph), test_name, "Execute must reject an uncompiled graph");
    suite.Check(callback_count == 0, test_name, "a rejected graph must execute zero callbacks");
}

void TestCrossGraphHandlesAreRejected(TestSuite& suite) {
    constexpr std::string_view resource_test_name = "cross-graph resource handle rejection";
    RenderGraph                owner_graph("ResourceOwner");
    const auto foreign_resource = owner_graph.Import("Foreign", RenderGraph::ResourceKind::Buffer);

    RenderGraph consumer_graph("ResourceConsumer");
    int         resource_callback_count = 0;
    consumer_graph.AddRecordPass(
        "InvalidResourceUser",
        [foreign_resource](RenderGraph::PassBuilder& builder) {
            builder.Read(foreign_resource);
        },
        [&](Moer::Render::CommandList&) {
            ++resource_callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !consumer_graph.Compile(), resource_test_name, "Compile must reject a foreign resource handle"
    );
    suite.Check(
        Contains(consumer_graph.GetCompileError(), "invalid resource"),
        resource_test_name,
        "foreign resource rejection must be diagnosed"
    );
    suite.Check(
        !RecordGraphForTest(consumer_graph), resource_test_name, "a graph with a foreign resource must not execute"
    );
    suite.Check(
        resource_callback_count == 0, resource_test_name, "foreign resource graph must run zero callbacks"
    );

    constexpr std::string_view pass_test_name = "cross-graph pass handle rejection";
    RenderGraph                pass_owner_graph("PassOwner");
    const auto                 foreign_pass = pass_owner_graph.AddRecordPass("ForeignPass", {}, [](Moer::Render::CommandList&) {}, RenderGraph::PassRecordPolicy::Serial);

    RenderGraph dependent_graph("PassConsumer");
    int         pass_callback_count = 0;
    dependent_graph.AddRecordPass(
        "InvalidDependent",
        [foreign_pass](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(foreign_pass);
        },
        [&](Moer::Render::CommandList&) {
            ++pass_callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(!dependent_graph.Compile(), pass_test_name, "Compile must reject a foreign pass handle");
    suite.Check(
        Contains(dependent_graph.GetCompileError(), "invalid pass handle") ||
            Contains(dependent_graph.GetCompileError(), "invalid pass"),
        pass_test_name,
        "foreign pass rejection must be diagnosed"
    );
    suite.Check(
        !RecordGraphForTest(dependent_graph), pass_test_name, "a graph with a foreign dependency must not execute"
    );
    suite.Check(pass_callback_count == 0, pass_test_name, "foreign pass graph must run zero callbacks");
}

void TestRecompileBeforeOneShotExecution(TestSuite& suite) {
    constexpr std::string_view test_name = "recompile before one-shot execution";
    RenderGraph                graph("OneShot");
    int                        callback_count = 0;
    const auto                 token = graph.CreateTransientToken("Ready");
    const auto producer = graph.AddRecordPass(
        "Producer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(token);
        },
        [&](Moer::Render::CommandList&) {
            ++callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );

    bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const std::string first_dump = graph.Dump();
    compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    suite.Check(
        graph.Dump() == first_dump && callback_count == 0,
        test_name,
        "repeated compilation must rebuild an identical plan without recording callbacks"
    );

    const auto consumer = graph.AddRecordPass(
        "Consumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(token);
        },
        [&](Moer::Render::CommandList&) {
            ++callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !graph.IsCompiled() && graph.GetCompiledPlan().execution_order.empty(),
        test_name,
        "adding a declaration must invalidate and clear the previous compiled plan"
    );
    compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    suite.Check(
        graph.GetCompiledPlan().execution_order.size() == 2 &&
            HasEdgeReason(
                graph.GetCompiledPlan(), producer, consumer,
                RenderGraph::EdgeReasonKind::ReadAfterWrite, token.Untyped()
            ) && Contains(graph.Dump(), "Producer -> Consumer reasons=[RAW:Ready@"),
        test_name,
        "recompilation must retain declarations and publish the added dependency"
    );
    const size_t compiled_pass_count = graph.GetCompiledPlan().execution_order.size();
    suite.Check(RecordGraphForTest(graph), test_name, graph.GetCompileError());
    suite.Check(callback_count == 2, test_name, "the first Execute must run each callback once");
    suite.Check(!RecordGraphForTest(graph), test_name, "a second Execute must be rejected");
    suite.Check(callback_count == 2, test_name, "a rejected second Execute must not repeat callbacks");
    suite.Check(!graph.Compile(), test_name, "Compile after execution must be rejected");
    suite.Check(callback_count == 2, test_name, "a rejected post-execution Compile must not repeat callbacks");
    suite.Check(
        graph.IsCompiled() && graph.GetCompiledPlan().execution_order.size() == compiled_pass_count,
        test_name,
        "a rejected post-execution Compile must preserve the valid executed plan for diagnostics"
    );
}

void TestLifetimeAndExportDump(TestSuite& suite) {
    constexpr std::string_view test_name = "transient lifetime and export dump";
    RenderGraph                graph("Lifetime");
    const auto transient = graph.CreateTransient("Intermediate", RenderGraph::ResourceKind::Texture);
    graph.Export(transient);

    graph.AddRecordPass(
        "CreateIntermediate",
        [transient](RenderGraph::PassBuilder& builder) {
            builder.Write(transient);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "ReadIntermediate",
        [transient](RenderGraph::PassBuilder& builder) {
            builder.Read(transient);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "UnrelatedTail",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const std::string dump = graph.Dump();
    suite.Check(
        Contains(
            dump,
            "Intermediate kind=texture lifetime=transient first=0 last=1 versions=1 exported=true aliases=[]"
        ),
        test_name,
        "dump must report the transient's first use, last use and export state"
    );
}

void TestUntouchedImportedResourceCanBeExported(TestSuite& suite) {
    constexpr std::string_view test_name = "untouched imported export boundary";
    RenderGraph                graph("ImportedForwarding");
    int                        physical_output = 0;
    const auto output = graph.Import("Output", RenderGraph::ResourceKind::Texture, &physical_output);
    graph.Export(output);
    graph.AddRecordPass(
        "ExternalWindowWrite",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const std::string dump = graph.Dump();
    suite.Check(
        Contains(
            dump, "Output kind=texture lifetime=imported first=unused last=unused versions=0 exported=true"
        ),
        test_name,
        "an imported resource must be exportable unchanged without a fake graph access"
    );
}

void TestTypedTextureSubresourceHazards(TestSuite& suite) {
    constexpr std::string_view test_name = "typed texture subresource hazards";
    RenderGraph                graph("TextureSubresources");
    int                        physical_texture = 0;
    const auto                 texture          = graph.ImportTexture(
        "MipChain", &physical_texture, RenderGraph::TextureDesc{.mip_count = 4, .layer_count = 2}
    );

    std::vector<int> callback_order;
    const auto       write_mip0 = graph.AddRecordPass(
        "WriteMip0",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, RenderGraph::TextureRange::Mips(0, 1));
        },
        [&](Moer::Render::CommandList&) {
            callback_order.push_back(0);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto write_mip1 = graph.AddRecordPass(
        "WriteMip1",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, RenderGraph::TextureRange::Mips(1, 1));
        },
        [&](Moer::Render::CommandList&) {
            callback_order.push_back(1);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto read_mip0 = graph.AddRecordPass(
        "ReadMip0",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureRange::Mips(0, 1));
        },
        [&](Moer::Render::CommandList&) {
            callback_order.push_back(2);
        },
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        !HasEdgeReason(
            plan, write_mip0, write_mip1, RenderGraph::EdgeReasonKind::WriteAfterWrite, texture.Untyped()
        ),
        test_name,
        "disjoint mips must not create a WAW dependency"
    );
    suite.Check(
        HasEdgeReason(
            plan, write_mip0, read_mip0, RenderGraph::EdgeReasonKind::ReadAfterWrite, texture.Untyped()
        ),
        test_name,
        "an overlapping mip read must depend on its writer"
    );
    suite.Check(
        !HasEdgeReason(
            plan, write_mip1, read_mip0, RenderGraph::EdgeReasonKind::ReadAfterWrite, texture.Untyped()
        ),
        test_name,
        "a read must not depend on a writer of another mip"
    );
    suite.Check(
        plan.dependency_levels.size() == 2 &&
            DependencyLevelContains(plan.dependency_levels[0], write_mip0) &&
            DependencyLevelContains(plan.dependency_levels[0], write_mip1) &&
            DependencyLevelContains(plan.dependency_levels[1], read_mip0),
        test_name,
        "dependency levels must expose independent mip writers without changing execution policy"
    );
    suite.Check(RecordGraphForTest(graph), test_name, graph.GetCompileError());
    suite.Check(
        callback_order == std::vector<int>{0, 1, 2},
        test_name,
        "production callbacks must remain serial and declaration ordered"
    );
}

void TestTypedTextureLayerAndAspectHazards(TestSuite& suite) {
    constexpr std::string_view test_name = "typed texture layer and aspect hazards";
    RenderGraph                graph("TextureLayersAndAspects");
    int                        physical_texture = 0;
    const auto                 texture          = graph.ImportTexture(
        "DepthStencilArray",
        &physical_texture,
        RenderGraph::TextureDesc{
                                     .mip_count   = 1,
                                     .layer_count = 2,
                                     .aspects = RenderGraph::TextureAspect::Depth | RenderGraph::TextureAspect::Stencil
        }
    );

    auto make_range = [](RenderGraph::TextureAspect aspect, uint32_t layer) {
        auto range    = RenderGraph::TextureRange::Layers(layer, 1);
        range.aspects = aspect;
        return range;
    };
    const auto depth_layer0   = make_range(RenderGraph::TextureAspect::Depth, 0);
    const auto depth_layer1   = make_range(RenderGraph::TextureAspect::Depth, 1);
    const auto stencil_layer0 = make_range(RenderGraph::TextureAspect::Stencil, 0);

    const auto write_depth_layer0 = graph.AddRecordPass(
        "WriteDepthLayer0",
        [texture, depth_layer0](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, depth_layer0);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto write_depth_layer1 = graph.AddRecordPass(
        "WriteDepthLayer1",
        [texture, depth_layer1](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, depth_layer1);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto write_stencil_layer0 = graph.AddRecordPass(
        "WriteStencilLayer0",
        [texture, stencil_layer0](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, stencil_layer0);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto read_depth_layer0 = graph.AddRecordPass(
        "ReadDepthLayer0",
        [texture, depth_layer0](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, depth_layer0);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        !HasEdgeReason(
            plan,
            write_depth_layer0,
            write_depth_layer1,
            RenderGraph::EdgeReasonKind::WriteAfterWrite,
            texture.Untyped()
        ),
        test_name,
        "different array layers must remain independent"
    );
    suite.Check(
        !HasEdgeReason(
            plan,
            write_depth_layer0,
            write_stencil_layer0,
            RenderGraph::EdgeReasonKind::WriteAfterWrite,
            texture.Untyped()
        ),
        test_name,
        "depth and stencil aspects must remain independent"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            write_depth_layer0,
            read_depth_layer0,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            texture.Untyped()
        ) &&
            !HasEdgeReason(
                plan,
                write_depth_layer1,
                read_depth_layer0,
                RenderGraph::EdgeReasonKind::ReadAfterWrite,
                texture.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                write_stencil_layer0,
                read_depth_layer0,
                RenderGraph::EdgeReasonKind::ReadAfterWrite,
                texture.Untyped()
            ),
        test_name,
        "a read must depend only on the writer of its exact layer and aspect"
    );
}

void TestTransientTextureInitializationIsPerSubresource(TestSuite& suite) {
    constexpr std::string_view test_name = "transient texture subresource initialization";
    RenderGraph                graph("TransientSubresources");
    const auto                 texture = graph.CreateTransientTexture(
        "TransientMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    int callback_count = 0;

    graph.AddRecordPass(
        "WriteMip0",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, RenderGraph::TextureRange::Mips(0, 1));
        },
        [&](Moer::Render::CommandList&) {
            ++callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "ReadUninitializedMip1",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureRange::Mips(1, 1));
        },
        [&](Moer::Render::CommandList&) {
            ++callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(!graph.Compile(), test_name, "reading an unwritten mip must be rejected");
    suite.Check(
        Contains(graph.GetCompileError(), "read before its first producer"),
        test_name,
        "subresource initialization failure must be diagnosed"
    );
    suite.Check(!RecordGraphForTest(graph), test_name, "a rejected graph must not execute");
    suite.Check(callback_count == 0, test_name, "a rejected graph must execute zero callbacks");
}

void TestExportedTransientRequiresWholeResourceInitialization(TestSuite& suite) {
    constexpr std::string_view test_name = "exported transient whole-resource initialization";

    RenderGraph partial_graph("PartialTransientExport");
    const auto  partial_texture = partial_graph.CreateTransientTexture(
        "PartialMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    int partial_callback_count = 0;
    partial_graph.AddRecordPass(
        "WriteMip0",
        [partial_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(partial_texture, RenderGraph::TextureRange::Mips(0, 1));
        },
        [&](Moer::Render::CommandList&) {
            ++partial_callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    partial_graph.Export(partial_texture);

    suite.Check(
        !partial_graph.Compile(), test_name, "a whole-resource export must reject an uninitialized mip"
    );
    suite.Check(
        Contains(partial_graph.GetCompileError(), "uninitialized subresources"),
        test_name,
        "a partial transient export must have a precise diagnostic"
    );
    suite.Check(
        !RecordGraphForTest(partial_graph), test_name, "a graph with an invalid transient export must not execute"
    );
    suite.Check(
        partial_callback_count == 0, test_name, "an invalid transient export must execute zero callbacks"
    );

    partial_graph.AddRecordPass(
        "WriteMip1",
        [partial_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(partial_texture, RenderGraph::TextureRange::Mips(1, 1));
        },
        [&](Moer::Render::CommandList&) {
            ++partial_callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    const bool recovered = partial_graph.Compile();
    suite.Check(recovered, test_name, partial_graph.GetCompileError());
    suite.Check(
        partial_graph.GetCompileError().empty() &&
            partial_graph.GetCompiledPlan().execution_order.size() == 2,
        test_name,
        "repairing the missing producer must replace the failed compile's diagnostic and partial plan"
    );
    const bool recovered_recording = RecordGraphForTest(partial_graph);
    suite.Check(recovered_recording, test_name, partial_graph.GetCompileError());
    suite.Check(
        partial_callback_count == 2, test_name,
        "the recovered graph must record both original and newly added producers once"
    );

    RenderGraph complete_graph("CompleteTransientExport");
    const auto  complete_texture = complete_graph.CreateTransientTexture(
        "CompleteMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    complete_graph.AddRecordPass(
        "WriteWholeTexture",
        [complete_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(complete_texture);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    complete_graph.Export(complete_texture);

    suite.Check(complete_graph.Compile(), test_name, complete_graph.GetCompileError());
    suite.Check(RecordGraphForTest(complete_graph), test_name, complete_graph.GetCompileError());
}

void TestTypedPartialExportRequiresOnlyDeclaredRange(TestSuite& suite) {
    constexpr std::string_view test_name = "typed partial export initialization";

    RenderGraph valid_graph("TypedPartialExport");
    const auto  valid_texture = valid_graph.CreateTransientTexture(
        "PartialMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    const auto write_mip_zero = valid_graph.AddRecordPass(
        "WriteMip0",
        [valid_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(
                valid_texture,
                RenderGraph::TextureAccessType::ColorAttachment,
                RenderGraph::TextureRange::Mips(0, 1)
            );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    valid_graph.Export(
        valid_texture,
        RenderGraph::TextureAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        RenderGraph::TextureRange::Mips(0, 1)
    );

    suite.Check(valid_graph.Compile(), test_name, valid_graph.GetCompileError());
    const auto& valid_plan = valid_graph.GetCompiledPlan();
    const auto* final_barrier = FindBarrier(
        valid_plan, valid_texture.Untyped(), write_mip_zero, {}
    );
    suite.Check(
        final_barrier != nullptr && final_barrier->export_boundary &&
            RenderGraph::GetRangeKind(final_barrier->range) == RenderGraph::ResourceKind::Texture &&
            std::get<RenderGraph::TextureRange>(final_barrier->range).mip_first == 0 &&
            std::get<RenderGraph::TextureRange>(final_barrier->range).mip_count == 1 &&
            valid_plan.state_plan_complete,
        test_name,
        "a typed partial export must cover only its initialized declared range"
    );

    RenderGraph invalid_graph("UninitializedTypedPartialExport");
    const auto  invalid_texture = invalid_graph.CreateTransientTexture(
        "PartialMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    invalid_graph.AddRecordPass(
        "WriteMip0",
        [invalid_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(
                invalid_texture,
                RenderGraph::TextureAccessType::ColorAttachment,
                RenderGraph::TextureRange::Mips(0, 1)
            );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    invalid_graph.Export(
        invalid_texture,
        RenderGraph::TextureAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        RenderGraph::TextureRange::Mips(1, 1)
    );
    suite.Check(
        !invalid_graph.Compile() &&
            Contains(invalid_graph.GetCompileError(), "uninitialized subresources"),
        test_name,
        "a typed partial export must still reject an uninitialized declared range"
    );
}

void TestTypedBufferRangeHazards(TestSuite& suite) {
    constexpr std::string_view test_name = "typed buffer range hazards";
    RenderGraph                graph("BufferRanges");
    int                        physical_buffer = 0;
    const auto                 buffer =
        graph.ImportBuffer("RangeBuffer", &physical_buffer, RenderGraph::BufferDesc{.byte_size = 256});

    const auto write_head = graph.AddRecordPass(
        "WriteHead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferRange{.offset = 0, .size = 64});
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto read_adjacent = graph.AddRecordPass(
        "ReadAdjacent",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferRange{.offset = 64, .size = 64});
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto read_overlap = graph.AddRecordPass(
        "ReadOverlap",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferRange{.offset = 32, .size = 32});
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto overwrite_overlap = graph.AddRecordPass(
        "OverwriteOverlap",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferRange{.offset = 32, .size = 16});
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        !HasEdgeReason(
            plan, write_head, read_adjacent, RenderGraph::EdgeReasonKind::ReadAfterWrite, buffer.Untyped()
        ),
        test_name,
        "adjacent non-overlapping byte ranges must not create a dependency"
    );
    suite.Check(
        HasEdgeReason(
            plan, write_head, read_overlap, RenderGraph::EdgeReasonKind::ReadAfterWrite, buffer.Untyped()
        ),
        test_name,
        "overlapping buffer ranges must create RAW"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            read_overlap,
            overwrite_overlap,
            RenderGraph::EdgeReasonKind::WriteAfterRead,
            buffer.Untyped()
        ),
        test_name,
        "an overlapping writer must wait for the active reader"
    );
    suite.Check(
        !HasEdgeReason(
            plan,
            read_adjacent,
            overwrite_overlap,
            RenderGraph::EdgeReasonKind::WriteAfterRead,
            buffer.Untyped()
        ),
        test_name,
        "a writer must not wait for a reader of a disjoint range"
    );
}

void TestLogicalResourceVersions(TestSuite& suite) {
    constexpr std::string_view test_name = "logical resource versions";
    RenderGraph                graph("Versions");
    const auto buffer = graph.CreateTransientBuffer("Scratch", RenderGraph::BufferDesc{.byte_size = 128});

    const auto produce = graph.AddRecordPass(
        "Produce",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto consume = graph.AddRecordPass(
        "Consume",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto overwrite = graph.AddRecordPass(
        "Overwrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan        = graph.GetCompiledPlan();
    auto        find_access = [&](RenderGraph::PassHandle pass) -> const RenderGraph::CompiledAccess* {
        const auto found = std::find_if(
            plan.accesses.begin(),
            plan.accesses.end(),
            [&](const RenderGraph::CompiledAccess& access) {
                return access.pass == pass && access.resource == buffer.Untyped();
            }
        );
        return found == plan.accesses.end() ? nullptr : &*found;
    };
    const auto* produce_access   = find_access(produce);
    const auto* consume_access   = find_access(consume);
    const auto* overwrite_access = find_access(overwrite);
    suite.Check(
        produce_access != nullptr && produce_access->input_version == RenderGraph::InvalidVersion &&
            produce_access->output_version == 1,
        test_name,
        "the first transient write must produce version 1"
    );
    suite.Check(
        consume_access != nullptr && consume_access->input_version == 1 &&
            consume_access->output_version == 1,
        test_name,
        "a read must consume without advancing the version"
    );
    suite.Check(
        overwrite_access != nullptr && overwrite_access->input_version == 1 &&
            overwrite_access->output_version == 2,
        test_name,
        "the next write must advance the resource version"
    );
    suite.Check(
        buffer.Untyped().resource_id < plan.resources.size() &&
            plan.resources[buffer.Untyped().resource_id].version_count == 2,
        test_name,
        "compiled resource metadata must expose the final version count"
    );
}

void TestInvalidTypedRangeIsRejected(TestSuite& suite) {
    constexpr std::string_view test_name = "invalid typed range rejection";
    RenderGraph                graph("InvalidRange");
    int                        physical_texture = 0;
    const auto                 texture          = graph.ImportTexture(
        "TwoMips", &physical_texture, RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    int callback_count = 0;
    graph.AddRecordPass(
        "OutOfBoundsRead",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureRange::Mips(2, 1));
        },
        [&](Moer::Render::CommandList&) {
            ++callback_count;
        },
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(!graph.Compile(), test_name, "out-of-bounds typed ranges must fail compile");
    suite.Check(
        Contains(graph.GetCompileError(), "beyond the mip count"),
        test_name,
        "invalid range rejection must identify the mip bound"
    );
    suite.Check(callback_count == 0, test_name, "compile failure must happen before callbacks");
}

void TestUnknownTextureAspectIsRejected(TestSuite& suite) {
    constexpr std::string_view test_name = "unknown texture descriptor aspect";
    RenderGraph                graph("UnknownTextureAspect");
    constexpr auto             invalid_aspects = static_cast<RenderGraph::TextureAspect>(
        static_cast<uint8_t>(RenderGraph::TextureAspect::Color) | uint8_t{0x80}
    );
    const auto texture = graph.CreateTransientTexture(
        "InvalidTexture",
        RenderGraph::TextureDesc{.mip_count = 1, .layer_count = 1, .aspects = invalid_aspects}
    );

    suite.Check(!texture.IsValid(), test_name, "an unknown aspect bit must reject the declaration");
    suite.Check(!graph.Compile(), test_name, "a graph with an unknown texture aspect must not compile");
    suite.Check(
        Contains(graph.GetCompileError(), "descriptor is invalid"),
        test_name,
        "an unknown texture aspect must have a precise diagnostic"
    );
}

void TestTextureAttachmentStateMatchesSelectedAspects(TestSuite& suite) {
    constexpr std::string_view test_name = "texture attachment state aspect validation";
    auto add_side_effect = [](RenderGraph& graph) {
        graph.AddRecordPass(
            "SideEffect",
            [](RenderGraph::PassBuilder& builder) {
                builder.SideEffect();
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
    };

    RenderGraph color_access_graph("ColorAsDepthAttachment");
    const auto  color_access = color_access_graph.CreateTransientTexture(
        "Color", RenderGraph::TextureDesc{.aspects = RenderGraph::TextureAspect::Color}
    );
    color_access_graph.AddRecordPass(
        "InvalidDepthWrite",
        [color_access](RenderGraph::PassBuilder& builder) {
            builder.Write(color_access, RenderGraph::TextureAccessType::DepthStencilAttachment);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !color_access_graph.Compile() &&
            Contains(color_access_graph.GetCompileError(), "selected aspects"),
        test_name,
        "a color pass range must reject depth/stencil attachment states"
    );

    RenderGraph depth_access_graph("DepthAsColorAttachment");
    const auto  depth_access = depth_access_graph.CreateTransientTexture(
        "Depth", RenderGraph::TextureDesc{.aspects = RenderGraph::TextureAspect::Depth}
    );
    depth_access_graph.AddRecordPass(
        "InvalidColorWrite",
        [depth_access](RenderGraph::PassBuilder& builder) {
            builder.Write(depth_access, RenderGraph::TextureAccessType::ColorAttachment);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !depth_access_graph.Compile() &&
            Contains(depth_access_graph.GetCompileError(), "selected aspects"),
        test_name,
        "a depth pass range must reject the color render-target state"
    );

    RenderGraph depth_storage_graph("DepthAsStorageImage");
    const auto  depth_storage = depth_storage_graph.CreateTransientTexture(
        "Depth", RenderGraph::TextureDesc{.aspects = RenderGraph::TextureAspect::Depth}
    );
    depth_storage_graph.AddRecordPass(
        "InvalidStorageAccess",
        [depth_storage](RenderGraph::PassBuilder& builder) {
            builder.ReadWrite(depth_storage, RenderGraph::TextureAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !depth_storage_graph.Compile() &&
            Contains(depth_storage_graph.GetCompileError(), "selected aspects"),
        test_name,
        "the portable depth/stencil model must reject unordered-access image states"
    );

    RenderGraph initial_graph("InvalidInitialAttachmentAspect");
    int         initial_physical = 0;
    const auto  initial_texture = initial_graph.ImportTexture(
        "Color",
        &initial_physical,
        RenderGraph::TextureDesc{.aspects = RenderGraph::TextureAspect::Color}
    );
    initial_graph.SetInitialState(
        initial_texture,
        RenderGraph::TextureAccessType::DepthStencilAttachment,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    add_side_effect(initial_graph);
    suite.Check(
        !initial_graph.Compile() && Contains(initial_graph.GetCompileError(), "selected aspects"),
        test_name,
        "an imported color range must reject a depth/stencil boundary state"
    );

    RenderGraph final_graph("InvalidFinalAttachmentAspect");
    int         final_physical = 0;
    const auto  final_texture = final_graph.ImportTexture(
        "Depth",
        &final_physical,
        RenderGraph::TextureDesc{.aspects = RenderGraph::TextureAspect::Depth}
    );
    final_graph.SetInitialState(
        final_texture,
        RenderGraph::TextureAccessType::DepthStencilAttachment,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    final_graph.Export(
        final_texture,
        RenderGraph::TextureAccessType::ColorAttachment,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    add_side_effect(final_graph);
    suite.Check(
        !final_graph.Compile() && Contains(final_graph.GetCompileError(), "selected aspects"),
        test_name,
        "an exported depth range must reject the color render-target state"
    );

    RenderGraph valid_graph("ValidDepthStencilAspects");
    int         valid_physical = 0;
    const auto  valid_texture = valid_graph.ImportTexture(
        "DepthStencil",
        &valid_physical,
        RenderGraph::TextureDesc{
            .aspects = RenderGraph::TextureAspect::Depth | RenderGraph::TextureAspect::Stencil
        }
    );
    valid_graph.SetInitialState(
        valid_texture,
        RenderGraph::TextureAccessType::DepthStencilAttachment,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    valid_graph.AddRecordPass(
        "ReadDepth",
        [valid_texture](RenderGraph::PassBuilder& builder) {
            builder.Read(
                valid_texture,
                RenderGraph::TextureAccessType::DepthStencilAttachment,
                RenderGraph::TextureRange{.aspects = RenderGraph::TextureAspect::Depth}
            );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    valid_graph.Export(
        valid_texture,
        RenderGraph::TextureAccessType::DepthStencilAttachment,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        RenderGraph::TextureRange{.aspects = RenderGraph::TextureAspect::Stencil}
    );
    suite.Check(
        valid_graph.Compile(),
        test_name,
        "depth and stencil subsets must accept depth/stencil attachment states: " +
            valid_graph.GetCompileError()
    );
}

void TestPresentationSourceBoundaryContract(TestSuite& suite) {
    constexpr std::string_view test_name =
        "presentation source boundary contract";
    auto import_color = [](RenderGraph& graph, int& physical) {
        const auto texture = graph.ImportTexture(
            "Color",
            &physical,
            RenderGraph::TextureDesc{
                .aspects = RenderGraph::TextureAspect::Color,
            }
        );
        graph.SetInitialState(
            texture,
            RenderGraph::TextureAccessType::Storage,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        graph.AddRecordPass(
            "SideEffect",
            [](RenderGraph::PassBuilder& builder) {
                builder.SideEffect();
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        return texture;
    };

    RenderGraph valid_graph("ValidPresentationSource");
    int         valid_physical = 0;
    const auto  valid_texture =
        import_color(valid_graph, valid_physical);
    valid_graph.Export(
        valid_texture,
        RenderGraph::TextureBoundary::PresentationSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    suite.Check(
        valid_graph.Compile(),
        test_name,
        "a color texture must be exportable as a read-only presentation source: " +
            valid_graph.GetCompileError()
    );

    for (const auto [graph_name, queue] :
         {std::pair{"ComputePresentationSourceRejected",
                    RenderGraph::QueueRole::Compute},
          std::pair{"CopyPresentationSourceRejected",
                    RenderGraph::QueueRole::Copy},
          std::pair{"UnownedPresentationSourceRejected",
                    RenderGraph::QueueRole::None}}) {
        RenderGraph queue_graph(graph_name);
        int         queue_physical = 0;
        const auto  queue_texture =
            import_color(queue_graph, queue_physical);
        queue_graph.Export(
            queue_texture,
            RenderGraph::TextureBoundary::PresentationSource,
            queue,
            RenderGraph::AccessMode::Read
        );
        suite.Check(
            !queue_graph.Compile() &&
                Contains(queue_graph.GetCompileError(), "Graphics queue"),
            test_name,
            "a presentation source export accepted a non-Graphics owner queue"
        );
    }

    RenderGraph initial_graph("PresentationSourceReimport");
    int         initial_physical = 0;
    const auto  initial_texture = initial_graph.ImportTexture(
        "Color",
        &initial_physical,
        RenderGraph::TextureDesc{
            .aspects = RenderGraph::TextureAspect::Color,
        }
    );
    initial_graph.SetInitialState(
        initial_texture,
        RenderGraph::TextureBoundary::PresentationSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    initial_graph.AddRecordPass(
        "ReadImportedColor",
        [initial_texture](RenderGraph::PassBuilder& builder) {
            builder
                .Read(
                    initial_texture,
                    RenderGraph::TextureAccessType::Storage
                )
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    initial_graph.Export(
        initial_texture,
        RenderGraph::TextureAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    suite.Check(
        initial_graph.Compile(),
        test_name,
        "a following graph must import the previous presentation-copy read boundary"
    );

    RenderGraph write_graph("PresentationSourceWriteRejected");
    int         write_physical = 0;
    const auto  write_texture =
        import_color(write_graph, write_physical);
    write_graph.Export(
        write_texture,
        RenderGraph::TextureBoundary::PresentationSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    suite.Check(
        !write_graph.Compile() &&
            Contains(write_graph.GetCompileError(), "boundary access"),
        test_name,
        "a presentation source boundary must reject write access"
    );

    RenderGraph read_write_graph("PresentationSourceReadWriteRejected");
    int         read_write_physical = 0;
    const auto  read_write_texture =
        import_color(read_write_graph, read_write_physical);
    read_write_graph.Export(
        read_write_texture,
        RenderGraph::TextureBoundary::PresentationSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::ReadWrite
    );
    suite.Check(
        !read_write_graph.Compile() &&
            Contains(read_write_graph.GetCompileError(), "boundary access"),
        test_name,
        "a presentation source boundary must reject read-write access"
    );

    RenderGraph depth_graph("DepthPresentationSourceRejected");
    int         depth_physical = 0;
    const auto  depth_texture = depth_graph.ImportTexture(
        "Depth",
        &depth_physical,
        RenderGraph::TextureDesc{
            .aspects = RenderGraph::TextureAspect::Depth,
        }
    );
    depth_graph.SetInitialState(
        depth_texture,
        RenderGraph::TextureAccessType::DepthStencilAttachment,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    depth_graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    depth_graph.Export(
        depth_texture,
        RenderGraph::TextureBoundary::PresentationSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    suite.Check(
        !depth_graph.Compile() &&
            Contains(depth_graph.GetCompileError(), "selected aspects"),
        test_name,
        "only a color aspect may be exported as a presentation source"
    );
}

void TestTypedAliasDescriptorMismatchIsRejected(TestSuite& suite) {
    constexpr std::string_view test_name = "typed alias descriptor mismatch";
    RenderGraph                graph("AliasDescriptorMismatch");
    int                        physical_texture = 0;
    const auto                 canonical        = graph.ImportTexture(
        "Canonical", &physical_texture, RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    const auto invalid_alias = graph.ImportTexture(
        "InvalidAlias", &physical_texture, RenderGraph::TextureDesc{.mip_count = 3, .layer_count = 1}
    );
    graph.AddRecordPass(
        "ReadCanonical",
        [canonical](RenderGraph::PassBuilder& builder) {
            builder.Read(canonical);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(!invalid_alias.IsValid(), test_name, "a mismatched alias import must return invalid");
    suite.Check(!graph.Compile(), test_name, "a mismatched alias descriptor must reject the graph");
    suite.Check(
        Contains(graph.GetCompileError(), "different descriptor"),
        test_name,
        "alias descriptor mismatch must be diagnosed"
    );
}

void TestMultipleReadersProduceAllWarEdges(TestSuite& suite) {
    constexpr std::string_view test_name = "multiple reader WAR edges";
    RenderGraph                graph("ReaderSet");
    int                        physical_buffer = 0;
    const auto                 buffer =
        graph.ImportBuffer("Shared", &physical_buffer, RenderGraph::BufferDesc{.byte_size = 64});
    const auto reader_a = graph.AddRecordPass(
        "ReaderA",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto reader_b = graph.AddRecordPass(
        "ReaderB",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto writer = graph.AddRecordPass(
        "Writer",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan, reader_a, writer, RenderGraph::EdgeReasonKind::WriteAfterRead, buffer.Untyped()
        ) &&
            HasEdgeReason(
                plan, reader_b, writer, RenderGraph::EdgeReasonKind::WriteAfterRead, buffer.Untyped()
            ),
        test_name,
        "a writer must depend on every active overlapping reader"
    );
}

void TestExplicitStateValidation(TestSuite& suite) {
    constexpr std::string_view test_name = "explicit state validation";

    struct TextureCase {
        RenderGraph::AccessMode   access;
        RenderGraph::TextureAccessType state;
        RenderGraph::QueueRole    queue;
        RenderGraph::GpuWorkType  work;
        bool                     valid;
    };
    const std::vector<TextureCase> texture_cases{
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureAccessType::Sampled,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::GpuWorkType::Graphics,
         true},
        {RenderGraph::AccessMode::Write,
         RenderGraph::TextureAccessType::ColorAttachment,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::GpuWorkType::Graphics,
         true},
        {RenderGraph::AccessMode::ReadWrite,
         RenderGraph::TextureAccessType::Storage,
         RenderGraph::QueueRole::Compute,
         RenderGraph::GpuWorkType::Compute,
         true},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureAccessType::TransferSource,
         RenderGraph::QueueRole::Copy,
         RenderGraph::GpuWorkType::Transfer,
         true},
        {RenderGraph::AccessMode::Write,
         RenderGraph::TextureAccessType::Sampled,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::GpuWorkType::Graphics,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureAccessType::TransferDestination,
         RenderGraph::QueueRole::Copy,
         RenderGraph::GpuWorkType::Transfer,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureAccessType::ColorAttachment,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::GpuWorkType::Graphics,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureAccessType::Unspecified,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::GpuWorkType::Graphics,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureAccessType::Sampled,
         RenderGraph::QueueRole::Copy,
         RenderGraph::GpuWorkType::Transfer,
         false},
        {RenderGraph::AccessMode::Write,
         RenderGraph::TextureAccessType::ColorAttachment,
         RenderGraph::QueueRole::Compute,
         RenderGraph::GpuWorkType::Compute,
         false},
    };

    for (uint32_t index = 0; index < texture_cases.size(); ++index) {
        const auto& current = texture_cases[index];
        RenderGraph graph("TextureStateCase");
        int         physical = 0;
        const auto  texture = graph.ImportTexture("Texture", &physical, RenderGraph::TextureDesc{});
        graph.AddRecordPass(
            "Access",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(current.queue, current.work);
                switch (current.access) {
                    case RenderGraph::AccessMode::Read:
                        builder.Read(texture, current.state);
                        break;
                    case RenderGraph::AccessMode::Write:
                        builder.Write(texture, current.state);
                        break;
                    case RenderGraph::AccessMode::ReadWrite:
                        builder.ReadWrite(texture, current.state);
                        break;
                    default:
                        break;
                }
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        suite.Check(
            graph.Compile() == current.valid,
            test_name,
            "texture state/access/domain case " + std::to_string(index) + " had the wrong validity"
        );
    }

    struct BufferCase {
        RenderGraph::AccessMode  access;
        RenderGraph::BufferAccessType state;
        RenderGraph::QueueRole   queue;
        RenderGraph::GpuWorkType work;
        bool                     valid;
    };
    const std::vector<BufferCase> buffer_cases{
        {RenderGraph::AccessMode::Read,
         RenderGraph::BufferAccessType::ShaderRead,
         RenderGraph::QueueRole::Compute,
         RenderGraph::GpuWorkType::Compute,
         true},
        {RenderGraph::AccessMode::Write,
         RenderGraph::BufferAccessType::TransferDestination,
         RenderGraph::QueueRole::Copy,
         RenderGraph::GpuWorkType::Transfer,
         true},
        {RenderGraph::AccessMode::Read,
         RenderGraph::BufferAccessType::VertexBuffer,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::GpuWorkType::Graphics,
         true},
        {RenderGraph::AccessMode::Write,
         RenderGraph::BufferAccessType::ShaderRead,
         RenderGraph::QueueRole::Compute,
         RenderGraph::GpuWorkType::Compute,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::BufferAccessType::AccelerationStructureWrite,
         RenderGraph::QueueRole::Compute,
         RenderGraph::GpuWorkType::RayTracing,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::BufferAccessType::VertexBuffer,
         RenderGraph::QueueRole::Compute,
         RenderGraph::GpuWorkType::Compute,
         false},
    };

    for (uint32_t index = 0; index < buffer_cases.size(); ++index) {
        const auto& current = buffer_cases[index];
        RenderGraph graph("BufferStateCase");
        int         physical = 0;
        const auto  buffer = graph.ImportBuffer("Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 64});
        graph.AddRecordPass(
            "Access",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(current.queue, current.work);
                switch (current.access) {
                    case RenderGraph::AccessMode::Read:
                        builder.Read(buffer, current.state);
                        break;
                    case RenderGraph::AccessMode::Write:
                        builder.Write(buffer, current.state);
                        break;
                    case RenderGraph::AccessMode::ReadWrite:
                        builder.ReadWrite(buffer, current.state);
                        break;
                    default:
                        break;
                }
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        suite.Check(
            graph.Compile() == current.valid,
            test_name,
            "buffer state/access/domain case " + std::to_string(index) + " had the wrong validity"
        );
    }
}

void TestSameStateMemoryDependencies(TestSuite& suite) {
    constexpr std::string_view test_name = "same-state memory dependencies";
    auto add_compute_access = [](
                                  RenderGraph&             graph,
                                  std::string_view         name,
                                  RenderGraph::BufferHandle buffer,
                                  RenderGraph::AccessMode  mode
                              ) {
        return graph.AddRecordPass(
            name,
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
                if (mode == RenderGraph::AccessMode::Read) {
                    builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
                } else {
                    builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
                }
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
    };

    for (const auto second_mode : {RenderGraph::AccessMode::Read, RenderGraph::AccessMode::Write}) {
        RenderGraph graph("UavWriteDependency");
        int         physical = 0;
        const auto  buffer = graph.ImportBuffer("Uav", &physical, RenderGraph::BufferDesc{.byte_size = 64});
        graph.SetInitialState(
            buffer,
            RenderGraph::BufferAccessType::Storage,
            RenderGraph::QueueRole::Compute,
            RenderGraph::AccessMode::Read
        );
        const auto first  = add_compute_access(graph, "FirstWrite", buffer, RenderGraph::AccessMode::Write);
        const auto second = add_compute_access(graph, "SecondAccess", buffer, second_mode);
        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const auto* barrier = FindBarrier(graph.GetCompiledPlan(), buffer.Untyped(), first, second);
        suite.Check(
            barrier != nullptr && barrier->memory_dependency && !barrier->state_transition,
            test_name,
            second_mode == RenderGraph::AccessMode::Read ?
                "same-state UAV W->R must emit only a memory dependency" :
                "same-state UAV W->W must emit only a memory dependency"
        );
    }

    RenderGraph read_graph("UavReadRead");
    int         physical = 0;
    const auto  buffer = read_graph.ImportBuffer("Uav", &physical, RenderGraph::BufferDesc{.byte_size = 64});
    read_graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::Storage,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Read
    );
    const auto first  = add_compute_access(read_graph, "FirstRead", buffer, RenderGraph::AccessMode::Read);
    const auto second = add_compute_access(read_graph, "SecondRead", buffer, RenderGraph::AccessMode::Read);
    suite.Check(read_graph.Compile(), test_name, read_graph.GetCompileError());
    suite.Check(
        FindBarrier(read_graph.GetCompiledPlan(), buffer.Untyped(), first, second) == nullptr,
        test_name,
        "same-state R->R must not emit a barrier"
    );
}

void TestImportAndExportBoundaries(TestSuite& suite) {
    constexpr std::string_view test_name = "import and export boundaries";

    RenderGraph known_graph("KnownImport");
    int         known_physical = 0;
    const auto  known = known_graph.ImportBuffer(
        "Known", &known_physical, RenderGraph::BufferDesc{.byte_size = 64}
    );
    known_graph.SetInitialState(
        known,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto known_read = known_graph.AddRecordPass(
        "Read",
        [known](RenderGraph::PassBuilder& builder) {
            builder.Read(known, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(known_graph.Compile(), test_name, known_graph.GetCompileError());
    const auto& known_plan = known_graph.GetCompiledPlan();
    const auto* known_barrier = FindBarrier(known_plan, known.Untyped(), {}, known_read);
    suite.Check(
        known_plan.state_plan_complete && known_plan.prologue_barriers.size() == 1 &&
            known_barrier != nullptr && known_barrier->import_boundary &&
            !known_barrier->source_state_unknown && !known_barrier->state_transition,
        test_name,
        "a known import must produce a complete, known-source prologue record"
    );

    RenderGraph unknown_graph("UnknownImport");
    int         unknown_physical = 0;
    const auto  unknown = unknown_graph.ImportBuffer(
        "Unknown", &unknown_physical, RenderGraph::BufferDesc{.byte_size = 64}
    );
    const auto unknown_read = unknown_graph.AddRecordPass(
        "Read",
        [unknown](RenderGraph::PassBuilder& builder) {
            builder.Read(unknown, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(unknown_graph.Compile(), test_name, unknown_graph.GetCompileError());
    const auto& unknown_plan = unknown_graph.GetCompiledPlan();
    const auto* unknown_barrier = FindBarrier(unknown_plan, unknown.Untyped(), {}, unknown_read);
    suite.Check(
        !unknown_plan.state_plan_complete && unknown_plan.prologue_barriers.size() == 1 &&
            unknown_barrier != nullptr && unknown_barrier->import_boundary &&
            unknown_barrier->source_state_unknown && unknown_barrier->state_transition,
        test_name,
        "an unknown import must remain visibly incomplete and transition at its first use"
    );

    RenderGraph final_graph("FinalState");
    const auto  output = final_graph.CreateTransientBuffer(
        "Output", RenderGraph::BufferDesc{.byte_size = 64}
    );
    const auto write = final_graph.AddRecordPass(
        "Write",
        [output](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Write(output, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    final_graph.Export(
        output,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    suite.Check(final_graph.Compile(), test_name, final_graph.GetCompileError());
    const auto& final_plan = final_graph.GetCompiledPlan();
    const auto* final_barrier = FindBarrier(final_plan, output.Untyped(), write, {});
    suite.Check(
        final_plan.epilogue_barriers.size() == 1 && final_barrier != nullptr &&
            final_barrier->export_boundary && final_barrier->state_transition &&
            final_barrier->memory_dependency,
        test_name,
        "an explicit final state must create a state-and-memory epilogue barrier"
    );

    RenderGraph untouched_graph("UntouchedExport");
    int         untouched_physical = 0;
    const auto  untouched = untouched_graph.ImportBuffer(
        "Untouched", &untouched_physical, RenderGraph::BufferDesc{.byte_size = 64}
    );
    untouched_graph.SetInitialState(
        untouched,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    untouched_graph.Export(
        untouched,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    untouched_graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(untouched_graph.Compile(), test_name, untouched_graph.GetCompileError());
    const auto& untouched_plan = untouched_graph.GetCompiledPlan();
    const auto* untouched_barrier = FindBarrier(untouched_plan, untouched.Untyped(), {}, {});
    suite.Check(
        untouched_plan.epilogue_barriers.size() == 1 && untouched_barrier != nullptr &&
            untouched_barrier->export_boundary && !untouched_barrier->state_transition &&
            !untouched_barrier->memory_dependency,
        test_name,
        "an untouched imported resource must still retain its explicit export boundary"
    );
}

void TestQueueTopologySynchronization(TestSuite& suite) {
    constexpr std::string_view test_name = "queue topology synchronization";
    auto check_case = [&](RenderGraph::QueueTopology topology,
                          RenderGraph::SharingMode sharing,
                          bool                                  queue_dependency,
                          bool                                  ownership,
                          bool                                  gpu_wait,
                          std::string_view                      expectation) {
        RenderGraph graph("QueueTopology", topology);
        const auto  buffer = graph.CreateTransientBuffer(
            "Shared", RenderGraph::BufferDesc{.byte_size = 64, .sharing_mode = sharing}
        );
        const auto producer = graph.AddRecordPass(
            "Produce",
            [buffer](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
                builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        const auto consumer = graph.AddRecordPass(
            "Consume",
            [buffer](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Graphics);
                builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );
        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const auto& plan = graph.GetCompiledPlan();
        const auto* barrier = FindBarrier(plan, buffer.Untyped(), producer, consumer);
        suite.Check(
            barrier != nullptr && barrier->queue_dependency == queue_dependency &&
                barrier->queue_ownership == ownership,
            test_name,
            expectation
        );
        suite.Check(
            plan.queue_syncs.size() == 1 && plan.queue_syncs.front().gpu_wait_required == gpu_wait,
            test_name,
            "queue synchronization mode must follow native queue identity"
        );
    };

    check_case(
        RenderGraph::QueueTopology::SingleQueue(),
        RenderGraph::SharingMode::Exclusive,
        false,
        false,
        false,
        "logical roles on one native queue must not transfer ownership"
    );
    check_case(
        RenderGraph::QueueTopology{
            .graphics = {RenderGraph::QueueRole::Graphics, 0, 0},
            .compute  = {RenderGraph::QueueRole::Compute, 1, 0},
            .copy     = {RenderGraph::QueueRole::Copy, 2, 0},
        },
        RenderGraph::SharingMode::Exclusive,
        true,
        false,
        true,
        "different native queues in one family require synchronization but no ownership transfer"
    );
    check_case(
        RenderGraph::QueueTopology::DedicatedQueues(),
        RenderGraph::SharingMode::Exclusive,
        true,
        true,
        true,
        "different families must transfer an exclusive resource"
    );
    check_case(
        RenderGraph::QueueTopology::DedicatedQueues(),
        RenderGraph::SharingMode::Concurrent,
        true,
        false,
        true,
        "different families must not transfer a concurrent resource"
    );
}

void TestExclusiveOwnershipUsesCurrentOwnerFamily(TestSuite& suite) {
    constexpr std::string_view test_name = "exclusive ownership follows current owner family";
    RenderGraph graph("OwnershipChain", RenderGraph::QueueTopology::DedicatedQueues());
    int         physical = 0;
    const auto  buffer = graph.ImportBuffer(
        "Shared",
        &physical,
        RenderGraph::BufferDesc{
            .byte_size    = 64,
            .sharing_mode = RenderGraph::SharingMode::Exclusive,
        }
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto graphics_read = graph.AddRecordPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_read = graph.AddRecordPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto copy_read = graph.AddRecordPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::GpuWorkType::Transfer);
            builder.Read(buffer, RenderGraph::BufferAccessType::TransferSource);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* transfer = FindBarrier(plan, buffer.Untyped(), compute_read, copy_read);
    suite.Check(
        transfer != nullptr && transfer->queue_ownership && transfer->sources.size() == 1 &&
            HasBarrierSource(*transfer, compute_read, RenderGraph::AccessMode::Read) &&
            !HasBarrierSource(*transfer, graphics_read, RenderGraph::AccessMode::Read),
        test_name,
        "the second transfer must release only from the currently owning Compute family"
    );
    const auto* final_transfer = FindBarrier(plan, buffer.Untyped(), copy_read, {});
    suite.Check(
        final_transfer != nullptr && final_transfer->export_boundary &&
            final_transfer->queue_ownership && final_transfer->sources.size() == 1 &&
            HasBarrierSource(*final_transfer, copy_read, RenderGraph::AccessMode::Read),
        test_name,
        "the export transfer must release only from the final Copy owner"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            compute_read,
            copy_read,
            RenderGraph::EdgeReasonKind::QueueOwnership,
            buffer.Untyped()
        ) &&
            !HasEdgeReason(
                plan,
                graphics_read,
                copy_read,
                RenderGraph::EdgeReasonKind::QueueOwnership,
                buffer.Untyped()
            ),
        test_name,
        "the ownership chain must be Graphics-to-Compute-to-Copy without a stale Graphics-to-Copy edge"
    );
    const auto graphics_to_copy = std::find_if(
        plan.queue_syncs.begin(),
        plan.queue_syncs.end(),
        [](const RenderGraph::CompiledQueueSync& sync) {
            return sync.signal_queue.role == RenderGraph::QueueRole::Graphics &&
                   sync.wait_queue.role == RenderGraph::QueueRole::Copy;
        }
    );
    suite.Check(
        plan.queue_syncs.size() == 2 && graphics_to_copy == plan.queue_syncs.end(),
        test_name,
        "the second transfer must not create a redundant Graphics-to-Copy queue sync"
    );
}

void TestOwnershipWriterChainUsesCurrentFrontier(TestSuite& suite) {
    constexpr std::string_view test_name = "ownership writer chain uses current frontier";
    RenderGraph graph("WriterOwnershipChain", RenderGraph::QueueTopology::DedicatedQueues());
    const auto  buffer = graph.CreateTransientBuffer(
        "Shared", RenderGraph::BufferDesc{.byte_size = 64}
    );
    const auto writer = graph.AddRecordPass(
        "GraphicsWrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_read = graph.AddRecordPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto copy_read = graph.AddRecordPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::GpuWorkType::Transfer);
            builder.Read(buffer, RenderGraph::BufferAccessType::TransferSource);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* transfer = FindBarrier(plan, buffer.Untyped(), compute_read, copy_read);
    suite.Check(
        transfer != nullptr && transfer->queue_ownership && transfer->sources.size() == 1 &&
            HasBarrierSource(*transfer, compute_read, RenderGraph::AccessMode::Read) &&
            !HasBarrierSource(*transfer, writer, RenderGraph::AccessMode::Write),
        test_name,
        "the second transfer must use only the read that established the current frontier"
    );
    suite.Check(
        !HasEdgeReason(
            plan,
            writer,
            copy_read,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            buffer.Untyped()
        ) &&
            !HasEdgeReason(
                plan,
                writer,
                copy_read,
                RenderGraph::EdgeReasonKind::StateTransition,
                buffer.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                writer,
                copy_read,
                RenderGraph::EdgeReasonKind::QueueOwnership,
                buffer.Untyped()
            ),
        test_name,
        "an old writer must not remain a direct dependency after a read transfer"
    );
    const auto stale_sync = std::find_if(
        plan.queue_syncs.begin(),
        plan.queue_syncs.end(),
        [&](const RenderGraph::CompiledQueueSync& sync) {
            return sync.signal_pass == writer && sync.wait_pass == copy_read;
        }
    );
    suite.Check(
        plan.queue_syncs.size() == 2 && stale_sync == plan.queue_syncs.end(),
        test_name,
        "the ownership chain must not lower a stale Graphics-to-Copy GPU wait"
    );
}

void TestUnspecifiedReadsPreserveAvailabilityFrontier(TestSuite& suite) {
    constexpr std::string_view test_name = "unspecified reads preserve availability frontier";
    RenderGraph                graph("UnspecifiedAvailability", RenderGraph::QueueTopology::DedicatedQueues());
    const auto  buffer = graph.CreateTransientBuffer(
        "Concurrent",
        RenderGraph::BufferDesc{
            .byte_size    = 64,
            .sharing_mode = RenderGraph::SharingMode::Concurrent,
        }
    );
    const auto writer = graph.AddRecordPass(
        "GraphicsWrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto transition = graph.AddRecordPass(
        "GraphicsTransition",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto unspecified_compute_a = graph.AddRecordPass(
        "ComputeUnspecifiedA",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "GraphicsUnspecified",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto unspecified_compute_b = graph.AddRecordPass(
        "ComputeUnspecifiedB",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan,
            transition,
            unspecified_compute_a,
            RenderGraph::EdgeReasonKind::StateTransition,
            buffer.Untyped()
        ) &&
            HasEdgeReason(
                plan,
                transition,
                unspecified_compute_b,
                RenderGraph::EdgeReasonKind::StateTransition,
                buffer.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                writer,
                unspecified_compute_b,
                RenderGraph::EdgeReasonKind::ReadAfterWrite,
                buffer.Untyped()
            ),
        test_name,
        "Unspecified reads must inherit, preserve, and not bypass the transition availability frontier"
    );
    const uint32_t transition_syncs = static_cast<uint32_t>(std::count_if(
        plan.queue_syncs.begin(),
        plan.queue_syncs.end(),
        [&](const RenderGraph::CompiledQueueSync& sync) {
            return sync.signal_pass == transition && sync.gpu_wait_required;
        }
    ));
    suite.Check(
        transition_syncs == 2,
        test_name,
        "each later Compute batch must wait for the retained availability frontier"
    );
}

void TestSameNativeReadsDependOnTransitionFrontier(TestSuite& suite) {
    constexpr std::string_view test_name = "same-native reads depend on transition frontier";
    RenderGraph graph("SameNativeAvailability", RenderGraph::QueueTopology::SingleQueue());
    const auto  buffer = graph.CreateTransientBuffer(
        "Buffer", RenderGraph::BufferDesc{.byte_size = 64}
    );
    const auto writer = graph.AddRecordPass(
        "Write",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto transition = graph.AddRecordPass(
        "TransitionRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto sibling_a = graph.AddRecordPass(
        "CompatibleReadA",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto sibling_b = graph.AddRecordPass(
        "CompatibleReadB",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan,
            transition,
            sibling_a,
            RenderGraph::EdgeReasonKind::StateTransition,
            buffer.Untyped()
        ) &&
            HasEdgeReason(
                plan,
                transition,
                sibling_b,
                RenderGraph::EdgeReasonKind::StateTransition,
                buffer.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                writer,
                sibling_a,
                RenderGraph::EdgeReasonKind::ReadAfterWrite,
                buffer.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                writer,
                sibling_b,
                RenderGraph::EdgeReasonKind::ReadAfterWrite,
                buffer.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                sibling_a,
                sibling_b,
                RenderGraph::EdgeReasonKind::StateTransition,
                buffer.Untyped()
            ),
        test_name,
        "compatible reads must fan out from the transition frontier without restoring a stale writer"
    );
    suite.Check(
        plan.dependency_levels.size() == 3 &&
            DependencyLevelContains(plan.dependency_levels[0], writer) &&
            DependencyLevelContains(plan.dependency_levels[1], transition) &&
            DependencyLevelContains(plan.dependency_levels[2], sibling_a) &&
            DependencyLevelContains(plan.dependency_levels[2], sibling_b),
        test_name,
        "the writer, transition, and compatible sibling reads must occupy three dependency levels"
    );
    suite.Check(
        FindBarrier(plan, buffer.Untyped(), transition, sibling_a) == nullptr &&
            FindBarrier(plan, buffer.Untyped(), transition, sibling_b) == nullptr &&
            plan.queue_syncs.empty(),
        test_name,
        "a same-state same-queue availability edge must not create a barrier or queue sync"
    );
}

void TestOwnershipAcquireOrdersSiblingNativeQueue(TestSuite& suite) {
    constexpr std::string_view test_name = "ownership acquire orders sibling native queue";
    RenderGraph graph(
        "OwnershipAcquireFrontier",
        RenderGraph::QueueTopology{
            .graphics = {RenderGraph::QueueRole::Graphics, 1, 1},
            .compute  = {RenderGraph::QueueRole::Compute, 2, 1},
            .copy     = {RenderGraph::QueueRole::Copy, 0, 0},
        }
    );
    int        physical = 0;
    const auto buffer = graph.ImportBuffer(
        "Shared", &physical, RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    const auto acquire = graph.AddRecordPass(
        "GraphicsAcquire",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto sibling_read = graph.AddRecordPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan,
            acquire,
            sibling_read,
            RenderGraph::EdgeReasonKind::QueueOwnership,
            buffer.Untyped()
        ),
        test_name,
        "a sibling native queue must wait for the pass that acquired family ownership"
    );
    suite.Check(
        plan.queue_syncs.size() == 1 && plan.queue_syncs.front().gpu_wait_required &&
            plan.queue_syncs.front().signal_pass == acquire &&
            plan.queue_syncs.front().wait_pass == sibling_read,
        test_name,
        "the ownership acquire frontier must lower to one cross-native GPU wait"
    );
}

void TestImportAvailabilityOrdersSiblingNativeQueue(TestSuite& suite) {
    constexpr std::string_view test_name = "import availability orders sibling native queue";
    RenderGraph graph(
        "ImportAvailabilityFrontier",
        RenderGraph::QueueTopology{
            .graphics = {RenderGraph::QueueRole::Graphics, 1, 1},
            .compute  = {RenderGraph::QueueRole::Compute, 2, 1},
            .copy     = {RenderGraph::QueueRole::Copy, 0, 0},
        }
    );
    int        physical = 0;
    const auto buffer = graph.ImportBuffer(
        "Shared", &physical, RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    const auto first_read = graph.AddRecordPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto sibling_read = graph.AddRecordPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* boundary = FindBarrier(plan, buffer.Untyped(), {}, first_read);
    suite.Check(
        boundary != nullptr && boundary->import_boundary && boundary->memory_dependency &&
            !boundary->state_transition && !boundary->queue_ownership,
        test_name,
        "the first read must acquire the external write through an import memory barrier"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            first_read,
            sibling_read,
            RenderGraph::EdgeReasonKind::StateTransition,
            buffer.Untyped()
        ) &&
            HasEdgeReason(
                plan,
                first_read,
                sibling_read,
                RenderGraph::EdgeReasonKind::QueueOwnership,
                buffer.Untyped()
            ) &&
            plan.queue_syncs.size() == 1 && plan.queue_syncs.front().gpu_wait_required &&
            plan.queue_syncs.front().signal_pass == first_read &&
            plan.queue_syncs.front().wait_pass == sibling_read,
        test_name,
        "an Unspecified sibling access must wait for the pass that acquired external availability"
    );
}

void TestOwnershipTransitionCollapsesReaderFrontier(TestSuite& suite) {
    constexpr std::string_view test_name = "ownership transition collapses reader frontier";
    RenderGraph graph(
        "OwnershipStateFrontier",
        RenderGraph::QueueTopology{
            .graphics = {RenderGraph::QueueRole::Graphics, 1, 1},
            .compute  = {RenderGraph::QueueRole::Compute, 2, 1},
            .copy     = {RenderGraph::QueueRole::Copy, 0, 0},
        }
    );
    int        physical = 0;
    const auto buffer = graph.ImportBuffer(
        "Shared", &physical, RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::TransferSource,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    const auto old_owner_read = graph.AddRecordPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::GpuWorkType::Transfer);
            builder.Read(buffer, RenderGraph::BufferAccessType::TransferSource);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto acquire = graph.AddRecordPass(
        "GraphicsAcquire",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto transition = graph.AddRecordPass(
        "ComputeTransition",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto sibling_read = graph.AddRecordPass(
        "GraphicsSiblingRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* barrier = FindBarrier(plan, buffer.Untyped(), acquire, transition);
    suite.Check(
        barrier != nullptr && barrier->state_transition && !barrier->queue_ownership &&
            barrier->sources.size() == 1 &&
            HasBarrierSource(*barrier, acquire, RenderGraph::AccessMode::Read) &&
            !HasBarrierSource(*barrier, old_owner_read, RenderGraph::AccessMode::Read),
        test_name,
        "a same-family transition must not reuse a reader from the family that lost ownership"
    );
    suite.Check(
        !HasEdgeReason(
            plan,
            old_owner_read,
            transition,
            RenderGraph::EdgeReasonKind::StateTransition,
            buffer.Untyped()
        ),
        test_name,
        "the collapsed reader frontier must avoid a stale Copy-to-Compute transition edge"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            transition,
            sibling_read,
            RenderGraph::EdgeReasonKind::StateTransition,
            buffer.Untyped()
        ) &&
            HasEdgeReason(
                plan,
                transition,
                sibling_read,
                RenderGraph::EdgeReasonKind::QueueOwnership,
                buffer.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                acquire,
                sibling_read,
                RenderGraph::EdgeReasonKind::QueueOwnership,
                buffer.Untyped()
            ),
        test_name,
        "a state transition must advance both availability anchors past the old acquire pass"
    );
}

void TestWriterAdvancesAvailabilityFrontiers(TestSuite& suite) {
    constexpr std::string_view test_name = "writer advances availability frontiers";
    RenderGraph graph(
        "WriterFrontier",
        RenderGraph::QueueTopology{
            .graphics = {RenderGraph::QueueRole::Graphics, 1, 1},
            .compute  = {RenderGraph::QueueRole::Compute, 2, 1},
            .copy     = {RenderGraph::QueueRole::Copy, 0, 0},
        }
    );
    int        physical = 0;
    const auto buffer = graph.ImportBuffer(
        "Shared", &physical, RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto transition = graph.AddRecordPass(
        "GraphicsTransition",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto writer = graph.AddRecordPass(
        "ComputeWrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto sibling_read = graph.AddRecordPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan,
            writer,
            sibling_read,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            buffer.Untyped()
        ) &&
            !HasEdgeReason(
                plan,
                transition,
                sibling_read,
                RenderGraph::EdgeReasonKind::StateTransition,
                buffer.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                transition,
                sibling_read,
                RenderGraph::EdgeReasonKind::QueueOwnership,
                buffer.Untyped()
            ),
        test_name,
        "the writer must replace older state and ownership establishment anchors"
    );
    const auto stale_sync = std::find_if(
        plan.queue_syncs.begin(),
        plan.queue_syncs.end(),
        [&](const RenderGraph::CompiledQueueSync& sync) {
            return sync.signal_pass == transition && sync.wait_pass == sibling_read;
        }
    );
    suite.Check(
        stale_sync == plan.queue_syncs.end(),
        test_name,
        "an older transition must not create a direct GPU sync to a post-write reader"
    );
}

void TestOwnershipEpochDoesNotReusePriorFamilySources(TestSuite& suite) {
    constexpr std::string_view test_name = "ownership epochs do not reuse prior family sources";
    RenderGraph graph(
        "OwnershipEpochs",
        RenderGraph::QueueTopology{
            .graphics = {RenderGraph::QueueRole::Graphics, 0, 0},
            .compute  = {RenderGraph::QueueRole::Compute, 1, 1},
            .copy     = {RenderGraph::QueueRole::Copy, 2, 2},
        }
    );
    int        physical = 0;
    const auto buffer = graph.ImportBuffer(
        "Shared", &physical, RenderGraph::BufferDesc{.byte_size = 64}
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto graphics_epoch_zero = graph.AddRecordPass(
        "GraphicsEpochZero",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_epoch_zero = graph.AddRecordPass(
        "ComputeEpochZero",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto graphics_epoch_one = graph.AddRecordPass(
        "GraphicsEpochOne",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_epoch_one = graph.AddRecordPass(
        "ComputeEpochOne",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* transfer = FindBarrier(
        plan, buffer.Untyped(), graphics_epoch_one, compute_epoch_one
    );
    suite.Check(
        transfer != nullptr && transfer->queue_ownership && transfer->sources.size() == 1 &&
            HasBarrierSource(*transfer, graphics_epoch_one, RenderGraph::AccessMode::Read) &&
            !HasBarrierSource(*transfer, graphics_epoch_zero, RenderGraph::AccessMode::Read) &&
            !HasBarrierSource(*transfer, compute_epoch_zero, RenderGraph::AccessMode::Read),
        test_name,
        "a repeated family must expose only the source from its current ownership epoch"
    );
    suite.Check(
        !HasEdgeReason(
            plan,
            graphics_epoch_zero,
            compute_epoch_one,
            RenderGraph::EdgeReasonKind::QueueOwnership,
            buffer.Untyped()
        ),
        test_name,
        "a prior Graphics ownership epoch must not transfer directly to the later Compute epoch"
    );
    const auto stale_sync = std::find_if(
        plan.queue_syncs.begin(),
        plan.queue_syncs.end(),
        [&](const RenderGraph::CompiledQueueSync& sync) {
            return sync.signal_pass == graphics_epoch_zero && sync.wait_pass == compute_epoch_one;
        }
    );
    suite.Check(
        stale_sync == plan.queue_syncs.end(),
        test_name,
        "a prior ownership epoch must not create a stale cross-family sync"
    );
}

void TestTokenCrossQueueSyncHasNoOwnership(TestSuite& suite) {
    constexpr std::string_view test_name = "token cross-queue synchronization";
    RenderGraph graph("TokenSync", RenderGraph::QueueTopology::DedicatedQueues());
    const auto  token = graph.CreateTransientToken("Token");
    const auto  producer = graph.AddRecordPass(
        "Produce",
        [token](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Write(token);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto consumer = graph.AddRecordPass(
        "Consume",
        [token](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Graphics);
            builder.Read(token);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan, producer, consumer, RenderGraph::EdgeReasonKind::ReadAfterWrite, token.Untyped()
        ) &&
            plan.queue_syncs.size() == 1 && plan.queue_syncs.front().gpu_wait_required,
        test_name,
        "a token hazard must create cross-native-queue synchronization"
    );
    suite.Check(
        plan.barriers.empty(),
        test_name,
        "a logical token must never create resource state or ownership barriers"
    );
}

void TestBatchPairSyncDeduplication(TestSuite& suite) {
    constexpr std::string_view test_name = "batch-pair synchronization deduplication";
    RenderGraph graph("SyncDedup", RenderGraph::QueueTopology::DedicatedQueues());
    const auto  first = graph.CreateTransientBuffer("First", RenderGraph::BufferDesc{.byte_size = 64});
    const auto  second = graph.CreateTransientBuffer("Second", RenderGraph::BufferDesc{.byte_size = 64});
    graph.AddRecordPass(
        "ProduceFirst",
        [first](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Write(first, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "ProduceSecond",
        [second](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Write(second, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "ConsumeBoth",
        [first, second](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Graphics);
            builder.Read(first, RenderGraph::BufferAccessType::ShaderRead);
            builder.Read(second, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.queue_batches.size() == 2 && plan.queue_syncs.size() == 1,
        test_name,
        "all dependencies between one producer/consumer batch pair must share one sync"
    );
    suite.Check(
        !plan.queue_syncs.empty() && plan.queue_syncs.front().dependency_edges.size() == 2 &&
            plan.queue_syncs.front().barriers.size() == 2,
        test_name,
        "the deduplicated sync must retain both dependency edges and both barriers"
    );
}

void TestGpuWorkTypesAndBarrierSources(TestSuite& suite) {
    constexpr std::string_view test_name = "GPU work types and barrier sources";
    RenderGraph graph("DomainsAndSources", RenderGraph::QueueTopology::DedicatedQueues());
    const auto buffer = graph.CreateTransientBuffer(
        "Shared",
        RenderGraph::BufferDesc{
            .byte_size    = 64,
            .sharing_mode = RenderGraph::SharingMode::Concurrent,
        }
    );
    const auto producer = graph.AddRecordPass(
        "Producer",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto reader_a = graph.AddRecordPass(
        "ReaderA",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::RayTracing);
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto reader_b = graph.AddRecordPass(
        "ReaderB",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::RayTracing);
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto writer = graph.AddRecordPass(
        "Writer",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::RayTracing);
            builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* producer_access = FindAccess(plan, producer, buffer.Untyped());
    const auto* reader_access   = FindAccess(plan, reader_a, buffer.Untyped());
    suite.Check(
        producer_access != nullptr &&
            producer_access->domain == RenderGraph::ExecutionDomain{
                RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute
            } &&
            reader_access != nullptr &&
            reader_access->domain == RenderGraph::ExecutionDomain{
                RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::RayTracing
            },
        test_name,
        "compiled accesses must retain their declared GPU work types"
    );

    const auto reader_a_barrier = std::find_if(
        plan.barriers.begin(), plan.barriers.end(), [&](const RenderGraph::CompiledBarrier& barrier) {
            return barrier.resource == buffer.Untyped() && barrier.dst_pass == reader_a;
        }
    );
    suite.Check(
        reader_a_barrier != plan.barriers.end() && reader_a_barrier->src_domain.work ==
                                                          RenderGraph::GpuWorkType::Compute &&
            reader_a_barrier->dst_domain.work == RenderGraph::GpuWorkType::RayTracing &&
            HasBarrierSource(*reader_a_barrier, producer, RenderGraph::AccessMode::Write),
        test_name,
        "a fan-out barrier must retain the producer source and both work categories"
    );

    const auto fan_in = std::find_if(
        plan.barriers.begin(), plan.barriers.end(), [&](const RenderGraph::CompiledBarrier& barrier) {
            return barrier.resource == buffer.Untyped() && barrier.dst_pass == writer;
        }
    );
    suite.Check(
        fan_in != plan.barriers.end() &&
            HasBarrierSource(*fan_in, producer, RenderGraph::AccessMode::Write) &&
            HasBarrierSource(*fan_in, reader_a, RenderGraph::AccessMode::Read) &&
            HasBarrierSource(*fan_in, reader_b, RenderGraph::AccessMode::Read),
        test_name,
        "a fan-in write barrier must retain the writer and every active reader source"
    );
}

void TestPartialRangeBarrier(TestSuite& suite) {
    constexpr std::string_view test_name = "partial-range barrier";
    RenderGraph graph("PartialBarrier");
    int         physical = 0;
    const auto  buffer = graph.ImportBuffer("Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 128});
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto write = graph.AddRecordPass(
        "WriteMiddle",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(
                buffer,
                RenderGraph::BufferAccessType::Storage,
                RenderGraph::BufferRange{.offset = 32, .size = 32}
            );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto* barrier = FindBarrier(graph.GetCompiledPlan(), buffer.Untyped(), {}, write);
    suite.Check(
        barrier != nullptr && RenderGraph::GetRangeKind(barrier->range) == RenderGraph::ResourceKind::Buffer &&
            std::get<RenderGraph::BufferRange>(barrier->range) == RenderGraph::BufferRange{.offset = 32, .size = 32},
        test_name,
        "a partial access must emit one barrier for exactly its canonical byte interval"
    );
}

void TestMixedQueueExecuteRemainsDeclarationOrder(TestSuite& suite) {
    constexpr std::string_view test_name = "mixed-queue serial execution order";
    RenderGraph graph("MixedQueueExecute", RenderGraph::QueueTopology::DedicatedQueues());
    std::vector<int> callbacks;
    const auto first = graph.AddRecordPass(
        "Compute",
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute)
                .SideEffect();
        },
        [&](Moer::Render::CommandList&) {
            callbacks.push_back(1);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto second = graph.AddRecordPass(
        "Graphics",
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Graphics)
                .SideEffect();
        },
        [&](Moer::Render::CommandList&) {
            callbacks.push_back(2);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto third = graph.AddRecordPass(
        "Copy",
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::GpuWorkType::Transfer).SideEffect();
        },
        [&](Moer::Render::CommandList&) {
            callbacks.push_back(3);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    suite.Check(
        graph.GetCompiledPlan().execution_order ==
            std::vector<RenderGraph::PassHandle>{first, second, third},
        test_name,
        "mixed logical queues must not change the production execution order"
    );
    suite.Check(RecordGraphForTest(graph), test_name, graph.GetCompileError());
    suite.Check(
        callbacks == std::vector<int>{1, 2, 3},
        test_name,
        "mixed-queue callbacks must still execute serially in declaration order"
    );
}

[[nodiscard]] std::string BuildStageTwoDump(TestSuite& suite, std::string_view test_name) {
    RenderGraph graph("StageTwoDump", RenderGraph::QueueTopology::DedicatedQueues());
    const auto  buffer = graph.CreateTransientBuffer(
        "Output",
        RenderGraph::BufferDesc{
            .byte_size = 128,
            .sharing_mode = RenderGraph::SharingMode::Exclusive,
        }
    );
    graph.AddRecordPass(
        "CopyProduce",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::GpuWorkType::Transfer);
            builder.Write(
                buffer,
                RenderGraph::BufferAccessType::TransferDestination,
                RenderGraph::BufferRange{.offset = 0, .size = 64}
            );
            builder.Write(
                buffer,
                RenderGraph::BufferAccessType::TransferDestination,
                RenderGraph::BufferRange{.offset = 64, .size = 64}
            );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "ComputeConsume",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    return graph.Dump();
}

void TestStageTwoDumpDeterminism(TestSuite& suite) {
    constexpr std::string_view test_name = "Stage 2 dump determinism";
    const std::string first  = BuildStageTwoDump(suite, test_name);
    const std::string second = BuildStageTwoDump(suite, test_name);
    suite.Check(first == second, test_name, "equivalent Stage 2 plans must have byte-identical dumps");
    suite.Check(
        Contains(first, "barrier_plan=explicit") &&
            Contains(first, "sync_plan=queue-dag external_endpoints=unbound") &&
            Contains(first, "barriers:\n") && Contains(first, "queue_batches:\n") &&
            Contains(first, "queue_syncs:\n") && Contains(first, "work=compute") &&
            Contains(first, "flags=[execution,memory,transition,queue,ownership"),
        test_name,
        "the deterministic dump must retain state, domain, queue, and ownership decisions"
    );
}

void TestBarrierSourcesIgnoreUnrelatedLastRead(TestSuite& suite) {
    constexpr std::string_view test_name = "barrier sources ignore unrelated last read";
    RenderGraph graph(
        "SourceEndpoint",
        RenderGraph::QueueTopology{
            .graphics = {RenderGraph::QueueRole::Graphics, 0, 0},
            .compute  = {RenderGraph::QueueRole::Compute, 1, 0},
            .copy     = {RenderGraph::QueueRole::Copy, 2, 0},
        }
    );
    const auto buffer = graph.CreateTransientBuffer(
        "Concurrent",
        RenderGraph::BufferDesc{
            .byte_size    = 64,
            .sharing_mode = RenderGraph::SharingMode::Concurrent,
        }
    );
    const auto writer = graph.AddRecordPass(
        "Writer",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_read = graph.AddRecordPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto graphics_read = graph.AddRecordPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* barrier = FindBarrier(plan, buffer.Untyped(), writer, graphics_read);
    suite.Check(
        barrier != nullptr && !barrier->queue_dependency && barrier->src_pass == writer &&
            HasBarrierSource(*barrier, writer, RenderGraph::AccessMode::Write) &&
            !HasBarrierSource(*barrier, compute_read, RenderGraph::AccessMode::Read),
        test_name,
        "an unrelated R/R endpoint must not replace the actual RAW source or create a queue handoff"
    );
    const bool has_orphan_gpu_sync = std::any_of(
        plan.queue_syncs.begin(),
        plan.queue_syncs.end(),
        [](const RenderGraph::CompiledQueueSync& sync) {
            return sync.signal_batch == 1 && sync.wait_batch == 2 && sync.gpu_wait_required;
        }
    );
    suite.Check(
        !has_orphan_gpu_sync,
        test_name,
        "the unrelated compute read must not produce a GPU wait before the final graphics read"
    );
}

void TestFanInBarrierPlacementCoversEverySourceBatch(TestSuite& suite) {
    constexpr std::string_view test_name = "fan-in barrier placement covers every source batch";
    RenderGraph graph("FanInPlacement", RenderGraph::QueueTopology::DedicatedQueues());
    int         physical = 0;
    const auto  buffer = graph.ImportBuffer(
        "Concurrent",
        &physical,
        RenderGraph::BufferDesc{
            .byte_size    = 64,
            .sharing_mode = RenderGraph::SharingMode::Concurrent,
        }
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::TransferSource,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    const auto copy_read = graph.AddRecordPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::GpuWorkType::Transfer);
            builder.Read(buffer, RenderGraph::BufferAccessType::TransferSource);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto graphics_read = graph.AddRecordPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::TransferSource);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_write = graph.AddRecordPass(
        "ComputeWrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Write(buffer, RenderGraph::BufferAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto barrier_it = std::find_if(
        plan.barriers.begin(),
        plan.barriers.end(),
        [&](const RenderGraph::CompiledBarrier& barrier) {
            return barrier.resource == buffer.Untyped() && barrier.dst_pass == compute_write &&
                   HasBarrierSource(barrier, copy_read, RenderGraph::AccessMode::Read) &&
                   HasBarrierSource(barrier, graphics_read, RenderGraph::AccessMode::Read);
        }
    );
    suite.Check(barrier_it != plan.barriers.end(), test_name, "the fan-in barrier must retain both readers");
    if (barrier_it == plan.barriers.end()) {
        return;
    }
    const uint32_t barrier_index = static_cast<uint32_t>(barrier_it - plan.barriers.begin());
    suite.Check(
        plan.queue_batches.size() == 3 &&
            std::find(
                plan.queue_batches[0].post_barriers.begin(),
                plan.queue_batches[0].post_barriers.end(),
                barrier_index
            ) != plan.queue_batches[0].post_barriers.end() &&
            std::find(
                plan.queue_batches[1].post_barriers.begin(),
                plan.queue_batches[1].post_barriers.end(),
                barrier_index
            ) != plan.queue_batches[1].post_barriers.end(),
        test_name,
        "every cross-native source batch must receive the split-release placement hint"
    );
    const uint32_t associated_syncs = static_cast<uint32_t>(std::count_if(
        plan.queue_syncs.begin(),
        plan.queue_syncs.end(),
        [&](const RenderGraph::CompiledQueueSync& sync) {
            return sync.wait_batch == 2 && sync.gpu_wait_required &&
                   std::find(sync.barriers.begin(), sync.barriers.end(), barrier_index) !=
                       sync.barriers.end();
        }
    ));
    suite.Check(
        associated_syncs == 2,
        test_name,
        "the barrier must be associated with both GPU waits feeding the destination batch"
    );
}

void TestUntouchedBoundaryWriteRequiresMemoryDependency(TestSuite& suite) {
    constexpr std::string_view test_name = "untouched boundary write dependency";
    RenderGraph graph("UntouchedBoundaryWrite");
    int         physical = 0;
    const auto  buffer = graph.ImportBuffer("Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 64});
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    graph.Export(
        buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto* barrier = FindBarrier(graph.GetCompiledPlan(), buffer.Untyped(), {}, {});
    suite.Check(
        barrier != nullptr && barrier->memory_dependency && barrier->state_transition,
        test_name,
        "an untouched imported write must be made visible to the declared external reader"
    );
}

void TestShaderReadStatesRequireTransition(TestSuite& suite) {
    constexpr std::string_view test_name =
        "shader resource and sampled states require a transition";
    RenderGraph graph("DistinctShaderReadStates");
    int         physical = 0;
    const auto  texture =
        graph.ImportTexture("Texture", &physical, RenderGraph::TextureDesc{});
    graph.SetInitialState(
        texture,
        RenderGraph::TextureAccessType::Storage,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto storage_read = graph.AddRecordPass(
        "StorageRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureAccessType::Storage);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto sampled_read = graph.AddRecordPass(
        "SampledRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureAccessType::Sampled);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.Export(
        texture,
        RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* barrier =
        FindBarrier(plan, texture.Untyped(), storage_read, sampled_read);
    suite.Check(
        barrier != nullptr && barrier->state_transition &&
            barrier->before_state == RenderGraph::ResourceState{RenderGraph::TextureAccessType::Storage} &&
            barrier->after_state == RenderGraph::ResourceState{RenderGraph::TextureAccessType::Sampled} &&
            HasEdgeReason(
                plan,
                storage_read,
                sampled_read,
                RenderGraph::EdgeReasonKind::StateTransition,
                texture.Untyped()
            ),
        test_name,
        "physically distinct Vulkan shader-read layouts must retain a compiler transition"
    );
}

void TestReadTransitionWaitsForEveryActiveReader(TestSuite& suite) {
    constexpr std::string_view test_name = "read transition waits for every active reader";
    RenderGraph graph("ReadTransitionFanIn", RenderGraph::QueueTopology::DedicatedQueues());
    int         physical = 0;
    const auto  buffer = graph.ImportBuffer(
        "Concurrent",
        &physical,
        RenderGraph::BufferDesc{
            .byte_size    = 64,
            .sharing_mode = RenderGraph::SharingMode::Concurrent,
        }
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferAccessType::TransferSource,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    const auto copy_read = graph.AddRecordPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::GpuWorkType::Transfer);
            builder.Read(buffer, RenderGraph::BufferAccessType::TransferSource);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto graphics_read = graph.AddRecordPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferAccessType::TransferSource);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto compute_read = graph.AddRecordPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto barrier = std::find_if(
        plan.barriers.begin(),
        plan.barriers.end(),
        [&](const RenderGraph::CompiledBarrier& candidate) {
            return candidate.resource == buffer.Untyped() && candidate.dst_pass == compute_read &&
                   candidate.state_transition;
        }
    );
    suite.Check(
        barrier != plan.barriers.end() &&
            HasBarrierSource(*barrier, copy_read, RenderGraph::AccessMode::Read) &&
            HasBarrierSource(*barrier, graphics_read, RenderGraph::AccessMode::Read) &&
            HasEdgeReason(
                plan,
                copy_read,
                compute_read,
                RenderGraph::EdgeReasonKind::StateTransition,
                buffer.Untyped()
            ) &&
            HasEdgeReason(
                plan,
                graphics_read,
                compute_read,
                RenderGraph::EdgeReasonKind::StateTransition,
                buffer.Untyped()
            ),
        test_name,
        "a read-only state transition must fan in every reader still using the previous state"
    );
}

void TestUndefinedImportMustBeInitializedBeforeRead(TestSuite& suite) {
    constexpr std::string_view test_name = "undefined import initialization";
    int                        physical = 0;

    RenderGraph read_graph("UndefinedRead");
    const auto  read_buffer = read_graph.ImportBuffer(
        "Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 128}
    );
    read_graph.SetInitialState(
        read_buffer,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    read_graph.AddRecordPass(
        "InvalidRead",
        [read_buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(
                read_buffer,
                RenderGraph::BufferAccessType::ShaderRead,
                RenderGraph::BufferRange{.offset = 0, .size = 64}
            );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !read_graph.Compile() && Contains(read_graph.GetCompileError(), "read before its first producer"),
        test_name,
        "an explicit Undefined range must reject a read before initialization"
    );

    RenderGraph write_graph("UndefinedWrite");
    const auto  write_buffer = write_graph.ImportBuffer(
        "Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 128}
    );
    write_graph.SetInitialState(
        write_buffer,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    const auto write = write_graph.AddRecordPass(
        "Initialize",
        [write_buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(
                write_buffer,
                RenderGraph::BufferAccessType::Storage,
                RenderGraph::BufferRange{.offset = 0, .size = 64}
            );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    write_graph.Export(
        write_buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    suite.Check(write_graph.Compile(), test_name, write_graph.GetCompileError());
    const auto* barrier = FindBarrier(write_graph.GetCompiledPlan(), write_buffer.Untyped(), {}, write);
    suite.Check(
        barrier != nullptr && barrier->discard_previous_contents,
        test_name,
        "the first write after Undefined must be represented as a discard transition"
    );
    const auto* initialized_export_barrier =
        FindBarrier(write_graph.GetCompiledPlan(), write_buffer.Untyped(), write, {});
    suite.Check(
        initialized_export_barrier != nullptr && initialized_export_barrier->export_boundary &&
            !initialized_export_barrier->discard_previous_contents &&
            initialized_export_barrier->after_access == RenderGraph::AccessMode::Read,
        test_name,
        "a range initialized in the graph must remain exportable to an external reader"
    );

    RenderGraph read_export_graph("UndefinedReadExport");
    const auto  read_export_buffer = read_export_graph.ImportBuffer(
        "Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 128}
    );
    read_export_graph.SetInitialState(
        read_export_buffer,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange::Whole()
    );
    read_export_graph.Export(
        read_export_buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    read_export_graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !read_export_graph.Compile() &&
            Contains(read_export_graph.GetCompileError(), "uninitialized subresources"),
        test_name,
        "an untouched Undefined import must not be exported to an external reader"
    );

    RenderGraph write_export_graph("UndefinedWriteExport");
    const auto  write_export_buffer = write_export_graph.ImportBuffer(
        "Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 128}
    );
    write_export_graph.SetInitialState(
        write_export_buffer,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange::Whole()
    );
    write_export_graph.Export(
        write_export_buffer,
        RenderGraph::BufferAccessType::Storage,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Write,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    write_export_graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        write_export_graph.Compile(),
        test_name,
        "an external writer does not require preserved imported contents: " +
            write_export_graph.GetCompileError()
    );
    const auto* write_export_barrier =
        FindBarrier(write_export_graph.GetCompiledPlan(), write_export_buffer.Untyped(), {}, {});
    suite.Check(
        write_export_barrier != nullptr && write_export_barrier->export_boundary &&
            write_export_barrier->discard_previous_contents &&
            write_export_barrier->state_transition && write_export_barrier->memory_dependency &&
            write_export_barrier->after_access == RenderGraph::AccessMode::Write &&
            RenderGraph::GetRangeKind(write_export_barrier->range) == RenderGraph::ResourceKind::Buffer &&
            std::get<RenderGraph::BufferRange>(write_export_barrier->range).offset == 0 &&
            std::get<RenderGraph::BufferRange>(write_export_barrier->range).size == 64 &&
            write_export_graph.GetCompiledPlan().state_plan_complete,
        test_name,
        "a write-only export of Undefined contents must remain an explicit discard boundary"
    );

    RenderGraph read_write_export_graph("UndefinedReadWriteExport");
    const auto  read_write_export_buffer = read_write_export_graph.ImportBuffer(
        "Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 128}
    );
    read_write_export_graph.SetInitialState(
        read_write_export_buffer,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    read_write_export_graph.Export(
        read_write_export_buffer,
        RenderGraph::BufferAccessType::Storage,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::ReadWrite,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    read_write_export_graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        !read_write_export_graph.Compile() &&
            Contains(read_write_export_graph.GetCompileError(), "uninitialized subresources"),
        test_name,
        "a ReadWrite export must preserve the same initialization requirement as a read"
    );

    RenderGraph disjoint_export_graph("DisjointUndefinedReadExport");
    const auto  disjoint_export_buffer = disjoint_export_graph.ImportBuffer(
        "Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 128}
    );
    disjoint_export_graph.SetInitialState(
        disjoint_export_buffer,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    disjoint_export_graph.Export(
        disjoint_export_buffer,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        RenderGraph::BufferRange{.offset = 64, .size = 64}
    );
    disjoint_export_graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        disjoint_export_graph.Compile(),
        test_name,
        "an Undefined import range must not invalidate a disjoint typed export: " +
            disjoint_export_graph.GetCompileError()
    );

    RenderGraph legacy_export_graph("LegacyUndefinedExport");
    const auto  legacy_export_buffer = legacy_export_graph.ImportBuffer(
        "Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 128}
    );
    legacy_export_graph.SetInitialState(
        legacy_export_buffer,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    legacy_export_graph.Export(legacy_export_buffer);
    legacy_export_graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        legacy_export_graph.Compile(),
        test_name,
        "legacy import export has no external read contract and must remain compatible: " +
            legacy_export_graph.GetCompileError()
    );
}

void TestUnknownExportMakesStatePlanIncomplete(TestSuite& suite) {
    constexpr std::string_view test_name = "unknown exported state plan";
    RenderGraph graph("UnknownExport");
    int         physical = 0;
    const auto  buffer = graph.ImportBuffer("Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 64});
    graph.Export(buffer);
    graph.AddRecordPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    suite.Check(
        !graph.GetCompiledPlan().state_plan_complete,
        test_name,
        "an exported physical cell with no known state must keep the plan visibly incomplete"
    );
}

void TestMergedAccessStateDeterminesPlanCompleteness(TestSuite& suite) {
    constexpr std::string_view test_name = "merged access state completeness";

    const auto check_whole_resource_merge = [&](bool unspecified_first) {
        RenderGraph graph(
            unspecified_first ? "UnspecifiedThenExplicitState" : "ExplicitThenUnspecifiedState"
        );
        const auto texture = graph.CreateTransientTexture(
            "Color", RenderGraph::TextureDesc{.mip_count = 1, .layer_count = 1}
        );
        const auto pass = graph.AddRecordPass(
            "WriteColor",
            [=](RenderGraph::PassBuilder& builder) {
                if (unspecified_first) {
                    builder.Write(texture);
                    builder.Write(texture, RenderGraph::TextureAccessType::ColorAttachment);
                } else {
                    builder.Write(texture, RenderGraph::TextureAccessType::ColorAttachment);
                    builder.Write(texture);
                }
            },
            [](Moer::Render::CommandList&) {},
            RenderGraph::PassRecordPolicy::Serial
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const auto& plan   = graph.GetCompiledPlan();
        const auto* access = FindAccess(plan, pass, texture.Untyped());
        suite.Check(
            plan.state_plan_complete && plan.accesses.size() == 1 && access != nullptr &&
                access->mode == RenderGraph::AccessMode::Write &&
                access->state == RenderGraph::ResourceState{RenderGraph::TextureAccessType::ColorAttachment},
            test_name,
            "an explicit declaration must canonicalize the same atomic cell regardless of order"
        );
    };

    check_whole_resource_merge(true);
    check_whole_resource_merge(false);

    RenderGraph disjoint_graph("DisjointUnspecifiedAndExplicitStates");
    const auto  disjoint_texture = disjoint_graph.CreateTransientTexture(
        "MipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    disjoint_graph.AddRecordPass(
        "WriteMips",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                disjoint_texture,
                RenderGraph::TextureAccessType::ColorAttachment,
                RenderGraph::TextureRange::Mips(0, 1)
            );
            builder.Write(disjoint_texture, RenderGraph::TextureRange::Mips(1, 1));
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(disjoint_graph.Compile(), test_name, disjoint_graph.GetCompileError());
    suite.Check(
        !disjoint_graph.GetCompiledPlan().state_plan_complete,
        test_name,
        "an unspecified declaration on a disjoint atomic cell must keep the plan incomplete"
    );

    RenderGraph history_graph("UnspecifiedHistoryThenExplicitState");
    const auto  history_texture = history_graph.CreateTransientTexture(
        "History", RenderGraph::TextureDesc{.mip_count = 1, .layer_count = 1}
    );
    history_graph.AddRecordPass(
        "UnspecifiedWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(history_texture);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    history_graph.AddRecordPass(
        "ExplicitWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(history_texture, RenderGraph::TextureAccessType::ColorAttachment);
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(history_graph.Compile(), test_name, history_graph.GetCompileError());
    suite.Check(
        !history_graph.GetCompiledPlan().state_plan_complete,
        test_name,
        "a later explicit state must not hide an unknown state from an earlier pass"
    );
}

void TestInvalidRecordingPolicyFails(TestSuite& suite) {
    RenderGraph graph("InvalidRecordingPolicy");
    graph.AddRecordPass(
        "Invalid", {}, [](Moer::Render::CommandList&) {},
        static_cast<RenderGraph::PassRecordPolicy>(255)
    );
    suite.Check(!graph.Compile() && Contains(graph.GetCompileError(), "invalid recording policy"),
                "invalid recording policy", "an invalid policy must fail before scheduling");
}

void TestZeroRecordingWorkloadFails(TestSuite& suite) {
    RenderGraph graph("ZeroRecordingWorkload");
    int record_calls = 0;
    graph.AddRecordPass("Invalid", [](RenderGraph::PassBuilder& pass) {
        pass.SideEffect().RecordWorkload(0);
    }, [&](Moer::Render::CommandList&) { ++record_calls; }, RenderGraph::PassRecordPolicy::ParallelAllowed);
    suite.Check(!graph.Compile() && Contains(graph.GetCompileError(), "positive units") && record_calls == 0,
        "zero recording workload", "zero workload must fail declaration before scheduling callbacks");
}

void TestRecordingBatchPlanAndClassification(TestSuite& suite) {
    constexpr std::string_view test_name =
        "frontend record plan and classification";
    RenderGraph                graph("RecordingBatches");
    const auto                 hiz_token    = graph.CreateTransientToken("HiZ");
    const auto                 shadow_token = graph.CreateTransientToken("ShadowMask");

    const auto hiz = graph.AddRecordPass(
        "HiZBuild",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(hiz_token).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    const auto shadow = graph.AddRecordPass(
        "DirectionalShadowMask",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(shadow_token).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    const auto commit = graph.AddRecordPass(
        "CommitHiZHistory",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(hiz_token).DependsOn(hiz).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.frontend_record_units.size() == 3 &&
            plan.frontend_record_units[0].pass == hiz &&
            plan.frontend_record_units[1].pass == shadow &&
            plan.frontend_record_units[2].pass == commit,
        test_name,
        "the compiler must emit one stable frontend record unit per pass"
    );
    suite.Check(
        plan.frontend_record_units.size() == 3 &&
            plan.frontend_record_units[0].record_policy ==
                RenderGraph::PassRecordPolicy::ParallelAllowed &&
            plan.frontend_record_units[1].record_policy ==
                RenderGraph::PassRecordPolicy::ParallelAllowed &&
            plan.frontend_record_units[2].record_policy ==
                RenderGraph::PassRecordPolicy::Serial,
        test_name,
        "recording policy must survive compilation"
    );
    suite.Check(
        plan.frontend_record_units.size() == 3 &&
            plan.frontend_record_units[0].dependency_level_index ==
                plan.frontend_record_units[1].dependency_level_index &&
            plan.frontend_record_units[2].dependency_level_index >
                plan.frontend_record_units[0].dependency_level_index,
        test_name,
        "independent recording passes must share a dependency level while their consumer follows"
    );
    suite.Check(
        plan.frontend_dispatch_groups.size() == 2 &&
            plan.frontend_dispatch_groups[0].first_unit == 0 &&
            plan.frontend_dispatch_groups[0].unit_count == 2 &&
            plan.frontend_dispatch_groups[0].record_policy ==
                RenderGraph::PassRecordPolicy::ParallelAllowed &&
            plan.frontend_dispatch_groups[1].first_unit == 2 &&
            plan.frontend_dispatch_groups[1].unit_count == 1 &&
            plan.frontend_dispatch_groups[1].record_policy ==
                RenderGraph::PassRecordPolicy::Serial,
        test_name,
        "the compiler must precompute contiguous CPU recording dispatch groups"
    );
    suite.Check(
        Contains(graph.Dump(), "frontend_record_units:\n") &&
            Contains(
                graph.Dump(),
                "record=parallel-allowed native=parallel level=0"
            ),
        test_name,
        "the deterministic dump must expose the executable CPU recording schedule"
    );

}

class FakeUiViewportResources final :
    public Moer::Render::UiViewportRenderResources {
public:
    explicit FakeUiViewportResources(uint64_t pending_slot) :
        pending_slot(pending_slot) {}

    [[nodiscard]] uint64_t
    GetPendingRecordingSlot() const noexcept override {
        return pending_slot;
    }

    [[nodiscard]] bool
    IsPendingRecordingSlot(uint64_t slot) const noexcept override {
        return slot == pending_slot;
    }

    void CommitRecordingSlot(uint64_t slot) noexcept override {
        if (slot == pending_slot) {
            ++pending_slot;
            ++commit_count;
        }
    }

    [[nodiscard]] uint64_t PendingSlot() const {
        return pending_slot;
    }

    [[nodiscard]] uint32_t CommitCount() const {
        return commit_count;
    }

    void SetPendingSlot(uint64_t slot) {
        pending_slot = slot;
    }

private:
    uint64_t pending_slot = 0;
    uint32_t commit_count = 0;
};

class FakeUiDrawBackend final : public Moer::Render::UiDrawFrameBackend {
public:
    void RenderGUI(
        Moer::Render::CommandList&,
        const Moer::Render::TextureView&,
        const Moer::Render::UiDrawFramePacket& frame,
        Moer::Render::EUiDrawExecutionThread execution_thread
    ) override {
        ++render_count;
        last_execution_thread = execution_thread;
        const auto validate = [](const Moer::Render::UiViewportDrawPacket& viewport) {
            return viewport.commands.empty() ||
                   (viewport.recording_slot !=
                        Moer::Render::UiViewportDrawPacket::
                            InvalidRecordingSlot &&
                    viewport.render_resources &&
                    viewport.render_resources->IsPendingRecordingSlot(
                        viewport.recording_slot
                    ));
        };
        saw_valid_recording_slots =
            saw_valid_recording_slots && validate(frame.main_viewport);
        for (const auto& viewport : frame.platform_viewports) {
            saw_valid_recording_slots =
                saw_valid_recording_slots && validate(viewport);
        }
    }

    void PresentWindows(
        const Moer::Render::UiDrawFramePacket&,
        Moer::Render::EUiDrawExecutionThread
    ) override {
        ++present_count;
    }

    uint32_t render_count = 0;
    uint32_t present_count = 0;
    bool saw_valid_recording_slots = true;
    Moer::Render::EUiDrawExecutionThread last_execution_thread{
        Moer::Render::EUiDrawExecutionThread::Game
    };
};

class FakeUiTexture final : public Moer::Render::Texture {
public:
    explicit FakeUiTexture(
        std::string_view    name,
        ETextureUsageFlags usage =
            ETextureUsageFlags::SAMPLED |
            ETextureUsageFlags::COLOR_ATTACHMENT |
            ETextureUsageFlags::PRESENTATION_SOURCE |
            ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST,
        ETextureAspectFlags aspects = ETextureAspectFlags::COLOR,
        Extent2D            extent  = Extent2D(64, 64)
    ) :
        Texture(MakeInfo(usage, aspects, extent)) {
        SetName(name);
    }

    Moer::uint GetMipByteSize(Moer::uint) const override {
        return 4;
    }

    void SetName(std::string_view name) override {
        debug_name = std::string(name);
    }

private:
    static Moer::Render::TextureInfo MakeInfo(
        ETextureUsageFlags  usage,
        ETextureAspectFlags aspects,
        Extent2D            extent
    ) {
        using namespace Moer::Render;
        TextureInfo info{};
        info.usage        = usage;
        info.extent       = extent;
        info.num_mips     = 1;
        info.array_size   = 1;
        info.format       = PF_R8G8B8A8_UNORM;
        info.aspect_flags = aspects;
        return info;
    }
};

class FakeUiSwapchain final : public Moer::Render::Swapchain {
public:
    FakeUiSwapchain(
        Moer::Render::WindowSurfaceIdentity identity,
        Extent2D                             extent,
        bool                                 presentation_ready = true
    ) :
        identity(identity),
        presentation_ready(presentation_ready) {
        format = PF_R8G8B8A8_UNORM;
        size   = extent;
    }

    [[nodiscard]] bool Recreate(const Moer::Render::SwapchainCreateInfo&) override {
        return presentation_ready;
    }

    [[nodiscard]] bool IsPresentationReady() const noexcept override {
        return presentation_ready;
    }

    [[nodiscard]] Moer::Render::WindowSurfaceIdentity
    GetCommittedSurfaceIdentity() const noexcept override {
        return identity;
    }

private:
    Moer::Render::WindowSurfaceIdentity identity{};
    bool                                presentation_ready{true};
};

Moer::Render::UiDrawFramePacket MakeUiDrawPacket(
    const Moer::SharedPtr<FakeUiViewportResources>& resources,
    const Moer::SharedPtr<FakeUiDrawBackend>&       backend,
    bool                                             duplicate_resource
) {
    Moer::Render::UiDrawFramePacket packet{};
    packet.backend = backend;
    packet.main_viewport.render_resources = resources;
    packet.main_viewport.commands.emplace_back();
    if (duplicate_resource) {
        Moer::Render::UiViewportDrawPacket platform{};
        platform.render_resources = resources;
        platform.commands.emplace_back();
        packet.platform_viewports.emplace_back(std::move(platform));
    }
    return packet;
}

void TestUiDrawablePresentationContracts(TestSuite& suite) {
    using namespace Moer;
    using namespace Moer::Render;
    constexpr std::string_view test_name =
        "UI drawable presentation snapshot contract";

    const WindowSurfaceIdentity identity{
        .window_system          = EWindowSystemType::GLFW,
        .window_system_handle   = 0x1000u,
        .platform_window_handle = 0x2000u,
        .generation             = 1,
    };
    const WindowSurfaceIdentity other_identity{
        .window_system          = EWindowSystemType::GLFW,
        .window_system_handle   = 0x3000u,
        .platform_window_handle = 0x4000u,
        .generation             = 1,
    };
    const WindowFrameSnapshot detached_frame{
        .surface_identity     = identity,
        .logical_extent       = {32, 32},
        .drawable_extent      = {80, 80},
        .last_drawable_extent = {80, 80},
        .capture_sequence     = 1,
        .drawable_generation  = 1,
        .transition           = EWindowFrameTransition::Initial,
    };
    const UiViewportPresentationSnapshot committed_presentation{
        .surface_identity    = identity,
        .drawable_extent     = {64, 64},
        .drawable_generation = 1,
    };

    TextureRef   framebuffer{MoerNew(FakeUiTexture)("DetachedFramebuffer")};
    SwapchainRef matching_swapchain{
        MoerNew(FakeUiSwapchain)(identity, Extent2D(64, 64))
    };
    UiViewportDrawPacket viewport{
        .framebuffer  = framebuffer,
        .swapchain    = matching_swapchain,
        .window_frame = detached_frame,
    };
    BindUiViewportWindowFrame(viewport, detached_frame);
    BindUiViewportPresentation(viewport, committed_presentation);
    suite.Check(
        IsUiViewportPresentationCurrent(viewport) &&
            viewport.framebuffer_scale == float2(2.f, 2.f),
        test_name,
        "a committed 64x64 presentation for a raw 80x80 request was rejected or used raw scaling"
    );

    viewport.presentation.surface_identity = other_identity;
    suite.Check(
        !IsUiViewportPresentationCurrent(viewport),
        test_name,
        "a committed presentation with a different surface identity was accepted"
    );

    viewport.presentation = committed_presentation;
    viewport.swapchain =
        SwapchainRef{MoerNew(FakeUiSwapchain)(other_identity, Extent2D(64, 64))};
    suite.Check(
        !IsUiViewportPresentationCurrent(viewport),
        test_name,
        "a native swapchain with a different committed surface identity was accepted"
    );

    viewport.swapchain                         = matching_swapchain;
    viewport.presentation                      = committed_presentation;
    viewport.presentation.drawable_generation = 2;
    suite.Check(
        !IsUiViewportPresentationCurrent(viewport),
        test_name,
        "a committed presentation from a different drawable generation was accepted"
    );

    viewport.presentation = committed_presentation;
    viewport.swapchain =
        SwapchainRef{MoerNew(FakeUiSwapchain)(identity, Extent2D(64, 64), false)};
    suite.Check(
        !IsUiViewportPresentationCurrent(viewport),
        test_name,
        "a swapchain without a ready committed presentation was accepted"
    );

    viewport.swapchain = matching_swapchain;
    viewport.presentation.drawable_extent = {32, 32};
    suite.Check(
        !IsUiViewportPresentationCurrent(viewport),
        test_name,
        "a committed drawable extent that differs from the native swapchain was accepted"
    );

    viewport.presentation = committed_presentation;
    viewport.swapchain =
        SwapchainRef{MoerNew(FakeUiSwapchain)(identity, Extent2D(32, 32))};
    suite.Check(
        !IsUiViewportPresentationCurrent(viewport),
        test_name,
        "a swapchain extent that differs from the drawable snapshot was accepted"
    );

    TextureRef small_framebuffer{MoerNew(FakeUiTexture)(
        "SmallDetachedFramebuffer",
        ETextureUsageFlags::SAMPLED |
            ETextureUsageFlags::COLOR_ATTACHMENT |
            ETextureUsageFlags::PRESENTATION_SOURCE |
            ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST,
        ETextureAspectFlags::COLOR,
        Extent2D(32, 32)
    )};
    viewport.swapchain   = matching_swapchain;
    viewport.framebuffer = small_framebuffer;
    suite.Check(
        !IsUiViewportPresentationCurrent(viewport),
        test_name,
        "a framebuffer extent that differs from the committed swapchain was accepted"
    );

    viewport.framebuffer  = framebuffer;
    viewport.presentation = committed_presentation;
    UiDrawFramePacket draw_frame{};
    draw_frame.platform_viewports.emplace_back(std::move(viewport));
    UiCompositionFrameData composition{
        .enabled                = true,
        .separate_window        = true,
        .output_resolution      = {32, 32},
        .scene_color_position   = {3.f, 5.f},
        .scene_color_resolution = {10.f, 12.f},
        .window_frame_buffer    = framebuffer,
    };
    suite.Check(
        ResolveUiCompositionDrawableMetrics(
            composition,
            {},
            draw_frame
        ) &&
            composition.output_resolution == uint2(64, 64) &&
            composition.scene_color_position == float2(6.f, 10.f) &&
            composition.scene_color_resolution == float2(20.f, 24.f),
        test_name,
        "detached composition did not use the committed actual extent and 2x drawable scale"
    );

    UiClipRect detached_clip{
        .min = {11.f, 21.f},
        .max = {21.f, 31.f},
    };
    suite.Check(
        ConvertUiClipRectToDrawable(
            detached_clip,
            float2(10.f, 20.f),
            draw_frame.platform_viewports.front().framebuffer_scale
        ) &&
            detached_clip.min == float2(2.f, 2.f) &&
            detached_clip.max == float2(22.f, 22.f),
        test_name,
        "detached clip conversion used an assumed main-window 1x scale instead of its 2x scale"
    );

    UiViewportDrawPacket main_viewport{};
    main_viewport.commands.emplace_back(UiDrawCommand{
        .clip_min = {4.f, 8.f},
        .clip_max = {12.f, 16.f},
    });
    BindUiViewportWindowFrame(main_viewport, detached_frame);
    UiCompositionFrameData main_composition{
        .enabled                = true,
        .separate_window        = false,
        .scene_extent_resolved  = true,
        .output_resolution      = {80, 80},
        .scene_color_position   = {10.f, 20.f},
        .scene_color_resolution = {40.f, 20.f},
    };
    const bool main_retargeted = RetargetMainUiPresentation(
        main_viewport,
        main_composition,
        detached_frame,
        Extent2D(64, 64)
    );
    UiClipRect main_committed_clip{
        .min = main_viewport.commands.front().clip_min,
        .max = main_viewport.commands.front().clip_max,
    };
    const bool main_clip_resolved = ConvertUiClipRectToDrawable(
        main_committed_clip,
        float2(0.f, 0.f),
        main_viewport.framebuffer_scale
    );
    suite.Check(
        main_retargeted &&
            main_clip_resolved &&
            main_viewport.presentation.drawable_extent == Extent2D(64, 64) &&
            main_viewport.framebuffer_scale == float2(2.f, 2.f) &&
            main_viewport.commands.front().clip_min == float2(4.f, 8.f) &&
            main_viewport.commands.front().clip_max == float2(12.f, 16.f) &&
            main_committed_clip.min == float2(8.f, 16.f) &&
            main_committed_clip.max == float2(24.f, 32.f) &&
            main_composition.output_resolution == uint2(64, 64) &&
            main_composition.scene_color_position == float2(8.f, 16.f) &&
            main_composition.scene_color_resolution == float2(32.f, 16.f),
        test_name,
        "main UI was not retargeted from raw 80x80 into the committed 64x64 presentation domain"
    );

    auto& minimized_viewport = draw_frame.platform_viewports.front();
    WindowFrameSnapshot minimized_frame = detached_frame;
    minimized_frame.drawable_extent      = {0, 0};
    minimized_frame.last_drawable_extent = detached_frame.last_drawable_extent;
    minimized_frame.capture_sequence     = 2;
    minimized_frame.transition           = EWindowFrameTransition::Minimized;
    BindUiViewportWindowFrame(minimized_viewport, minimized_frame);
    BindUiViewportPresentation(minimized_viewport, committed_presentation);
    minimized_viewport.presentation_metadata_only = true;
    suite.Check(
        minimized_viewport.window_frame.GetStableDrawableExtent() ==
                detached_frame.last_drawable_extent &&
            IsUiViewportPresentationCommitted(minimized_viewport) &&
            !IsUiViewportPresentationCurrent(minimized_viewport),
        test_name,
        "a minimized snapshot did not retain its committed detached presentation"
    );

    UiCompositionFrameData minimized_composition{
        .enabled                = true,
        .separate_window        = true,
        .output_resolution      = {80, 80},
        .scene_color_position   = {3.f, 5.f},
        .scene_color_resolution = {10.f, 12.f},
        .window_frame_buffer    = framebuffer,
    };
    suite.Check(
        ResolveUiCompositionDrawableMetrics(
            minimized_composition,
            {},
            draw_frame
        ) &&
            !minimized_composition.enabled &&
            minimized_composition.scene_extent_resolved &&
            !minimized_composition.window_frame_buffer &&
            minimized_composition.output_resolution == uint2(64, 64) &&
            minimized_composition.scene_color_position == float2(6.f, 10.f) &&
            minimized_composition.scene_color_resolution == float2(20.f, 24.f),
        test_name,
        "minimization did not preserve scene extent as metadata-only composition state"
    );

    WindowFrameSnapshot invalid_frame = minimized_frame;
    invalid_frame.capture_sequence     = 3;
    invalid_frame.transition           = EWindowFrameTransition::Invalid;
    BindUiViewportWindowFrame(minimized_viewport, invalid_frame);
    BindUiViewportPresentation(minimized_viewport, committed_presentation);
    UiCompositionFrameData invalid_composition{
        .enabled             = true,
        .separate_window     = true,
        .window_frame_buffer = framebuffer,
    };
    suite.Check(
        !IsUiViewportPresentationCommitted(minimized_viewport) &&
            !IsUiViewportPresentationCurrent(minimized_viewport) &&
            !ResolveUiCompositionDrawableMetrics(
                invalid_composition,
                {},
                draw_frame
            ),
        test_name,
        "an invalid detached snapshot reused a committed presentation"
    );

    WindowFrameSnapshot replaced_generation_frame = detached_frame;
    replaced_generation_frame.capture_sequence     = 4;
    replaced_generation_frame.drawable_generation  = 2;
    replaced_generation_frame.transition           = EWindowFrameTransition::Replaced;
    BindUiViewportWindowFrame(minimized_viewport, replaced_generation_frame);
    BindUiViewportPresentation(minimized_viewport, committed_presentation);
    UiCompositionFrameData replaced_composition{
        .enabled             = true,
        .separate_window     = true,
        .window_frame_buffer = framebuffer,
    };
    suite.Check(
        !IsUiViewportPresentationCommitted(minimized_viewport) &&
            !IsUiViewportPresentationCurrent(minimized_viewport) &&
            !ResolveUiCompositionDrawableMetrics(
                replaced_composition,
                {},
                draw_frame
            ),
        test_name,
        "a replaced detached generation reused the prior committed presentation"
    );
}

void TestRasterStyleUiSlotClaimTracksNativeAcceptance(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name =
        "Raster copied UI slot native acceptance";

    auto rejected_resources =
        Moer::MakeShared<FakeUiViewportResources>(11);
    auto rejected_backend = Moer::MakeShared<FakeUiDrawBackend>();
    auto rejected_frame =
        MakeUiDrawPacket(rejected_resources, rejected_backend, true);
    UiDrawFrameSlotClaim rejected_claim(rejected_frame);
    CommandList         rejected_commands;
    suite.Check(
        rejected_claim.IsReadyForRecording() &&
            rejected_frame.main_viewport.recording_slot == 11 &&
            rejected_frame.platform_viewports.front().recording_slot == 11,
        test_name,
        "Raster-style copied frame did not freeze a shared pending slot"
    );
    RenderUiDrawFrame(
        rejected_commands,
        {},
        rejected_frame,
        EUiDrawExecutionThread::Game
    );
    suite.Check(
        rejected_backend->render_count == 1 &&
            rejected_backend->saw_valid_recording_slots,
        test_name,
        "non-empty Raster UI reached the backend without a valid slot claim"
    );
    rejected_claim.Reject();
    rejected_claim.Reject();
    suite.Check(
        rejected_claim.GetState() ==
                UiDrawFrameSlotClaim::EState::Rejected &&
            !rejected_claim.CommitAccepted() &&
            rejected_resources->PendingSlot() == 11 &&
            rejected_resources->CommitCount() == 0,
        test_name,
        "native rejection advanced or resurrected the copied-frame slot"
    );

    auto accepted_resources =
        Moer::MakeShared<FakeUiViewportResources>(23);
    auto accepted_backend = Moer::MakeShared<FakeUiDrawBackend>();
    auto accepted_frame =
        MakeUiDrawPacket(accepted_resources, accepted_backend, true);
    TextureRef accepted_main_output(
        MoerNew(FakeUiTexture)("RasterMainOutput")
    );
    TextureRef accepted_platform_output(
        MoerNew(FakeUiTexture)("RasterPlatformOutput")
    );
    accepted_frame.platform_viewports.front().framebuffer =
        accepted_platform_output;
    UiDrawFrameSlotClaim accepted_claim(accepted_frame);
    CommandList         accepted_commands;
    RenderUiDrawFrame(
        accepted_commands, accepted_main_output->GetView(), accepted_frame, EUiDrawExecutionThread::Game
    );
    suite.Check(
        accepted_backend->render_count == 1 &&
            accepted_backend->saw_valid_recording_slots &&
            accepted_resources->PendingSlot() == 23 &&
            accepted_resources->CommitCount() == 0,
        test_name,
        "Raster UI recording advanced the upload ring before native acceptance"
    );
    suite.Check(
        accepted_claim.CommitAccepted() &&
            accepted_claim.CommitAccepted(),
        test_name,
        "native acceptance was not idempotent"
    );
    accepted_claim.Reject();
    suite.Check(
        accepted_claim.GetState() ==
                UiDrawFrameSlotClaim::EState::Accepted &&
            accepted_resources->PendingSlot() == 24 &&
            accepted_resources->CommitCount() == 1,
        test_name,
        "shared Raster UI slot was not committed exactly once"
    );
}

void TestUiPreparedFrameLifecycleIsOneShot(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name =
        "UI prepared frame one-shot lifecycle";

    auto resources = Moer::MakeShared<FakeUiViewportResources>(17);
    auto backend   = Moer::MakeShared<FakeUiDrawBackend>();
    std::weak_ptr<FakeUiViewportResources> resource_owner = resources;
    std::weak_ptr<FakeUiDrawBackend>        backend_owner  = backend;
    auto accepted = UiFrameGraphPass::Prepare(
        {},
        MakeUiDrawPacket(resources, backend, true),
        EUiDrawExecutionThread::Render
    );
    resources.reset();
    backend.reset();

    suite.Check(
        !resource_owner.expired() && !backend_owner.expired(),
        test_name,
        "prepared frame did not retain copied UI backend/resources"
    );
    suite.Check(
        accepted->GetDrawFrame().main_viewport.recording_slot == 17 &&
            accepted->GetDrawFrame().platform_viewports.front().recording_slot ==
                17,
        test_name,
        "Prepare did not freeze the pending upload-ring slot"
    );
    auto accepted_resources = resource_owner.lock();
    suite.Check(
        accepted_resources && accepted_resources->PendingSlot() == 17 &&
            accepted_resources->CommitCount() == 0,
        test_name,
        "Prepare advanced the upload-ring before native acceptance"
    );
    suite.Check(
        UiFrameGraphPassTestAccess::BeginRecording(*accepted),
        test_name,
        "first recording claim failed"
    );
    suite.Check(
        !UiFrameGraphPassTestAccess::BeginRecording(*accepted),
        test_name,
        "a second recording claim replayed the copied packet"
    );
    UiFrameGraphPassTestAccess::FinishRecording(*accepted);
    suite.Check(
        accepted->GetRecordingState() ==
                UiFrameGraphPass::ERecordingState::Recorded &&
            !accepted->CanPresent() &&
            accepted_resources->CommitCount() == 0,
        test_name,
        "recording completion prematurely committed or presented the UI frame"
    );
    suite.Check(
        accepted->CommitAcceptedSource() &&
            accepted->GetRecordingState() ==
                UiFrameGraphPass::ERecordingState::SourceAccepted &&
            !accepted->CanPresent() &&
            accepted_resources->PendingSlot() == 18 &&
            accepted_resources->CommitCount() == 1,
        test_name,
        "native UI-source acceptance did not retire exactly one shared slot"
    );
    suite.Check(
        accepted->CommitAcceptedSource() &&
            accepted_resources->CommitCount() == 1,
        test_name,
        "repeated source acceptance advanced the upload ring twice"
    );
    accepted->RejectSource();
    accepted->Abandon();
    suite.Check(
        accepted->GetRecordingState() ==
            UiFrameGraphPass::ERecordingState::SourceAccepted,
        test_name,
        "a late reject/abandon reversed an accepted source"
    );
    suite.Check(
        accepted->CommitAcceptedFrame() && accepted->CanPresent() &&
            accepted->CommitAcceptedFrame(),
        test_name,
        "whole-frame acceptance was not idempotent"
    );
    accepted->RejectSource();
    accepted->Abandon();
    suite.Check(
        accepted->GetRecordingState() ==
                UiFrameGraphPass::ERecordingState::FrameAccepted &&
            accepted->CanPresent() &&
            accepted_resources->CommitCount() == 1,
        test_name,
        "an accepted frame was downgraded or recommitted"
    );

    auto failed_resources = Moer::MakeShared<FakeUiViewportResources>(41);
    auto failed_backend   = Moer::MakeShared<FakeUiDrawBackend>();
    auto failed = UiFrameGraphPass::Prepare(
        {},
        MakeUiDrawPacket(failed_resources, failed_backend, false),
        EUiDrawExecutionThread::Render
    );
    suite.Check(
        UiFrameGraphPassTestAccess::BeginRecording(*failed),
        test_name,
        "failure-path recording claim failed"
    );
    failed->Abandon();
    UiFrameGraphPassTestAccess::FinishRecording(*failed);
    suite.Check(
        failed->GetRecordingState() ==
                UiFrameGraphPass::ERecordingState::Failed &&
            !failed->CanPresent() &&
            failed_resources->PendingSlot() == 41 &&
            failed_resources->CommitCount() == 0 &&
            !failed->CommitAcceptedSource() &&
            !UiFrameGraphPassTestAccess::BeginRecording(*failed),
        test_name,
        "failed recording remained replayable or retired an unaccepted slot"
    );

    auto stale_resources = Moer::MakeShared<FakeUiViewportResources>(73);
    auto stale_backend   = Moer::MakeShared<FakeUiDrawBackend>();
    auto stale = UiFrameGraphPass::Prepare(
        {},
        MakeUiDrawPacket(stale_resources, stale_backend, false),
        EUiDrawExecutionThread::Render
    );
    suite.Check(
        UiFrameGraphPassTestAccess::BeginRecording(*stale),
        test_name,
        "stale-slot recording claim failed"
    );
    UiFrameGraphPassTestAccess::FinishRecording(*stale);
    stale_resources->SetPendingSlot(74);
    suite.Check(
        !stale->CommitAcceptedSource() &&
            stale->GetRecordingState() ==
                UiFrameGraphPass::ERecordingState::Failed &&
            stale_resources->CommitCount() == 0 &&
            !stale->CommitAcceptedFrame(),
        test_name,
        "a stale upload-ring token was committed or made presentable"
    );
}

void TestUiFrameGraphTailContract(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name =
        "UI frame graph presentation tail";

    TextureRef scene_color(MoerNew(FakeUiTexture)("SceneColor"));
    TextureRef main_output(MoerNew(FakeUiTexture)("MainOutput"));
    TextureRef window_output(MoerNew(FakeUiTexture)("SceneWindow"));
    TextureRef platform_output(MoerNew(FakeUiTexture)("PlatformOutput"));
    TextureRef metadata_output(MoerNew(FakeUiTexture)("MetadataOnlyOutput"));

    auto shared_slots = Moer::MakeShared<FakeUiViewportResources>(5);
    auto platform_slots = Moer::MakeShared<FakeUiViewportResources>(9);
    auto backend = Moer::MakeShared<FakeUiDrawBackend>();
    UiDrawFramePacket draw_frame{};
    draw_frame.backend = backend;
    draw_frame.main_viewport.render_resources = shared_slots;
    draw_frame.main_viewport.commands.emplace_back();

    const auto append_platform = [&](
                                     TextureRef framebuffer,
                                     Moer::SharedPtr<FakeUiViewportResources> slots,
                                     bool metadata_only = false
                                 ) {
        UiViewportDrawPacket viewport{};
        viewport.framebuffer                 = std::move(framebuffer);
        viewport.render_resources            = std::move(slots);
        viewport.presentation_metadata_only  = metadata_only;
        viewport.commands.emplace_back();
        draw_frame.platform_viewports.emplace_back(std::move(viewport));
    };
    append_platform(main_output, shared_slots);
    append_platform(window_output, shared_slots);
    append_platform(platform_output, platform_slots);
    append_platform(metadata_output, platform_slots, true);

    UiCompositionFrameData composition{
        .enabled                 = true,
        .separate_window         = true,
        .output_resolution       = Moer::uint2(64, 64),
        .scene_color_position    = Moer::float2(0.f, 0.f),
        .scene_color_resolution  = Moer::float2(64.f, 64.f),
        .window_frame_buffer     = window_output,
    };
    auto prepared = UiFrameGraphPass::Prepare(
        std::move(composition),
        std::move(draw_frame),
        EUiDrawExecutionThread::Render
    );

    RenderGraph graph("UiFrameGraphTailContract");
    const auto scene = graph.ImportTexture(
        "SceneColor",
        scene_color,
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    graph.SetInitialState(
        scene,
        RenderGraph::TextureBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    // Active lowering requires the final boundary of every imported resource.
    graph.Export(
        scene,
        RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto presentation_ready =
        graph.CreateTransientToken("PresentationReady");
    const auto producer = graph.AddRecordPass(
        "ShowTexture",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::GpuWorkType::Graphics
                )
                .Write(scene, RenderGraph::TextureAccessType::ColorAttachment)
                .Write(presentation_ready)
                .SideEffect();
        },
        [](CommandList&) {},
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );

    UiFrameGraphPass::GraphPasses ui_passes{};
    const bool added = UiFrameGraphPassTestAccess::AddPassesWithComposeRecorder(
        graph,
        [](CommandList&) {},
        prepared,
        scene,
        scene_color,
        main_output,
        presentation_ready,
        ui_passes
    );
    suite.Check(added && ui_passes.IsValid(), test_name, "UI tail was not declared");
    suite.Check(
        ui_passes.presentation_targets.size() == 3 &&
            ui_passes.main_output == ui_passes.presentation_targets.front(),
        test_name,
        "physical presentation aliases were not deduplicated"
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    if (!graph.IsCompiled()) {
        return;
    }

    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.state_plan_complete,
        test_name,
        "UI tail left an incomplete explicit resource-state plan"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            producer,
            ui_passes.clear,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            presentation_ready.Untyped()
        ) &&
            HasEdgeReason(
                plan,
                ui_passes.clear,
                ui_passes.compose,
                RenderGraph::EdgeReasonKind::Explicit
            ) &&
            HasEdgeReason(
                plan,
                ui_passes.compose,
                ui_passes.draw,
                RenderGraph::EdgeReasonKind::Explicit
            ),
        test_name,
        "ShowTexture -> Clear -> Compose -> Draw ordering was not explicit"
    );

    const auto& clear_unit =
        plan.frontend_record_units[ui_passes.clear.pass_id];
    const auto& compose_unit =
        plan.frontend_record_units[ui_passes.compose.pass_id];
    const auto& draw_unit =
        plan.frontend_record_units[ui_passes.draw.pass_id];
    suite.Check(
        clear_unit.record_policy ==
                RenderGraph::PassRecordPolicy::Serial &&
            compose_unit.record_policy ==
                RenderGraph::PassRecordPolicy::Serial &&
            draw_unit.record_policy ==
                RenderGraph::PassRecordPolicy::Serial &&
            clear_unit.native_translate_class ==
                ERHITranslateExecutionClass::Parallel &&
            compose_unit.native_translate_class ==
                ERHITranslateExecutionClass::Parallel &&
            draw_unit.native_translate_class ==
                ERHITranslateExecutionClass::SerialControl,
        test_name,
        "UI recording/translation ownership classes changed"
    );

    bool access_contract_valid = true;
    bool export_contract_valid = true;
    for (const auto target : ui_passes.presentation_targets) {
        const auto* clear_access =
            FindAccess(plan, ui_passes.clear, target.Untyped());
        const auto* draw_access =
            FindAccess(plan, ui_passes.draw, target.Untyped());
        const auto* export_barrier =
            FindBarrier(plan, target.Untyped(), ui_passes.draw, {});
        access_contract_valid =
            access_contract_valid && clear_access && draw_access &&
            clear_access->mode == RenderGraph::AccessMode::Write &&
            clear_access->state ==
                RenderGraph::ResourceState{RenderGraph::TextureAccessType::TransferDestination} &&
            clear_access->domain.queue == RenderGraph::QueueRole::Graphics &&
            clear_access->domain.work ==
                RenderGraph::GpuWorkType::Transfer &&
            draw_access->mode == RenderGraph::AccessMode::ReadWrite &&
            draw_access->state ==
                RenderGraph::ResourceState{RenderGraph::TextureAccessType::ColorAttachment} &&
            draw_access->domain.queue == RenderGraph::QueueRole::Graphics &&
            draw_access->domain.work ==
                RenderGraph::GpuWorkType::Graphics;
        export_contract_valid =
            export_contract_valid && export_barrier &&
            export_barrier->export_boundary &&
            export_barrier->after_state ==
                RenderGraph::ResourceState{RenderGraph::TextureBoundary::PresentationSource} &&
            export_barrier->after_access == RenderGraph::AccessMode::Read;
    }
    const auto* compose_access = FindAccess(
        plan,
        ui_passes.compose,
        ui_passes.presentation_targets[1].Untyped()
    );
    suite.Check(
        access_contract_valid && compose_access &&
            compose_access->mode == RenderGraph::AccessMode::Write &&
            compose_access->state ==
                RenderGraph::ResourceState{RenderGraph::TextureAccessType::ColorAttachment} &&
            compose_access->domain.queue ==
                RenderGraph::QueueRole::Graphics &&
            compose_access->domain.work ==
                RenderGraph::GpuWorkType::Graphics,
        test_name,
        "UI Clear/Compose/Draw access declarations changed"
    );
    suite.Check(
        export_contract_valid,
        test_name,
        "a presentation target was not exported as Graphics PresentationSource/Read"
    );
    const std::string dump = graph.Dump();
    suite.Check(
        Contains(dump, "UI.ClearTargets") &&
            Contains(dump, "UI.Compose") &&
            Contains(dump, "UI.DrawCopiedFrame") &&
            Contains(dump, "translate=serial-control") &&
            !Contains(dump, "texture:present") &&
            !Contains(dump, "ui_framebuffer") &&
            !Contains(dump, "gui_color"),
        test_name,
        "UI tail dump lost nodes/frontier or regained legacy fake dependencies"
    );

    // Compile's temporary compiler is already destroyed. Both lowering and
    // recompilation must consume graph-owned output, including nested frontiers.
    RenderGraphLowering::LoweredPlan lowered;
    std::string                     lowering_error;
    const bool lowering_succeeded = RenderGraphLowering::Lower(graph, lowered, lowering_error);
    suite.Check(lowering_succeeded, test_name, lowering_error);
    const std::string lowered_dump = lowered.Dump();
    const bool recompiled = graph.Compile();
    suite.Check(recompiled, test_name, graph.GetCompileError());
    RenderGraphLowering::LoweredPlan rebuilt_lowered;
    const bool rebuilt_lowering_succeeded =
        RenderGraphLowering::Lower(graph, rebuilt_lowered, lowering_error);
    suite.Check(rebuilt_lowering_succeeded, test_name, lowering_error);
    if (lowering_succeeded && rebuilt_lowering_succeeded) {
        suite.Check(
            graph.Dump() == dump && lowered.Dump() == lowered_dump &&
                rebuilt_lowered.Dump() == lowered_dump &&
                rebuilt_lowered.passes.size() == plan.execution_order.size(),
            test_name,
            "compiler teardown and recompilation must preserve independently owned compiled and lowered plans"
        );
    }

    bool draw_source_kept_serial_control = false;
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        [&](std::span<const RenderGraph::ExecutedPassInfo> passes,
            RHIRecordingSource& source) {
            if (std::ranges::any_of(passes, [&](const auto& pass) { return pass.handle == ui_passes.draw; })) {
                draw_source_kept_serial_control =
                    source.command_list->GetTranslateExecutionClass() ==
                        ERHITranslateExecutionClass::SerialControl &&
                    source.submit_metadata.translate_execution_class ==
                        ERHITranslateExecutionClass::SerialControl;
            }
        },
        false,
        [](Moer::Array<RHIRecordingSource>&&) {}
    );
    suite.Check(
        executed && draw_source_kept_serial_control &&
            backend->render_count == 1 &&
            backend->last_execution_thread ==
                EUiDrawExecutionThread::Render &&
            prepared->GetRecordingState() ==
                UiFrameGraphPass::ERecordingState::Recorded &&
            shared_slots->CommitCount() == 0 &&
            platform_slots->CommitCount() == 0,
        test_name,
        "UI tail did not record once with immutable ownership"
    );
}

void TestUiFrameGraphRejectsInvalidPhysicalContracts(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name =
        "UI frame graph physical contract rejection";

    TextureRef scene_color(MoerNew(FakeUiTexture)("SceneColor"));
    TextureRef other_scene(MoerNew(FakeUiTexture)("OtherScene"));
    TextureRef invalid_output(MoerNew(FakeUiTexture)(
        "InvalidOutput",
        ETextureUsageFlags::SAMPLED |
            ETextureUsageFlags::COLOR_ATTACHMENT
    ));
    TextureRef missing_presentation_usage(MoerNew(FakeUiTexture)(
        "MissingPresentationUsage",
        ETextureUsageFlags::SAMPLED |
            ETextureUsageFlags::COLOR_ATTACHMENT |
            ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST
    ));

    auto slots   = Moer::MakeShared<FakeUiViewportResources>(23);
    auto backend = Moer::MakeShared<FakeUiDrawBackend>();
    auto prepared = UiFrameGraphPass::Prepare(
        {},
        MakeUiDrawPacket(slots, backend, false),
        EUiDrawExecutionThread::Render
    );

    RenderGraph graph("UiFrameGraphInvalidPhysicalContracts");
    const auto mismatched_scene_handle = graph.ImportTexture(
        "OtherScene",
        other_scene,
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    const auto presentation_ready =
        graph.CreateTransientToken("PresentationReady");
    UiFrameGraphPass::GraphPasses ui_passes{};
    suite.Check(
        !UiFrameGraphPassTestAccess::AddPassesWithComposeRecorder(
            graph,
            [](CommandList&) {},
            prepared,
            mismatched_scene_handle,
            scene_color,
            invalid_output,
            presentation_ready,
            ui_passes
        ) &&
            !ui_passes.IsValid() &&
            prepared->GetRecordingState() ==
                UiFrameGraphPass::ERecordingState::Prepared,
        test_name,
        "graph declaration accepted mismatched scene identity or invalid "
        "presentation usage"
    );

    RenderGraph semantic_graph("UiFrameGraphMissingPresentationUsage");
    const auto  scene_handle = semantic_graph.ImportTexture(
        "SceneColor",
        scene_color,
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    const auto semantic_ready =
        semantic_graph.CreateTransientToken("PresentationReady");
    UiFrameGraphPass::GraphPasses semantic_passes{};
    suite.Check(
        !UiFrameGraphPassTestAccess::AddPassesWithComposeRecorder(
            semantic_graph,
            [](CommandList&) {},
            prepared,
            scene_handle,
            scene_color,
            missing_presentation_usage,
            semantic_ready,
            semantic_passes
        ) &&
            !semantic_passes.IsValid() &&
            prepared->GetRecordingState() ==
                UiFrameGraphPass::ERecordingState::Prepared,
        test_name,
        "graph declaration accepted a target without PRESENTATION_SOURCE "
        "usage"
    );

}

void TestSerialControlTranslationIsADeclaredRecordingPolicy(TestSuite& suite) {
    constexpr std::string_view test_name =
        "serial-control translation recording policy";
    RenderGraph graph("SerialControlTranslation");
    bool        recorded  = false;
    bool        published = false;

    graph.AddRecordPass(
        "NRDIsland",
        [](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::GpuWorkType::Compute
                )
                .SideEffect()

                .TranslateSerialControl();
        },
        [&](Moer::Render::CommandList&) { recorded = true; },
        RenderGraph::PassRecordPolicy::Serial
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.frontend_record_units.size() == 1 &&
            plan.frontend_record_units.front().record_policy ==
                RenderGraph::PassRecordPolicy::Serial &&
            plan.frontend_record_units.front().native_translate_class ==
                Moer::Render::ERHITranslateExecutionClass::SerialControl &&
            Contains(graph.Dump(), "translate=serial-control"),
        test_name,
        "the compiler and dump must retain the declared translation frontier"
    );

    const bool executed = graph.ExecuteFrontendRecordingPlan(
        {},
        false,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
            published = sources.size() == 1 &&
                        sources.front()
                                .submit_metadata.translate_execution_class ==
                            Moer::Render::ERHITranslateExecutionClass::
                                SerialControl;
        }
    );
    suite.Check(
        executed && recorded && published,
        test_name,
        "the graph handoff must publish SerialControl metadata without "
        "mutating the producer CommandList"
    );

    RenderGraph weakened("SerialControlCannotBeWeakened");
    weakened.AddRecordPass(
        "NRDIsland",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect().TranslateSerialControl();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(weakened.Compile(), test_name, weakened.GetCompileError());
    bool weakened_published = false;
    const bool weakened_executed = weakened.ExecuteFrontendRecordingPlan(
        [](std::span<const RenderGraph::ExecutedPassInfo>,
           Moer::Render::RHIRecordingSource& source) {
            source.submit_metadata.translate_execution_class.reset();
        },
        false,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&&) {
            weakened_published = true;
        }
    );
    suite.Check(
        !weakened_executed && !weakened_published &&
            Contains(
                weakened.GetCompileError(),
                "weakened the declared SerialControl"
            ),
        test_name,
        "source configuration must not weaken a graph-declared translation "
        "frontier"
    );

    RenderGraph publisher_attempt("SerialControlPublisherFloor");
    publisher_attempt.AddRecordPass(
        "NRDIsland",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect().TranslateSerialControl();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        publisher_attempt.Compile(),
        test_name,
        publisher_attempt.GetCompileError()
    );
    bool command_list_kept_floor = false;
    const bool publisher_attempt_executed =
        publisher_attempt.ExecuteFrontendRecordingPlan(
            {},
            false,
            [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
                sources.front()
                    .submit_metadata.translate_execution_class.reset();
                command_list_kept_floor =
                    sources.front()
                        .command_list->GetTranslateExecutionClass() ==
                    Moer::Render::ERHITranslateExecutionClass::SerialControl;
            }
        );
    suite.Check(
        publisher_attempt_executed && command_list_kept_floor,
        test_name,
        "the CommandList must retain the graph-declared SerialControl floor "
        "even if a custom publisher drops optional metadata"
    );
}

void TestSharedRecordingRespectsSourceBoundaries(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name = "shared streams respect queue and SerialControl boundaries";
    RenderGraph graph("SharedStreamBoundaries", RenderGraph::QueueTopology::DedicatedQueues());
    std::array<CommandList*, 7> streams{};
    for (size_t i = 0; i < streams.size(); ++i) {
        graph.AddRecordPass(std::string("Pass") + std::to_string(i),
            [i](RenderGraph::PassBuilder& pass) {
                pass.SideEffect();
                if (i == 2) { pass.TranslateSerialControl(); }
                if (i >= 5) {
                    pass.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::GpuWorkType::Compute);
                }
            },
            [&, i](CommandList& commands) { streams[i] = &commands; },
            RenderGraph::PassRecordPolicy::Serial);
    }
    Moer::Array<RHIRecordingSource> published;
    const bool recorded = graph.Compile() && graph.ExecuteFrontendRecordingPlan({}, false,
        [&](Moer::Array<RHIRecordingSource>&& sources) {
            for (auto& source : sources) { published.emplace_back(std::move(source)); }
        });
    suite.Check(recorded, test_name, graph.GetCompileError());
    suite.Check(published.size() == 4 && streams[0] && streams[0] == streams[1] &&
        streams[2] != streams[1] && streams[2] != streams[3] && streams[3] == streams[4] &&
        streams[4] != streams[5] && streams[5] == streams[6], test_name,
        "queue changes and both sides of SerialControl must split shared streams");
}

void TestConfiguredRecordingSourceReceivesAllPasses(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name = "configured shared source receives all pass identities";
    RenderGraph graph("ConfiguredSources");
    std::array<CommandList*, 3> streams{};
    for (size_t i = 0; i < streams.size(); ++i) {
        graph.AddRecordPass(std::string("Pass") + std::to_string(i),
            [](RenderGraph::PassBuilder& pass) { pass.SideEffect(); },
            [&, i](CommandList& commands) { streams[i] = &commands; },
            RenderGraph::PassRecordPolicy::Serial);
    }
    std::vector<std::string> configured_names, published_labels;
    int configure_calls = 0;
    const bool recorded = graph.Compile() && graph.ExecuteFrontendRecordingPlan(
        [&](std::span<const RenderGraph::ExecutedPassInfo> passes, RHIRecordingSource& source) {
            ++configure_calls;
            for (const auto& pass : passes) { configured_names.emplace_back(pass.name); }
            const bool contains_target = std::ranges::any_of(passes,
                [](const auto& pass) { return pass.name == "Pass1"; });
            source.submit_metadata.debug_label = contains_target ? "ContainsTarget" : "MissingTarget";
        }, false, [&](Moer::Array<RHIRecordingSource>&& sources) {
            for (const auto& source : sources) {
                published_labels.push_back(source.submit_metadata.debug_label.value_or("<missing>"));
            }
        });
    suite.Check(recorded, test_name, graph.GetCompileError());
    suite.Check(configured_names == std::vector<std::string>{"Pass0", "Pass1", "Pass2"} &&
        configure_calls == 1 && published_labels == std::vector<std::string>{"ContainsTarget"} &&
        streams[0] && streams[0] == streams[1] && streams[1] == streams[2], test_name,
        "one metadata callback must see every pass in its shared source, including an interior target");
}

void TestWeightedParallelRecordingSharesStreams(TestSuite& suite, bool merge) {
    using namespace std::chrono_literals;
    using namespace Moer::Render;
    const std::string test_name = std::string("weighted parallel recording: ") + (merge ? "merged" : "active");
    RenderGraph graph(test_name);
    constexpr std::array<uint32_t, 5> workloads{1, 1, 6, 1, 7};
    std::array<CommandList*, 5> streams{};
    std::mutex mutex;
    std::condition_variable cv;
    bool second_group_finished = false, timed_out = false;
    std::vector<int> completed_groups, callback_order;
    for (size_t i = 0; i < workloads.size(); ++i) {
        graph.AddRecordPass(std::string("Pass") + std::to_string(i),
            [workload = workloads[i]](RenderGraph::PassBuilder& pass) {
                pass.SideEffect().RecordWorkload(workload);
            },
            [&, i](CommandList& commands) {
                streams[i] = &commands;
                if (i == 0) {
                    std::unique_lock lock(mutex);
                    timed_out = !cv.wait_for(lock, 2s, [&] { return second_group_finished; });
                }
                commands.AddCallback([&, i] { callback_order.push_back(static_cast<int>(i)); });
                if (i == 2 || i == 4) {
                    std::lock_guard lock(mutex);
                    completed_groups.push_back(static_cast<int>(i));
                    if (i == 4) { second_group_finished = true; cv.notify_all(); }
                }
            }, RenderGraph::PassRecordPolicy::ParallelAllowed);
    }
    CommandList destination;
    Moer::Array<RHIRecordingSource> published;
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    Moer::TaskSystem::Init();
    const bool recorded = merge ? graph.RecordAndMergeFrontendCommands(destination, true) :
        graph.ExecuteFrontendRecordingPlan({}, true, [&](Moer::Array<RHIRecordingSource>&& sources) {
            for (auto& source : sources) { published.emplace_back(std::move(source)); }
        });
    Moer::TaskSystem::ShutDown();
    suite.Check(recorded, test_name, graph.GetCompileError());
    suite.Check(streams[0] && streams[0] == streams[1] && streams[1] == streams[2] &&
        streams[2] != streams[3] && streams[3] == streams[4], test_name,
        "workloads 1+1+6 and 1+7 must form two exclusive recording streams");
    suite.Check(!timed_out && completed_groups == std::vector<int>{4, 2}, test_name,
        "the two weighted jobs must overlap and complete in reverse order");
    if (!recorded) { return; }
    Moer::Array<CmdSubmit> submits;
    if (merge) { submits.emplace_back(destination.Submit()); }
    else {
        suite.Check(published.size() == 2, test_name, "one source per weighted stream must be published");
        for (auto& source : published) { submits.emplace_back(source.command_list->Submit()); }
    }
    for (auto& submit : submits) {
        for (auto& callback : submit.callbacks) { callback(); }
        submit.callbacks.clear();
    }
    suite.Check(callback_order == std::vector<int>{0, 1, 2, 3, 4}, test_name,
        "stream completion order must not change compiled command/callback order");
}

void TestSharedStreamGpuProfilingKeepsPassScopes(TestSuite& suite, bool merge) {
    using namespace Moer::Render;
    const std::string test_name = std::string("shared stream profiling: ") + (merge ? "merged" : "active");
    GpuScopeStream scope_stream(RenderGraphGpuScopeConfig());
    auto frame = scope_stream.BeginFrame(9110 + static_cast<uint64_t>(merge));
    RenderGraph graph(test_name);
    for (const auto name : {"First", "Second"}) {
        graph.AddRecordPass(name, [](RenderGraph::PassBuilder& pass) { pass.SideEffect(); },
            [](CommandList&) {}, RenderGraph::PassRecordPolicy::Serial);
    }
    int bind_calls = 0;
    const RenderGraph::GpuProfilingOptions profiling{
        .try_bind_source = [&](const RenderGraph::ExecutedPassInfo& pass, CommandList& commands,
                               RHIQueueBinding queue, uint64_t order) {
            ++bind_calls;
            suite.Check(pass.name == "First" && order == 23 && commands.IsEmpty(), test_name,
                        "binding must use the stream's first compiled pass before recording");
            auto recorder = frame.CreateRecorder(queue, order);
            if (!recorder.Valid()) { return false; }
            commands.SetGpuScopeRecorder(std::move(recorder));
            return true;
        }, .source_order_base = 23};
    CommandList destination;
    Moer::Array<RHIRecordingSource> published;
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const bool recorded = merge ? graph.RecordAndMergeFrontendCommands(destination, false, profiling) :
        graph.ExecuteFrontendRecordingPlan({}, false, [&](Moer::Array<RHIRecordingSource>&& sources) {
            for (auto& source : sources) { published.emplace_back(std::move(source)); }
        }, {}, profiling);
    suite.Check(recorded && bind_calls == 1, test_name, graph.GetCompileError());
    if (!recorded || (!merge && published.size() != 1)) {
        suite.Check(false, test_name, "two serial passes must produce one profiled source");
        return;
    }
    auto submit = merge ? destination.Submit() : published.front().command_list->Submit();
    suite.Check(submit.query_tokens.size() == 2, test_name, "one timestamp scope per pass must remain");
    suite.Check(scope_stream.SealFrame(frame), test_name, "profiling frame failed to seal");
    for (size_t i = 0; i < submit.query_tokens.size(); ++i) {
        suite.Check(QueryBackendAccess::ResolveTimestamp(submit.query_tokens[i],
            RenderGraphTimestamp(100 + i * 20, 110 + i * 20)), test_name, "timestamp resolution failed");
    }
    ResolvedGpuScopeFrame resolved;
    const bool popped = scope_stream.TryPopFrame(resolved);
    suite.Check(popped && resolved.valid && resolved.queue_roots[0].size() == 2 &&
        resolved.queue_roots[0][0].name == "First" && resolved.queue_roots[0][1].name == "Second" &&
        resolved.queue_roots[0][0].source_order == 23 && resolved.queue_roots[0][1].source_order == 23,
        test_name, "sharing a recorder must preserve both pass roots in compiled order");
    ReleaseRenderGraphSubmit(submit);
}

void TestParallelRecordingFallsBackWithoutTaskGraph(TestSuite& suite) {
    constexpr std::string_view test_name = "parallel recording without task graph";
    suite.Check(
        !TaskGraph::IsInitialized(),
        test_name,
        "the isolated fallback test must start without the engine TaskGraph lifecycle"
    );

    RenderGraph graph("NoTaskGraphFallback");
    const auto  first_token  = graph.CreateTransientToken("First");
    const auto  second_token = graph.CreateTransientToken("Second");
    std::vector<int> events{};
    const auto caller_thread = std::this_thread::get_id();
    bool       stayed_on_caller = true;

    auto add_record = [&](std::string_view name, RenderGraph::TokenHandle token, int id) {
        graph.AddRecordPass(
            name,
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(token).SideEffect();
            },
            [&, id](Moer::Render::CommandList&) {
                stayed_on_caller = stayed_on_caller &&
                                   std::this_thread::get_id() == caller_thread;
                events.emplace_back(id);
            },
            RenderGraph::PassRecordPolicy::ParallelAllowed
        );
    };
    add_record("First", first_token, 1);
    add_record("Second", second_token, 2);

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    Moer::Array<Moer::Render::RHIRecordingGateView> published_gates{};
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        {},
        true,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
            for (auto& source : sources) {
                published_gates.emplace_back(source.completion);
            }
        }
    );

    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        stayed_on_caller && events == std::vector<int>{1, 2},
        test_name,
        "parallel-eligible work must use the stable serial path when TaskGraph is unavailable"
    );
    suite.Check(
        published_gates.size() == 1 &&
            std::all_of(published_gates.begin(), published_gates.end(), [](const auto& gate) {
                return gate.Status() == Moer::Render::ERHIRecordingStatus::Succeeded;
            }),
        test_name,
        "serial fallback must still complete every already-published ownership gate"
    );
}

void TestParallelRecordingDispatchAndJoin(TestSuite& suite) {
    using namespace std::chrono_literals;
    constexpr std::string_view test_name = "parallel recording dispatch and join";

    struct Rendezvous {
        std::atomic<int> inflight{0};
        std::atomic<int> max_inflight{0};
        std::atomic<bool> named_task_ran{false};
        std::mutex              mutex{};
        std::condition_variable cv{};
        int                     entered{0};
        bool                    timed_out{false};
        std::vector<int>        completion_order{};
        GraphEventRef           deferred_named_task{};
    } rendezvous;
    Moer::Render::GpuScopeStream gpu_scope_stream(
        RenderGraphGpuScopeConfig()
    );
    auto gpu_scope_frame = gpu_scope_stream.BeginFrame(9101);
    std::vector<uint64_t>    profiling_source_orders{};
    std::vector<std::string> profiling_pass_names{};

    const auto make_record = [&](int id) {
        return [&, id](Moer::Render::CommandList&) {
            const int inflight = rendezvous.inflight.fetch_add(1) + 1;
            int       observed = rendezvous.max_inflight.load();
            while (observed < inflight &&
                   !rendezvous.max_inflight.compare_exchange_weak(observed, inflight)) {}

            if (id == 1) {
                auto deferred_named_task = LambdaTask::Dispatch(
                    [&] { rendezvous.named_task_ran.store(true); }, EThread::EMainThread
                );
                std::lock_guard lock(rendezvous.mutex);
                rendezvous.deferred_named_task = std::move(deferred_named_task);
            }

            {
                std::unique_lock lock(rendezvous.mutex);
                ++rendezvous.entered;
                rendezvous.cv.notify_all();
                if (!rendezvous.cv.wait_for(lock, 2s, [&] { return rendezvous.entered == 2; })) {
                    rendezvous.timed_out = true;
                }
            }
            if (id == 1) {
                std::this_thread::sleep_for(30ms);
            }
            {
                std::lock_guard lock(rendezvous.mutex);
                rendezvous.completion_order.push_back(id);
            }
            rendezvous.inflight.fetch_sub(1);
        };
    };

    RenderGraph graph("ParallelRecordingDispatch");
    const auto  gpu_dependency = graph.CreateTransientTexture(
        "GpuDependency",
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = 1,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    const auto  second_token = graph.CreateTransientToken("Second");
    const auto  first = graph.AddRecordPass(
        "FirstRecord",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(gpu_dependency).SideEffect().RecordWorkload(8);
        },
        make_record(1),
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    const auto second = graph.AddRecordPass(
        "SecondRecord",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(gpu_dependency).Write(second_token).SideEffect().RecordWorkload(8);
        },
        make_record(2),
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    bool joined_before_serial = false;
    bool serial_finished = false;
    bool joined_after_serial = false;
    const auto caller_thread = std::this_thread::get_id();
    graph.AddRecordPass(
        "JoinBoundary",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(gpu_dependency)
                .Read(second_token)
                .DependsOn(first)
                .DependsOn(second)
                .SideEffect();
        },
        [&](Moer::Render::CommandList&) {
            joined_before_serial = rendezvous.inflight.load() == 0 && rendezvous.entered == 2 &&
                                   std::this_thread::get_id() == caller_thread;
            serial_finished = true;
        },
        RenderGraph::PassRecordPolicy::Serial
    );

    graph.AddRecordPass(
        "AfterSerialRecord",
        [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); },
        [&](Moer::Render::CommandList&) { joined_after_serial = serial_finished; },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& recording_plan = graph.GetCompiledPlan().frontend_record_units;
    suite.Check(
        recording_plan.size() == 4 &&
            recording_plan[1].dependency_level_index > recording_plan[0].dependency_level_index,
        test_name,
        "the fixture must place the two record callbacks in different GPU dependency levels"
    );

    Moer::Array<Moer::Array<Moer::Render::RHIRecordingSource>> published{};
    bool published_pending = false;
    bool distinct_command_lists = false;
    Moer::TaskSystem::Init();
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        {},
        true,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
            published_pending |= sources.size() == 2 &&
                                sources[0].completion.Status() ==
                                    Moer::Render::ERHIRecordingStatus::Pending &&
                                sources[1].completion.Status() ==
                                    Moer::Render::ERHIRecordingStatus::Pending;
            distinct_command_lists |= sources.size() == 2 &&
                                     sources[0].command_list != sources[1].command_list;
            published.emplace_back(std::move(sources));
        },
        {},
        RenderGraph::GpuProfilingOptions{
            .try_bind_source =
                [&](const RenderGraph::ExecutedPassInfo& pass,
                    Moer::Render::CommandList&            command_list,
                    Moer::Render::RHIQueueBinding         queue_binding,
                    Moer::uint64                          source_order) {
                    if (pass.name != "FirstRecord" && pass.name != "SecondRecord") {
                        return false;
                    }
                    profiling_source_orders.emplace_back(source_order);
                    profiling_pass_names.emplace_back(pass.name);
                    auto recorder = gpu_scope_frame.CreateRecorder(
                        queue_binding, source_order
                    );
                    if (!recorder.Valid()) {
                        return false;
                    }
                    command_list.SetGpuScopeRecorder(std::move(recorder));
                    return true;
                },

            .source_order_base = 73,
        }
    );
    Moer::Array<Moer::Render::CmdSubmit> profiled_submits{};
    Moer::Render::ResolvedGpuScopeFrame  resolved_gpu_frame{};
    bool profiling_submit_contract = false;
    bool profiling_resolution_contract = false;
    if (published.size() == 3 && published.front().size() == 2) {
        profiled_submits.reserve(2);
        for (auto& source : published.front()) {
            profiled_submits.emplace_back(source.command_list->Submit());
        }
        profiling_submit_contract =
            profiled_submits.size() == 2 &&
            profiled_submits[0].query_tokens.size() == 1 &&
            profiled_submits[1].query_tokens.size() == 1 &&
            !published.front()[0].command_list->HasGpuScopeRecorder() &&
            !published.front()[1].command_list->HasGpuScopeRecorder() &&
            gpu_scope_stream.SealFrame(gpu_scope_frame);
        if (profiling_submit_contract) {
            const bool reverse_resolved =
                ResolveRenderGraphTimestamp(
                    profiled_submits[1], 200, 240
                ) &&
                !gpu_scope_stream.TryPopFrame(resolved_gpu_frame) &&
                ResolveRenderGraphTimestamp(
                    profiled_submits[0], 100, 130
                );
            profiling_resolution_contract =
                reverse_resolved &&
                gpu_scope_stream.TryPopFrame(resolved_gpu_frame);
        }
    }
    const bool avoided_named_thread_reentry = !rendezvous.named_task_ran.load();
    GraphEventRef deferred_named_task{};
    {
        std::lock_guard lock(rendezvous.mutex);
        deferred_named_task = rendezvous.deferred_named_task;
    }
    if (deferred_named_task) {
        TaskGraph::GetInterface().WaitUntilTaskComplete(
            deferred_named_task, EThread::EMainThread
        );
    }
    const bool explicitly_drained_named_task = rendezvous.named_task_ran.load();
    Moer::TaskSystem::ShutDown();

    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        published.size() == 3 && published.front().size() == 2 && published_pending,
        test_name,
        "one stable source group must be published before either producer completes"
    );
    suite.Check(
        distinct_command_lists,
        test_name,
        "parallel record callbacks must never share a CommandList"
    );
    suite.Check(
        rendezvous.max_inflight.load() >= 2 && !rendezvous.timed_out,
        test_name,
        "GPU-dependent callbacks must still overlap immutable CPU recording"
    );
    suite.Check(
        rendezvous.completion_order == std::vector<int>{2, 1},
        test_name,
        "the test must exercise completion order different from source order"
    );
    suite.Check(
        profiling_source_orders == std::vector<uint64_t>{73, 74} &&
            profiling_pass_names ==
                std::vector<std::string>{"FirstRecord", "SecondRecord"} &&
            profiling_submit_contract &&
            profiling_resolution_contract &&
            resolved_gpu_frame.valid &&
            resolved_gpu_frame.queue_roots[0].size() == 2 &&
            resolved_gpu_frame.queue_roots[0][0].name == "FirstRecord" &&
            resolved_gpu_frame.queue_roots[0][0].source_order == 73 &&
            resolved_gpu_frame.queue_roots[0][1].name == "SecondRecord" &&
            resolved_gpu_frame.queue_roots[0][1].source_order == 74,
        test_name,
        "GPU profiling roots must follow compiled source order despite reverse worker/query completion"
    );
    suite.Check(
        joined_before_serial && joined_after_serial,
        test_name,
        "serial recording must join preceding workers, run on the caller, and finish before later workers"
    );
    suite.Check(
        avoided_named_thread_reentry && explicitly_drained_named_task,
        test_name,
        "joining recording workers must not pump unrelated named-thread work"
    );
    suite.Check(
        published.size() == 3 &&
            published.front()[0].completion.Status() ==
                Moer::Render::ERHIRecordingStatus::Succeeded &&
            published.front()[1].completion.Status() ==
                Moer::Render::ERHIRecordingStatus::Succeeded,
        test_name,
        "every published source gate must reach a successful terminal state"
    );
    for (auto& submit : profiled_submits) {
        ReleaseRenderGraphSubmit(submit);
    }
}

void TestGpuProfilingBindingFailureContracts(TestSuite& suite) {
    using namespace Moer::Render;
    constexpr std::string_view test_name =
        "GPU profiling binding failure contracts";

    {
        RenderGraph graph("GpuProfileDrop");
        int record_calls = 0;
        int bind_calls = 0;
        int publish_calls = 0;
        Moer::Array<RHIRecordingSource> published{};
        graph.AddRecordPass(
            "Dropped",
            [](RenderGraph::PassBuilder& builder) {
                builder.SideEffect();
            },
            [&](CommandList& command_list) {
                ++record_calls;
                suite.Check(
                    !command_list.HasGpuScopeRecorder() &&
                        command_list
                            .IsLegacyGpuProfilingSuppressedForGeneration(),
                    test_name,
                    "a rejected profiling source did not suppress legacy "
                    "timing for its recording generation"
                );
                command_list.PushScopeWithTimeScope(
                    "RejectedInnerTimedScope"
                );
                command_list.PopScopeWithTimeScope();
            },
            RenderGraph::PassRecordPolicy::Serial
        );
        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            {},
            false,
            [&](Moer::Array<RHIRecordingSource>&& sources) {
                ++publish_calls;
                published = std::move(sources);
            },
            {},
            RenderGraph::GpuProfilingOptions{
                .try_bind_source =
                    [&](const RenderGraph::ExecutedPassInfo&,
                        CommandList&,
                        RHIQueueBinding,
                        Moer::uint64) {
                        ++bind_calls;
                        return false;
                    },
            }
        );
        bool no_fallback_query = false;
        if (published.size() == 1) {
            CmdSubmit submit = published.front().command_list->Submit();
            no_fallback_query = submit.query_tokens.empty();
            ReleaseRenderGraphSubmit(submit);
        }
        suite.Check(
            executed && record_calls == 1 && bind_calls == 1 &&
                publish_calls == 1 && no_fallback_query,
            test_name,
            "a false binder result failed the graph or created a legacy timestamp"
        );
    }

    {
        GpuScopeStream stream(RenderGraphGpuScopeConfig());
        auto           frame = stream.BeginFrame(9104);
        RenderGraph    graph("MetadataSetupCannotBindGpuRecorder");
        int            record_calls = 0;
        int            publish_calls = 0;
        graph.AddRecordPass(
            "Pass",
            [](RenderGraph::PassBuilder& builder) {
                builder.SideEffect();
            },
            [&](CommandList&) { ++record_calls; },
            RenderGraph::PassRecordPolicy::Serial
        );
        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            [&](std::span<const RenderGraph::ExecutedPassInfo>,
                RHIRecordingSource& source) {
                source.command_list->SetGpuScopeRecorder(
                    frame.CreateRecorder(
                        RHIQueueBinding{
                            .queue =
                                source.command_list->GetQueueType(),
                            .native_queue_id = 0,
                            .family_id = 0,
                            .available = true,
                        },
                        0
                    )
                );
            },
            false,
            [&](Moer::Array<RHIRecordingSource>&&) {
                ++publish_calls;
            }
        );
        ResolvedGpuScopeFrame resolved{};
        const bool drained =
            stream.SealFrame(frame) && stream.TryPopFrame(resolved);
        suite.Check(
            !executed && record_calls == 0 && publish_calls == 0 &&
                Contains(
                    graph.GetCompileError(),
                    "may only change submit metadata"
                ) &&
                drained && resolved.valid,
            test_name,
            "metadata-only source setup bypassed the independent GPU profiling seam"
        );
    }

    const auto expect_pre_publish_failure =
        [&](std::string_view graph_name,
            RenderGraph::GpuProfilingOptions::TryBindSource binder,
            std::string_view expected_error) {
            RenderGraph graph(graph_name);
            int record_calls = 0;
            int publish_calls = 0;
            graph.AddRecordPass(
                "Pass",
                [](RenderGraph::PassBuilder& builder) {
                    builder.SideEffect();
                },
                [&](CommandList&) { ++record_calls; },
                RenderGraph::PassRecordPolicy::Serial
            );
            suite.Check(graph.Compile(), test_name, graph.GetCompileError());
            const bool executed = graph.ExecuteFrontendRecordingPlan(
                {},
                false,
                [&](Moer::Array<RHIRecordingSource>&&) { ++publish_calls; },
                {},
                RenderGraph::GpuProfilingOptions{
                    .try_bind_source = std::move(binder),
                }
            );
            suite.Check(
                !executed && record_calls == 0 && publish_calls == 0 &&
                    Contains(graph.GetCompileError(), expected_error),
                test_name,
                graph.GetCompileError()
            );
        };

    expect_pre_publish_failure(
        "GpuProfileThrow",
        [](const RenderGraph::ExecutedPassInfo&,
           CommandList&,
           RHIQueueBinding,
           Moer::uint64) -> bool {
            throw std::runtime_error("synthetic bind failure");
        },
        "synthetic bind failure"
    );
    expect_pre_publish_failure(
        "GpuProfileMutation",
        [](const RenderGraph::ExecutedPassInfo&,
           CommandList& command_list,
           RHIQueueBinding,
           Moer::uint64) {
            command_list.SetResourceStateOwnership(
                ERHIResourceStateOwnership::Compiled
            );
            return false;
        },
        "illegally mutated"
    );
    expect_pre_publish_failure(
        "GpuProfileResultMismatch",
        [](const RenderGraph::ExecutedPassInfo&,
           CommandList&,
           RHIQueueBinding,
           Moer::uint64) { return true; },
        "illegally mutated"
    );

    for (const bool bind_source : {false, true}) {
        GpuScopeStream stream(RenderGraphGpuScopeConfig());
        auto frame = stream.BeginFrame(bind_source ? 9105 : 9106);
        RenderGraph graph(
            bind_source ?
                "GpuProfilePublisherClearsRecorder" :
                "GpuProfilePublisherClearsSuppression"
        );
        int record_calls = 0;
        int publish_calls = 0;
        graph.AddRecordPass(
            "Pass",
            [](RenderGraph::PassBuilder& builder) {
                builder.SideEffect();
            },
            [&](CommandList&) { ++record_calls; },
            RenderGraph::PassRecordPolicy::Serial
        );
        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            {},
            false,
            [&](Moer::Array<RHIRecordingSource>&& sources) {
                ++publish_calls;
                if (sources.size() == 1) {
                    const EQueueType queue =
                        sources.front().command_list->GetQueueType();
                    *sources.front().command_list = CommandList(queue);
                }
            },
            {},
            RenderGraph::GpuProfilingOptions{
                .try_bind_source =
                    [&](const RenderGraph::ExecutedPassInfo&,
                        CommandList& command_list,
                        RHIQueueBinding queue_binding,
                        Moer::uint64 source_order) {
                        if (!bind_source) {
                            return false;
                        }
                        auto recorder = frame.CreateRecorder(
                            queue_binding, source_order
                        );
                        if (!recorder.Valid()) {
                            return false;
                        }
                        command_list.SetGpuScopeRecorder(
                            std::move(recorder)
                        );
                        return true;
                    },
            }
        );
        ResolvedGpuScopeFrame resolved{};
        const bool drained =
            stream.SealFrame(frame) && stream.TryPopFrame(resolved);
        suite.Check(
            !executed && record_calls == 0 && publish_calls == 1 &&
                Contains(
                    graph.GetCompileError(),
                    "publisher mutated pending source state"
                ) &&
                drained && resolved.valid,
            test_name,
            "the publisher cleared an admitted recorder/suppression state "
            "without failing before dispatch"
        );
    }

    {
        RenderGraph graph("GpuProfileOrderOverflow");
        int bind_calls = 0;
        int record_calls = 0;
        int publish_calls = 0;
        for (std::string_view name : {"First", "Second"}) {
            graph.AddRecordPass(
                name,
                [](RenderGraph::PassBuilder& builder) {
                    builder.SideEffect();
                },
                [&](CommandList&) { ++record_calls; },
                RenderGraph::PassRecordPolicy::Serial
            );
        }
        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            {},
            false,
            [&](Moer::Array<RHIRecordingSource>&&) { ++publish_calls; },
            {},
            RenderGraph::GpuProfilingOptions{
                .try_bind_source =
                    [&](const RenderGraph::ExecutedPassInfo&,
                        CommandList&,
                        RHIQueueBinding,
                        Moer::uint64) {
                        ++bind_calls;
                        return false;
                    },
                .source_order_base = std::numeric_limits<Moer::uint64>::max(),
            }
        );
        suite.Check(
            !executed && bind_calls == 0 && record_calls == 0 &&
                publish_calls == 0 &&
                Contains(graph.GetCompileError(), "source order overflow"),
            test_name,
            "source-order overflow was not rejected before binding or publication"
        );
    }

    {
        RenderGraph graph("GpuProfileDisabledCompatibility");
        int record_calls = 0;
        Moer::Array<RHIRecordingSource> published{};
        graph.AddRecordPass(
            "Legacy",
            [](RenderGraph::PassBuilder& builder) {
                builder.SideEffect();
            },
            [&](CommandList& command_list) {
                ++record_calls;
                suite.Check(
                    !command_list.HasGpuScopeRecorder() &&
                        !command_list
                            .IsLegacyGpuProfilingSuppressedForGeneration(),
                    test_name,
                    "empty GPU profiling options changed the legacy path"
                );
                command_list.PushScopeWithTimeScope(
                    "LegacyCompatibilityScope"
                );
                command_list.PopScopeWithTimeScope();
            },
            RenderGraph::PassRecordPolicy::Serial
        );
        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const bool executed = graph.ExecuteFrontendRecordingPlan(
            {},
            false,
            [&](Moer::Array<RHIRecordingSource>&& sources) {
                published = std::move(sources);
            }
        );
        bool legacy_query_preserved = false;
        if (published.size() == 1) {
            const bool suppression_disabled =
                !published.front()
                     .command_list
                     ->IsLegacyGpuProfilingSuppressedForGeneration();
            CmdSubmit submit = published.front().command_list->Submit();
            const bool has_legacy_timestamp_markers =
                submit.cmds.size() == 2 &&
                submit.cmds[0]->Type() == Command::EType::Scope &&
                submit.cmds[1]->Type() == Command::EType::Scope &&
                static_cast<const ScopeCmd*>(submit.cmds[0].get())
                    ->QueryTimestamp() &&
                static_cast<const ScopeCmd*>(submit.cmds[1].get())
                    ->QueryTimestamp();
            legacy_query_preserved =
                suppression_disabled &&
                has_legacy_timestamp_markers &&
                submit.query_tokens.empty();
            ReleaseRenderGraphSubmit(submit);
        }
        suite.Check(
            executed && record_calls == 1 && legacy_query_preserved,
            test_name,
            "empty GPU profiling options suppressed the legacy timed query"
        );
    }
}

void TestGpuDependenciesDoNotSplitFrontendGroups(TestSuite& suite) {
    constexpr std::string_view test_name = "GPU dependencies do not split frontend groups";

    std::mutex              order_mutex{};
    std::vector<int>        record_order{};
    std::vector<size_t>     published_group_sizes{};

    const auto make_record = [&](int id) {
        return [&, id](Moer::Render::CommandList&) {
            {
                std::lock_guard lock(order_mutex);
                record_order.push_back(id);
            }
        };
    };

    RenderGraph graph("GpuDependenciesOnly");
    const auto  ordering_token = graph.CreateTransientToken("GpuOrderingToken");
    const auto  first = graph.AddRecordPass(
        "TokenProducer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(ordering_token).SideEffect();
        },
        make_record(1),
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    const auto second = graph.AddRecordPass(
        "TokenConsumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(ordering_token).SideEffect();
        },
        make_record(2),
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    graph.AddRecordPass(
        "ExplicitConsumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(second).SideEffect();
        },
        make_record(3),
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.frontend_dispatch_groups.size() == 1 &&
            plan.frontend_dispatch_groups.front().first_unit == 0 &&
            plan.frontend_dispatch_groups.front().unit_count == 3 &&
            plan.frontend_dispatch_groups.front().record_policy ==
                RenderGraph::PassRecordPolicy::ParallelAllowed,
        test_name,
        "GPU token and explicit edges must not become CPU recording boundaries"
    );
    Moer::TaskSystem::Init();
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        {},
        true,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
            published_group_sizes.push_back(sources.size());
        }
    );
    Moer::TaskSystem::ShutDown();

    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        published_group_sizes == std::vector<size_t>{1},
        test_name,
        "small adjacent parallel callbacks must share one source within their frontend group"
    );
    std::ranges::sort(record_order);
    suite.Check(
        record_order == std::vector<int>{1, 2, 3},
        test_name,
        "every frontend callback must still record exactly once"
    );
    (void)first;
}

void TestRecordingPublicationFailureTerminatesGates(TestSuite& suite) {
    constexpr std::string_view test_name = "recording publication failure terminates gates";
    RenderGraph                graph("PublicationFailure");
    graph.AddRecordPass(
        "Record",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());

    Moer::Array<Moer::Render::RHIRecordingSource> published{};
    const bool executed = graph.ExecuteFrontendRecordingPlan(
        {},
        true,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
            published = std::move(sources);
            throw std::runtime_error("injected publisher failure");
        }
    );
    suite.Check(
        !executed && Contains(graph.GetCompileError(), "injected publisher failure"),
        test_name,
        "publisher exceptions must become a graph execution failure"
    );
    suite.Check(
        published.size() == 1 &&
            published.front().completion.Status() == Moer::Render::ERHIRecordingStatus::Failed,
        test_name,
        "a published gate must never remain Pending when dispatch is abandoned"
    );

    RenderGraph ownership_graph("SourceOwnershipMutation");
    ownership_graph.AddRecordPass(
        "Record",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(ownership_graph.Compile(), test_name, ownership_graph.GetCompileError());
    bool publisher_called = false;
    const bool ownership_executed = ownership_graph.ExecuteFrontendRecordingPlan(
        [](std::span<const RenderGraph::ExecutedPassInfo>, Moer::Render::RHIRecordingSource& source) {
            source.completion = Moer::Render::RHIRecordingGate::Create();
        },
        false,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&&) {
            publisher_called = true;
        }
    );
    suite.Check(
        !ownership_executed && !publisher_called &&
            Contains(ownership_graph.GetCompileError(), "changed ownership"),
        test_name,
        "source setup must not replace the producer CommandList or completion gate"
    );
}

void TestFrameSetupTokenAndTlasBoundaryContract(TestSuite& suite) {
    constexpr std::string_view test_name = "frame setup token and TLAS boundary";
    RenderGraph                graph("FrameSetupContract");
    int                        bindless_identity = 0;
    int                        tlas_identity     = 0;
    int                        instance_identity = 0;
    int                        scene_identity    = 0;
    const auto bindless = graph.ImportToken("Bindless", &bindless_identity);
    const auto ready    = graph.CreateTransientToken("FrameSetupReady");
    const auto tlas = graph.ImportBuffer(
        "CurrentTLAS",
        &tlas_identity,
        RenderGraph::BufferDesc{.byte_size = 4096}
    );
    const auto previous_tlas = graph.ImportBuffer(
        "PreviousTLAS",
        &tlas_identity,
        RenderGraph::BufferDesc{.byte_size = 4096}
    );
    const auto instances = graph.ImportBuffer(
        "TLASInstances",
        &instance_identity,
        RenderGraph::BufferDesc{.byte_size = 4096}
    );
    const auto scene = graph.ImportBuffer(
        "SceneInstances",
        &scene_identity,
        RenderGraph::BufferDesc{.byte_size = 4096}
    );
    suite.Check(
        previous_tlas == tlas,
        test_name,
        "current/previous TLAS aliases must reuse one graph-local handle"
    );
    graph.SetInitialState(
        tlas,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.SetInitialState(
        instances,
        RenderGraph::BufferBoundary::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.SetInitialState(
        scene,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    const auto update = graph.AddRecordPass(
        "UpdateBindless",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(bindless).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto normalize = graph.AddRecordPass(
        "NormalizeScene",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(bindless)
                .Read(scene, RenderGraph::BufferAccessType::ShaderRead)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto build = graph.AddRecordPass(
        "BuildTLAS",
        [=](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(normalize)
                .Read(bindless)
                .Write(
                    instances,
                    RenderGraph::BufferAccessType::Storage
                )
                .Write(
                    tlas,
                    RenderGraph::BufferAccessType::AccelerationStructureWrite
                );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto finalize = graph.AddRecordPass(
        "FinalizeFrameSetup",
        [=](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(normalize)
                .DependsOn(build)
                .Read(bindless)
                .Read(
                    tlas,
                    RenderGraph::BufferAccessType::AccelerationStructureRead
                )
                .Write(ready)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto prepare_lights = graph.AddRecordPass(
        "PrepareLights",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(ready)
                .Read(bindless)
                .Read(
                    previous_tlas,
                    RenderGraph::BufferAccessType::AccelerationStructureRead
                )
                .Read(scene, RenderGraph::BufferAccessType::ShaderRead)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    graph.Export(
        tlas,
        RenderGraph::BufferAccessType::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        instances,
        RenderGraph::BufferAccessType::AccelerationStructureBuildInput,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan,
            update,
            normalize,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            bindless.Untyped()
        ),
        test_name,
        "bindless update must be a RAW predecessor of scene normalization"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            normalize,
            build,
            RenderGraph::EdgeReasonKind::Explicit
        ),
        test_name,
        "BuildTLAS must retain the explicit normalization dependency"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            build,
            finalize,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            tlas.Untyped()
        ),
        test_name,
        "FinalizeFrameSetup must normalize the built TLAS through a RAW edge"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            finalize,
            prepare_lights,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            ready.Untyped()
        ),
        test_name,
        "PrepareLights must consume the same-transaction FrameSetup ready token"
    );
    const auto* import_barrier =
        FindBarrier(plan, tlas.Untyped(), {}, build);
    const auto* consumer_barrier =
        FindBarrier(plan, tlas.Untyped(), build, finalize);
    suite.Check(
        import_barrier != nullptr && import_barrier->state_transition &&
            import_barrier->after_state ==
                RenderGraph::ResourceState{RenderGraph::BufferAccessType::AccelerationStructureWrite},
        test_name,
        "BuildTLAS must acquire an AS-write destination from the import boundary"
    );
    suite.Check(
        consumer_barrier != nullptr && consumer_barrier->state_transition &&
            consumer_barrier->after_state ==
                RenderGraph::ResourceState{RenderGraph::BufferAccessType::AccelerationStructureRead},
        test_name,
        "FinalizeFrameSetup must establish the AS-readable consumer boundary"
    );
    const auto* instance_import_barrier =
        FindBarrier(plan, instances.Untyped(), {}, build);
    suite.Check(
        instance_import_barrier != nullptr &&
            instance_import_barrier->state_transition &&
            instance_import_barrier->after_state ==
                RenderGraph::ResourceState{RenderGraph::BufferAccessType::Storage},
        test_name,
        "a fresh TLAS instance payload must be a write-only producer"
    );

    RenderGraph next_graph("PrimaryContract");
    const auto  next_bindless =
        next_graph.ImportToken("Bindless", &bindless_identity);
    suite.Check(
        next_bindless.Untyped().graph_id != bindless.Untyped().graph_id,
        test_name,
        "same physical token identity in another graph must remain a distinct transaction"
    );

    RenderGraph scene_update_graph("ExternalSceneReadBoundaryContract");
    int         updated_scene_identity = 0;
    int         stable_scene_identity  = 0;
    const auto  updated_scene = scene_update_graph.ImportBuffer(
        "UpdatedScene",
        &updated_scene_identity,
        RenderGraph::BufferDesc{.byte_size = 4096}
    );
    const auto stable_scene = scene_update_graph.ImportBuffer(
        "StableScene",
        &stable_scene_identity,
        RenderGraph::BufferDesc{.byte_size = 4096}
    );
    scene_update_graph.SetInitialState(
        updated_scene,
        RenderGraph::BufferAccessType::TransferDestination,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    scene_update_graph.SetInitialState(
        stable_scene,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto scene_read_boundary = scene_update_graph.AddRecordPass(
        "PublishSceneReads",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(updated_scene, RenderGraph::BufferAccessType::ShaderRead)
                .Read(stable_scene, RenderGraph::BufferAccessType::ShaderRead)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    suite.Check(
        scene_update_graph.Compile(),
        test_name,
        scene_update_graph.GetCompileError()
    );
    const auto* updated_scene_barrier = FindBarrier(
        scene_update_graph.GetCompiledPlan(),
        updated_scene.Untyped(),
        {},
        scene_read_boundary
    );
    suite.Check(
        updated_scene_barrier != nullptr &&
            updated_scene_barrier->state_transition &&
            updated_scene_barrier->memory_dependency &&
            updated_scene_barrier->before_state ==
                RenderGraph::ResourceState{RenderGraph::BufferAccessType::TransferDestination} &&
            updated_scene_barrier->after_state ==
                RenderGraph::ResourceState{RenderGraph::BufferAccessType::ShaderRead},
        test_name,
        "an uploaded scene buffer must publish transfer-write to shader-read"
    );
    const auto* stable_scene_boundary = FindBarrier(
        scene_update_graph.GetCompiledPlan(),
        stable_scene.Untyped(),
        {},
        scene_read_boundary
    );
    suite.Check(
        stable_scene_boundary == nullptr ||
            (!stable_scene_boundary->state_transition &&
             !stable_scene_boundary->memory_dependency),
        test_name,
        "an untouched shader-readable scene buffer must not invent a write boundary"
    );

    RenderGraph external_graph("ExternalTLASNormalizeContract");
    int         external_tlas_identity = 0;
    const auto  external_tlas = external_graph.ImportBuffer(
        "ExternallyBuiltTLAS",
        &external_tlas_identity,
        RenderGraph::BufferDesc{.byte_size = 4096}
    );
    external_graph.SetInitialState(
        external_tlas,
        RenderGraph::BufferAccessType::AccelerationStructureWrite,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    const auto external_normalize = external_graph.AddRecordPass(
        "NormalizeExternalTLAS",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(
                    external_tlas,
                    RenderGraph::BufferAccessType::AccelerationStructureRead
                )
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    external_graph.Export(
        external_tlas,
        RenderGraph::BufferAccessType::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    suite.Check(
        external_graph.Compile(),
        test_name,
        external_graph.GetCompileError()
    );
    const auto* external_import_barrier = FindBarrier(
        external_graph.GetCompiledPlan(),
        external_tlas.Untyped(),
        {},
        external_normalize
    );
    suite.Check(
        external_import_barrier != nullptr &&
            external_import_barrier->state_transition &&
            external_import_barrier->before_state ==
                RenderGraph::ResourceState{RenderGraph::BufferAccessType::AccelerationStructureWrite} &&
            external_import_barrier->after_state ==
                RenderGraph::ResourceState{RenderGraph::BufferAccessType::AccelerationStructureRead},
        test_name,
        "an externally built TLAS must normalize AS-write to AS-read"
    );
}

void TestNrdSerialControlIslandContract(TestSuite& suite) {
    constexpr std::string_view test_name =
        "NRD immutable serial-control island";
    RenderGraph graph("NrdIslandContract");
    int motion_identity = 0;
    int normal_identity = 0;
    int depth_identity = 0;
    int diffuse_identity = 0;
    int specular_identity = 0;
    int denoised_diffuse_identity = 0;
    int denoised_specular_identity = 0;
    const RenderGraph::TextureDesc desc{
        .mip_count   = 1,
        .layer_count = 1,
        .aspects     = RenderGraph::TextureAspect::Color,
    };
    const auto motion =
        graph.ImportTexture("Motion", &motion_identity, desc);
    const auto normal =
        graph.ImportTexture("NormalRoughness", &normal_identity, desc);
    const auto depth =
        graph.ImportTexture("ViewDepth", &depth_identity, desc);
    const auto diffuse =
        graph.ImportTexture("DiffuseLighting", &diffuse_identity, desc);
    const auto specular =
        graph.ImportTexture("SpecularLighting", &specular_identity, desc);
    const auto denoised_diffuse = graph.ImportTexture(
        "DenoisedDiffuse",
        &denoised_diffuse_identity,
        desc
    );
    const auto denoised_specular = graph.ImportTexture(
        "DenoisedSpecular",
        &denoised_specular_identity,
        desc
    );
    const auto setup_ready = graph.CreateTransientToken("FrameSetupReady");
    const auto nrd_ready   = graph.CreateTransientToken("NrdReady");

    for (const auto texture : {
             motion,
             normal,
             depth,
             diffuse,
             specular,
             denoised_diffuse,
             denoised_specular,
         }) {
        graph.SetInitialState(
            texture,
            RenderGraph::TextureAccessType::Sampled,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    graph.AddRecordPass(
        "FrameSetup",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(setup_ready).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto lighting = graph.AddRecordPass(
        "Lighting",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(setup_ready)
                .Write(
                    diffuse,
                    RenderGraph::TextureAccessType::Storage
                )
                .Write(
                    specular,
                    RenderGraph::TextureAccessType::Storage
                );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    const auto nrd = graph.AddRecordPass(
        "NRD",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(setup_ready)
                .Read(motion, RenderGraph::TextureAccessType::Sampled)
                .Read(normal, RenderGraph::TextureAccessType::Sampled)
                .Read(depth, RenderGraph::TextureAccessType::Sampled)
                .Read(diffuse, RenderGraph::TextureAccessType::Sampled)
                .Read(specular, RenderGraph::TextureAccessType::Sampled)
                .ReadWrite(
                    denoised_diffuse,
                    RenderGraph::TextureAccessType::Storage
                )
                .ReadWrite(
                    denoised_specular,
                    RenderGraph::TextureAccessType::Storage
                )
                .Write(nrd_ready)
                .SideEffect()

                .TranslateSerialControl();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::Serial
    );
    const auto composition = graph.AddRecordPass(
        "Composition",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(nrd_ready)
                .Read(
                    denoised_diffuse,
                    RenderGraph::TextureAccessType::Sampled
                )
                .Read(
                    denoised_specular,
                    RenderGraph::TextureAccessType::Sampled
                );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    graph.Export(
        denoised_diffuse,
        RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        denoised_specular,
        RenderGraph::TextureAccessType::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan,
            lighting,
            nrd,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            diffuse.Untyped()
        ) &&
            HasEdgeReason(
                plan,
                lighting,
                nrd,
                RenderGraph::EdgeReasonKind::ReadAfterWrite,
                specular.Untyped()
            ),
        test_name,
        "Lighting outputs must be RAW predecessors of NRD"
    );
    suite.Check(
        HasEdgeReason(
            plan,
            nrd,
            composition,
            RenderGraph::EdgeReasonKind::ReadAfterWrite,
            denoised_diffuse.Untyped()
        ) &&
            HasEdgeReason(
                plan,
                nrd,
                composition,
                RenderGraph::EdgeReasonKind::ReadAfterWrite,
                nrd_ready.Untyped()
            ),
        test_name,
        "NRD outputs and ready token must order Composition"
    );
    const auto* denoised_barrier = FindBarrier(
        plan,
        denoised_diffuse.Untyped(),
        nrd,
        composition
    );
    suite.Check(
        denoised_barrier != nullptr &&
            denoised_barrier->before_state ==
                RenderGraph::ResourceState{RenderGraph::TextureAccessType::Storage} &&
            denoised_barrier->after_state ==
                RenderGraph::ResourceState{RenderGraph::TextureAccessType::Sampled},
        test_name,
        "NRD storage outputs must normalize to Sampled before Composition"
    );
    suite.Check(
        plan.frontend_record_units[nrd.pass_id].record_policy ==
                RenderGraph::PassRecordPolicy::Serial &&
            plan.frontend_record_units[nrd.pass_id].native_translate_class ==
                Moer::Render::ERHITranslateExecutionClass::SerialControl,
        test_name,
        "only the NRD island must carry the explicit translation frontier"
    );
}

void TestNrdDenoiserFamiliesAreExplicit(TestSuite& suite) {
    constexpr std::string_view test_name =
        "NRD denoiser family classification";
    using Moer::Render::Ext::IsReblurDenoiser;
    using Moer::Render::Ext::IsRelaxDenoiser;

    suite.Check(
        IsReblurDenoiser(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR) &&
            IsReblurDenoiser(nrd::Denoiser::REBLUR_DIFFUSE) &&
            IsReblurDenoiser(nrd::Denoiser::REBLUR_SPECULAR) &&
            !IsReblurDenoiser(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR) &&
            !IsReblurDenoiser(nrd::Denoiser::MAX_NUM),
        test_name,
        "ReBLUR classification must not depend on enum ordering"
    );
    suite.Check(
        IsRelaxDenoiser(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR) &&
            IsRelaxDenoiser(nrd::Denoiser::RELAX_DIFFUSE) &&
            IsRelaxDenoiser(nrd::Denoiser::RELAX_SPECULAR) &&
            !IsRelaxDenoiser(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR) &&
            !IsRelaxDenoiser(nrd::Denoiser::MAX_NUM),
        test_name,
        "RELAX classification must not depend on enum ordering"
    );
}

} // namespace

int main() {
    TestSuite suite;
    TestStableSerialCallbackOrder(suite);
    TestMergedRecordingSharesStagingStreamAndPreservesSubmitOrder(suite);
    TestMergedRecordingFailureLeavesDestinationUntouched(suite);
    TestMergedRecordingFailureLeavesDestinationUntouched(suite, true);
    TestFrontendCommandListMergeRebasesCachedArguments(suite);

    TestUiDrawablePresentationContracts(suite);
    TestRasterStyleUiSlotClaimTracksNativeAcceptance(suite);
    TestUiPreparedFrameLifecycleIsOneShot(suite);
    TestUiFrameGraphTailContract(suite);
    TestUiFrameGraphRejectsInvalidPhysicalContracts(suite);
    TestImportedAliasIdentityAndDump(suite);
    TestResourceNameAndIdentityNamespaces(suite);
    TestDependencyLevelOrdering(suite);
    TestExplicitDependencyOrderRejections(suite);
    TestHazardsAndDeterministicDump(suite);
    TestTransientReadBeforeProducerFailsWithoutCallbacks(suite);
    TestCrossGraphHandlesAreRejected(suite);
    TestRecompileBeforeOneShotExecution(suite);
    TestLifetimeAndExportDump(suite);
    TestUntouchedImportedResourceCanBeExported(suite);
    TestTypedTextureSubresourceHazards(suite);
    TestTypedTextureLayerAndAspectHazards(suite);
    TestTransientTextureInitializationIsPerSubresource(suite);
    TestExportedTransientRequiresWholeResourceInitialization(suite);
    TestTypedPartialExportRequiresOnlyDeclaredRange(suite);
    TestTypedBufferRangeHazards(suite);
    TestLogicalResourceVersions(suite);
    TestInvalidTypedRangeIsRejected(suite);
    TestUnknownTextureAspectIsRejected(suite);
    TestTextureAttachmentStateMatchesSelectedAspects(suite);
    TestPresentationSourceBoundaryContract(suite);
    TestTypedAliasDescriptorMismatchIsRejected(suite);
    TestMultipleReadersProduceAllWarEdges(suite);
    TestExplicitStateValidation(suite);
    TestSameStateMemoryDependencies(suite);
    TestImportAndExportBoundaries(suite);
    TestQueueTopologySynchronization(suite);
    TestExclusiveOwnershipUsesCurrentOwnerFamily(suite);
    TestOwnershipWriterChainUsesCurrentFrontier(suite);
    TestUnspecifiedReadsPreserveAvailabilityFrontier(suite);
    TestSameNativeReadsDependOnTransitionFrontier(suite);
    TestOwnershipAcquireOrdersSiblingNativeQueue(suite);
    TestImportAvailabilityOrdersSiblingNativeQueue(suite);
    TestOwnershipTransitionCollapsesReaderFrontier(suite);
    TestWriterAdvancesAvailabilityFrontiers(suite);
    TestOwnershipEpochDoesNotReusePriorFamilySources(suite);
    TestTokenCrossQueueSyncHasNoOwnership(suite);
    TestBatchPairSyncDeduplication(suite);
    TestGpuWorkTypesAndBarrierSources(suite);
    TestPartialRangeBarrier(suite);
    TestMixedQueueExecuteRemainsDeclarationOrder(suite);
    TestStageTwoDumpDeterminism(suite);
    TestBarrierSourcesIgnoreUnrelatedLastRead(suite);
    TestFanInBarrierPlacementCoversEverySourceBatch(suite);
    TestUntouchedBoundaryWriteRequiresMemoryDependency(suite);
    TestShaderReadStatesRequireTransition(suite);
    TestReadTransitionWaitsForEveryActiveReader(suite);
    TestUndefinedImportMustBeInitializedBeforeRead(suite);
    TestUnknownExportMakesStatePlanIncomplete(suite);
    TestMergedAccessStateDeterminesPlanCompleteness(suite);
    TestInvalidRecordingPolicyFails(suite);
    TestZeroRecordingWorkloadFails(suite);
    TestRecordingBatchPlanAndClassification(suite);

    TestSerialControlTranslationIsADeclaredRecordingPolicy(suite);

    TestParallelRecordingFallsBackWithoutTaskGraph(suite);
    TestParallelRecordingDispatchAndJoin(suite);
    TestSharedRecordingRespectsSourceBoundaries(suite);
    TestConfiguredRecordingSourceReceivesAllPasses(suite);
    for (bool merge : {false, true}) {
        TestWeightedParallelRecordingSharesStreams(suite, merge);
        TestSharedStreamGpuProfilingKeepsPassScopes(suite, merge);
    }

    TestGpuProfilingBindingFailureContracts(suite);
    TestGpuDependenciesDoNotSplitFrontendGroups(suite);
    TestRecordingPublicationFailureTerminatesGates(suite);
    TestFrameSetupTokenAndTlasBoundaryContract(suite);
    TestNrdSerialControlIslandContract(suite);
    TestNrdDenoiserFamiliesAreExplicit(suite);

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraph: " << suite.FailureCount() << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraph: all checks passed\n";
    return EXIT_SUCCESS;
}
