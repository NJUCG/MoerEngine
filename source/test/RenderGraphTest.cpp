#include "rendergraph/RenderGraph.h"
#include "rhi/plugin/NrdPlugin.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

[[nodiscard]] bool WaveContains(const RenderGraph::CompiledWave& wave, RenderGraph::PassHandle pass) {
    return std::find(wave.passes.begin(), wave.passes.end(), pass) != wave.passes.end();
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

void TestStableSerialCallbackOrder(TestSuite& suite) {
    constexpr std::string_view test_name = "stable serial callback order";
    RenderGraph                graph("StableSerial");
    std::vector<int>           callback_order;

    graph.AddPass(
        "First",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&] {
            callback_order.push_back(1);
        }
    );
    graph.AddPass(
        "Second",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&] {
            callback_order.push_back(2);
        }
    );
    graph.AddPass(
        "Third",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&] {
            callback_order.push_back(3);
        }
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const bool executed = compiled && graph.Execute();
    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        callback_order == std::vector<int>{1, 2, 3},
        test_name,
        "callbacks must execute exactly once in declaration order"
    );
}

void TestPassCompletionObserverRunsAfterEachCallback(TestSuite& suite) {
    constexpr std::string_view test_name = "pass completion observer ordering";
    RenderGraph                graph("PassCompletionObserver");
    std::vector<std::string>   events;
    int                        cpu_value = 0;

    graph.AddPass(
        "Produce",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [&] {
            cpu_value = 41;
            events.emplace_back("execute:Produce");
        }
    );
    graph.AddPass(
        "Consume",
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                RenderGraph::QueueRole::Compute,
                RenderGraph::PipelineType::Compute
            );
            builder.SideEffect();
        },
        [&] {
            suite.Check(
                cpu_value == 41,
                test_name,
                "later pass callbacks must observe earlier callback CPU state"
            );
            events.emplace_back("execute:Consume");
        }
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const bool executed = compiled && graph.Execute([&](const RenderGraph::ExecutedPassInfo& info) {
        events.emplace_back(std::string("after:") + std::string(info.name));
        if (info.name == "Produce") {
            suite.Check(
                info.domain.queue == RenderGraph::QueueRole::Graphics && info.side_effect,
                test_name,
                "observer must receive the compiled Graphics domain and side-effect bit"
            );
        } else if (info.name == "Consume") {
            suite.Check(
                info.domain.queue == RenderGraph::QueueRole::Compute && info.side_effect,
                test_name,
                "observer must receive the compiled Compute domain and side-effect bit"
            );
        }
    });
    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        events == std::vector<std::string>{
                      "execute:Produce",
                      "after:Produce",
                      "execute:Consume",
                      "after:Consume",
                  },
        test_name,
        "observer must run once after each fully returned pass callback"
    );
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

    graph.AddPass(
        "ReadSceneColor",
        [texture_view](RenderGraph::PassBuilder& builder) {
            builder.Read(texture_view);
        },
        [] {}
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

[[nodiscard]] std::string BuildHazardDump(TestSuite& suite, std::string_view test_name) {
    RenderGraph graph("Hazards");
    int         physical_buffer = 0;
    const auto  shared          = graph.Import("Shared", RenderGraph::ResourceKind::Buffer, &physical_buffer);

    graph.AddPass(
        "Produce",
        [shared](RenderGraph::PassBuilder& builder) {
            builder.Write(shared);
        },
        [] {}
    );
    graph.AddPass(
        "Consume",
        [shared](RenderGraph::PassBuilder& builder) {
            builder.Read(shared);
        },
        [] {}
    );
    graph.AddPass(
        "Overwrite",
        [shared](RenderGraph::PassBuilder& builder) {
            builder.Write(shared);
        },
        [] {}
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    return graph.Dump();
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

    graph.AddPass(
        "InvalidRead",
        [transient](RenderGraph::PassBuilder& builder) {
            builder.Read(transient);
        },
        [&] {
            ++callback_count;
        }
    );

    suite.Check(
        !graph.Compile(), test_name, "Compile must reject a transient read before its first producer"
    );
    suite.Check(
        Contains(graph.GetCompileError(), "read before its first producer"),
        test_name,
        "Compile must provide a useful first-read diagnostic"
    );
    suite.Check(!graph.Execute(), test_name, "Execute must reject an uncompiled graph");
    suite.Check(callback_count == 0, test_name, "a rejected graph must execute zero callbacks");
}

void TestCrossGraphHandlesAreRejected(TestSuite& suite) {
    constexpr std::string_view resource_test_name = "cross-graph resource handle rejection";
    RenderGraph                owner_graph("ResourceOwner");
    const auto foreign_resource = owner_graph.Import("Foreign", RenderGraph::ResourceKind::Buffer);

    RenderGraph consumer_graph("ResourceConsumer");
    int         resource_callback_count = 0;
    consumer_graph.AddPass(
        "InvalidResourceUser",
        [foreign_resource](RenderGraph::PassBuilder& builder) {
            builder.Read(foreign_resource);
        },
        [&] {
            ++resource_callback_count;
        }
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
        !consumer_graph.Execute(), resource_test_name, "a graph with a foreign resource must not execute"
    );
    suite.Check(
        resource_callback_count == 0, resource_test_name, "foreign resource graph must run zero callbacks"
    );

    constexpr std::string_view pass_test_name = "cross-graph pass handle rejection";
    RenderGraph                pass_owner_graph("PassOwner");
    const auto                 foreign_pass = pass_owner_graph.AddPass("ForeignPass", {}, [] {});

    RenderGraph dependent_graph("PassConsumer");
    int         pass_callback_count = 0;
    dependent_graph.AddPass(
        "InvalidDependent",
        [foreign_pass](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(foreign_pass);
        },
        [&] {
            ++pass_callback_count;
        }
    );
    suite.Check(!dependent_graph.Compile(), pass_test_name, "Compile must reject a foreign pass handle");
    suite.Check(
        Contains(dependent_graph.GetCompileError(), "invalid pass handle") ||
            Contains(dependent_graph.GetCompileError(), "invalid pass"),
        pass_test_name,
        "foreign pass rejection must be diagnosed"
    );
    suite.Check(
        !dependent_graph.Execute(), pass_test_name, "a graph with a foreign dependency must not execute"
    );
    suite.Check(pass_callback_count == 0, pass_test_name, "foreign pass graph must run zero callbacks");
}

void TestCompileAndExecuteAreOneShot(TestSuite& suite) {
    constexpr std::string_view test_name = "Compile and Execute one-shot";
    RenderGraph                graph("OneShot");
    int                        callback_count = 0;
    graph.AddPass("OnlyPass", {}, [&] {
        ++callback_count;
    });

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const size_t compiled_pass_count = graph.GetCompiledPlan().execution_order.size();
    suite.Check(graph.Execute(), test_name, graph.GetCompileError());
    suite.Check(callback_count == 1, test_name, "the first Execute must run the callback once");
    suite.Check(!graph.Execute(), test_name, "a second Execute must be rejected");
    suite.Check(callback_count == 1, test_name, "a rejected second Execute must not repeat the callback");
    suite.Check(!graph.Compile(), test_name, "Compile after execution must be rejected");
    suite.Check(callback_count == 1, test_name, "a rejected second Compile must not repeat the callback");
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

    graph.AddPass(
        "CreateIntermediate",
        [transient](RenderGraph::PassBuilder& builder) {
            builder.Write(transient);
        },
        [] {}
    );
    graph.AddPass(
        "ReadIntermediate",
        [transient](RenderGraph::PassBuilder& builder) {
            builder.Read(transient);
        },
        [] {}
    );
    graph.AddPass(
        "UnrelatedTail",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
    graph.AddPass(
        "ExternalWindowWrite",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
    const auto       write_mip0 = graph.AddPass(
        "WriteMip0",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, RenderGraph::TextureRange::Mips(0, 1));
        },
        [&] {
            callback_order.push_back(0);
        }
    );
    const auto write_mip1 = graph.AddPass(
        "WriteMip1",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, RenderGraph::TextureRange::Mips(1, 1));
        },
        [&] {
            callback_order.push_back(1);
        }
    );
    const auto read_mip0 = graph.AddPass(
        "ReadMip0",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureRange::Mips(0, 1));
        },
        [&] {
            callback_order.push_back(2);
        }
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
        plan.dependency_waves.size() == 2 && WaveContains(plan.dependency_waves[0], write_mip0) &&
            WaveContains(plan.dependency_waves[0], write_mip1) &&
            WaveContains(plan.dependency_waves[1], read_mip0),
        test_name,
        "dependency waves must expose independent mip writers without changing execution policy"
    );
    suite.Check(graph.Execute(), test_name, graph.GetCompileError());
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

    const auto write_depth_layer0 = graph.AddPass(
        "WriteDepthLayer0",
        [texture, depth_layer0](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, depth_layer0);
        },
        [] {}
    );
    const auto write_depth_layer1 = graph.AddPass(
        "WriteDepthLayer1",
        [texture, depth_layer1](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, depth_layer1);
        },
        [] {}
    );
    const auto write_stencil_layer0 = graph.AddPass(
        "WriteStencilLayer0",
        [texture, stencil_layer0](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, stencil_layer0);
        },
        [] {}
    );
    const auto read_depth_layer0 = graph.AddPass(
        "ReadDepthLayer0",
        [texture, depth_layer0](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, depth_layer0);
        },
        [] {}
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

    graph.AddPass(
        "WriteMip0",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Write(texture, RenderGraph::TextureRange::Mips(0, 1));
        },
        [&] {
            ++callback_count;
        }
    );
    graph.AddPass(
        "ReadUninitializedMip1",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureRange::Mips(1, 1));
        },
        [&] {
            ++callback_count;
        }
    );

    suite.Check(!graph.Compile(), test_name, "reading an unwritten mip must be rejected");
    suite.Check(
        Contains(graph.GetCompileError(), "read before its first producer"),
        test_name,
        "subresource initialization failure must be diagnosed"
    );
    suite.Check(!graph.Execute(), test_name, "a rejected graph must not execute");
    suite.Check(callback_count == 0, test_name, "a rejected graph must execute zero callbacks");
}

void TestExportedTransientRequiresWholeResourceInitialization(TestSuite& suite) {
    constexpr std::string_view test_name = "exported transient whole-resource initialization";

    RenderGraph partial_graph("PartialTransientExport");
    const auto  partial_texture = partial_graph.CreateTransientTexture(
        "PartialMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    int partial_callback_count = 0;
    partial_graph.AddPass(
        "WriteMip0",
        [partial_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(partial_texture, RenderGraph::TextureRange::Mips(0, 1));
        },
        [&] {
            ++partial_callback_count;
        }
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
        !partial_graph.Execute(), test_name, "a graph with an invalid transient export must not execute"
    );
    suite.Check(
        partial_callback_count == 0, test_name, "an invalid transient export must execute zero callbacks"
    );

    RenderGraph complete_graph("CompleteTransientExport");
    const auto  complete_texture = complete_graph.CreateTransientTexture(
        "CompleteMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    complete_graph.AddPass(
        "WriteWholeTexture",
        [complete_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(complete_texture);
        },
        [] {}
    );
    complete_graph.Export(complete_texture);

    suite.Check(complete_graph.Compile(), test_name, complete_graph.GetCompileError());
    suite.Check(complete_graph.Execute(), test_name, complete_graph.GetCompileError());
}

void TestTypedPartialExportRequiresOnlyDeclaredRange(TestSuite& suite) {
    constexpr std::string_view test_name = "typed partial export initialization";

    RenderGraph valid_graph("TypedPartialExport");
    const auto  valid_texture = valid_graph.CreateTransientTexture(
        "PartialMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    const auto write_mip_zero = valid_graph.AddPass(
        "WriteMip0",
        [valid_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(
                valid_texture,
                RenderGraph::TextureState::RenderTarget,
                RenderGraph::TextureRange::Mips(0, 1)
            );
        },
        [] {}
    );
    valid_graph.Export(
        valid_texture,
        RenderGraph::TextureState::ShaderResource,
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
            final_barrier->range.kind == RenderGraph::ResourceKind::Texture &&
            final_barrier->range.texture.mip_first == 0 &&
            final_barrier->range.texture.mip_count == 1 &&
            valid_plan.state_plan_complete,
        test_name,
        "a typed partial export must cover only its initialized declared range"
    );

    RenderGraph invalid_graph("UninitializedTypedPartialExport");
    const auto  invalid_texture = invalid_graph.CreateTransientTexture(
        "PartialMipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    invalid_graph.AddPass(
        "WriteMip0",
        [invalid_texture](RenderGraph::PassBuilder& builder) {
            builder.Write(
                invalid_texture,
                RenderGraph::TextureState::RenderTarget,
                RenderGraph::TextureRange::Mips(0, 1)
            );
        },
        [] {}
    );
    invalid_graph.Export(
        invalid_texture,
        RenderGraph::TextureState::ShaderResource,
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

    const auto write_head = graph.AddPass(
        "WriteHead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferRange{.offset = 0, .size = 64});
        },
        [] {}
    );
    const auto read_adjacent = graph.AddPass(
        "ReadAdjacent",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferRange{.offset = 64, .size = 64});
        },
        [] {}
    );
    const auto read_overlap = graph.AddPass(
        "ReadOverlap",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferRange{.offset = 32, .size = 32});
        },
        [] {}
    );
    const auto overwrite_overlap = graph.AddPass(
        "OverwriteOverlap",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferRange{.offset = 32, .size = 16});
        },
        [] {}
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

    const auto produce = graph.AddPass(
        "Produce",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer);
        },
        [] {}
    );
    const auto consume = graph.AddPass(
        "Consume",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [] {}
    );
    const auto overwrite = graph.AddPass(
        "Overwrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer);
        },
        [] {}
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
        buffer.Untyped().index < plan.resources.size() &&
            plan.resources[buffer.Untyped().index].version_count == 2,
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
    graph.AddPass(
        "OutOfBoundsRead",
        [texture](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureRange::Mips(2, 1));
        },
        [&] {
            ++callback_count;
        }
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
        graph.AddPass(
            "SideEffect",
            [](RenderGraph::PassBuilder& builder) {
                builder.SideEffect();
            },
            [] {}
        );
    };

    RenderGraph color_access_graph("ColorAsDepthAttachment");
    const auto  color_access = color_access_graph.CreateTransientTexture(
        "Color", RenderGraph::TextureDesc{.aspects = RenderGraph::TextureAspect::Color}
    );
    color_access_graph.AddPass(
        "InvalidDepthWrite",
        [color_access](RenderGraph::PassBuilder& builder) {
            builder.Write(color_access, RenderGraph::TextureState::DepthStencilWrite);
        },
        [] {}
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
    depth_access_graph.AddPass(
        "InvalidColorWrite",
        [depth_access](RenderGraph::PassBuilder& builder) {
            builder.Write(depth_access, RenderGraph::TextureState::RenderTarget);
        },
        [] {}
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
    depth_storage_graph.AddPass(
        "InvalidStorageAccess",
        [depth_storage](RenderGraph::PassBuilder& builder) {
            builder.ReadWrite(depth_storage, RenderGraph::TextureState::UnorderedAccess);
        },
        [] {}
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
        RenderGraph::TextureState::DepthStencilRead,
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
        RenderGraph::TextureState::DepthStencilRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    final_graph.Export(
        final_texture,
        RenderGraph::TextureState::RenderTarget,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    add_side_effect(final_graph);
    suite.Check(
        !final_graph.Compile() && Contains(final_graph.GetCompileError(), "selected aspects"),
        test_name,
        "an exported depth range must reject the color render-target state"
    );

    RenderGraph present_graph("InvalidPresentAspect");
    int         present_physical = 0;
    const auto  present_texture = present_graph.ImportTexture(
        "Depth",
        &present_physical,
        RenderGraph::TextureDesc{.aspects = RenderGraph::TextureAspect::Depth}
    );
    present_graph.SetInitialState(
        present_texture,
        RenderGraph::TextureState::DepthStencilRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    present_graph.Export(
        present_texture,
        RenderGraph::TextureState::Present,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    add_side_effect(present_graph);
    suite.Check(
        !present_graph.Compile() && Contains(present_graph.GetCompileError(), "selected aspects"),
        test_name,
        "only color aspects may cross a Present boundary"
    );

    RenderGraph color_present_graph("ValidColorPresent");
    int         color_present_physical = 0;
    const auto  color_present_texture = color_present_graph.ImportTexture(
        "Color",
        &color_present_physical,
        RenderGraph::TextureDesc{.aspects = RenderGraph::TextureAspect::Color}
    );
    color_present_graph.SetInitialState(
        color_present_texture,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    color_present_graph.Export(
        color_present_texture,
        RenderGraph::TextureState::Present,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    add_side_effect(color_present_graph);
    suite.Check(
        color_present_graph.Compile(),
        test_name,
        "a color texture must remain exportable to Present: " +
            color_present_graph.GetCompileError()
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
        RenderGraph::TextureState::DepthStencilRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    valid_graph.AddPass(
        "ReadDepth",
        [valid_texture](RenderGraph::PassBuilder& builder) {
            builder.Read(
                valid_texture,
                RenderGraph::TextureState::DepthStencilRead,
                RenderGraph::TextureRange{.aspects = RenderGraph::TextureAspect::Depth}
            );
        },
        [] {}
    );
    valid_graph.Export(
        valid_texture,
        RenderGraph::TextureState::DepthStencilRead,
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
    graph.AddPass(
        "ReadCanonical",
        [canonical](RenderGraph::PassBuilder& builder) {
            builder.Read(canonical);
        },
        [] {}
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
    const auto reader_a = graph.AddPass(
        "ReaderA",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [] {}
    );
    const auto reader_b = graph.AddPass(
        "ReaderB",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [] {}
    );
    const auto writer = graph.AddPass(
        "Writer",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer);
        },
        [] {}
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
        RenderGraph::TextureState state;
        RenderGraph::QueueRole    queue;
        RenderGraph::PipelineType pipeline;
        bool                      valid;
    };
    const std::vector<TextureCase> texture_cases{
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureState::Sampled,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::PipelineType::Graphics,
         true},
        {RenderGraph::AccessMode::Write,
         RenderGraph::TextureState::RenderTarget,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::PipelineType::Graphics,
         true},
        {RenderGraph::AccessMode::ReadWrite,
         RenderGraph::TextureState::UnorderedAccess,
         RenderGraph::QueueRole::Compute,
         RenderGraph::PipelineType::Compute,
         true},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureState::TransferSource,
         RenderGraph::QueueRole::Copy,
         RenderGraph::PipelineType::Copy,
         true},
        {RenderGraph::AccessMode::Write,
         RenderGraph::TextureState::Sampled,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::PipelineType::Graphics,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureState::TransferDestination,
         RenderGraph::QueueRole::Copy,
         RenderGraph::PipelineType::Copy,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureState::RenderTarget,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::PipelineType::Graphics,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureState::Automatic,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::PipelineType::Graphics,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::TextureState::Sampled,
         RenderGraph::QueueRole::Copy,
         RenderGraph::PipelineType::Copy,
         false},
        {RenderGraph::AccessMode::Write,
         RenderGraph::TextureState::RenderTarget,
         RenderGraph::QueueRole::Compute,
         RenderGraph::PipelineType::Compute,
         false},
    };

    for (uint32_t index = 0; index < texture_cases.size(); ++index) {
        const auto& current = texture_cases[index];
        RenderGraph graph("TextureStateCase");
        int         physical = 0;
        const auto  texture = graph.ImportTexture("Texture", &physical, RenderGraph::TextureDesc{});
        graph.AddPass(
            "Access",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(current.queue, current.pipeline);
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
            [] {}
        );
        suite.Check(
            graph.Compile() == current.valid,
            test_name,
            "texture state/access/domain case " + std::to_string(index) + " had the wrong validity"
        );
    }

    struct BufferCase {
        RenderGraph::AccessMode  access;
        RenderGraph::BufferState state;
        RenderGraph::QueueRole   queue;
        RenderGraph::PipelineType pipeline;
        bool                     valid;
    };
    const std::vector<BufferCase> buffer_cases{
        {RenderGraph::AccessMode::Read,
         RenderGraph::BufferState::ShaderResource,
         RenderGraph::QueueRole::Compute,
         RenderGraph::PipelineType::Compute,
         true},
        {RenderGraph::AccessMode::Write,
         RenderGraph::BufferState::TransferDestination,
         RenderGraph::QueueRole::Copy,
         RenderGraph::PipelineType::Copy,
         true},
        {RenderGraph::AccessMode::Read,
         RenderGraph::BufferState::VertexBuffer,
         RenderGraph::QueueRole::Graphics,
         RenderGraph::PipelineType::Graphics,
         true},
        {RenderGraph::AccessMode::Write,
         RenderGraph::BufferState::ShaderResource,
         RenderGraph::QueueRole::Compute,
         RenderGraph::PipelineType::Compute,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::BufferState::AccelerationStructureWrite,
         RenderGraph::QueueRole::Compute,
         RenderGraph::PipelineType::RayTracing,
         false},
        {RenderGraph::AccessMode::Read,
         RenderGraph::BufferState::VertexBuffer,
         RenderGraph::QueueRole::Compute,
         RenderGraph::PipelineType::Compute,
         false},
    };

    for (uint32_t index = 0; index < buffer_cases.size(); ++index) {
        const auto& current = buffer_cases[index];
        RenderGraph graph("BufferStateCase");
        int         physical = 0;
        const auto  buffer = graph.ImportBuffer("Buffer", &physical, RenderGraph::BufferDesc{.byte_size = 64});
        graph.AddPass(
            "Access",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(current.queue, current.pipeline);
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
            [] {}
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
        return graph.AddPass(
            name,
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
                if (mode == RenderGraph::AccessMode::Read) {
                    builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
                } else {
                    builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
                }
            },
            [] {}
        );
    };

    for (const auto second_mode : {RenderGraph::AccessMode::Read, RenderGraph::AccessMode::Write}) {
        RenderGraph graph("UavWriteDependency");
        int         physical = 0;
        const auto  buffer = graph.ImportBuffer("Uav", &physical, RenderGraph::BufferDesc{.byte_size = 64});
        graph.SetInitialState(
            buffer,
            RenderGraph::BufferState::UnorderedAccess,
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
        RenderGraph::BufferState::UnorderedAccess,
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
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto known_read = known_graph.AddPass(
        "Read",
        [known](RenderGraph::PassBuilder& builder) {
            builder.Read(known, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
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
    const auto unknown_read = unknown_graph.AddPass(
        "Read",
        [unknown](RenderGraph::PassBuilder& builder) {
            builder.Read(unknown, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
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
    const auto write = final_graph.AddPass(
        "Write",
        [output](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Write(output, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    final_graph.Export(
        output,
        RenderGraph::BufferState::ShaderResource,
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
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    untouched_graph.Export(
        untouched,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    untouched_graph.AddPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
                          RenderGraph::TextureDesc::SharingMode sharing,
                          bool                                  queue_dependency,
                          bool                                  ownership,
                          bool                                  gpu_wait,
                          std::string_view                      expectation) {
        RenderGraph graph("QueueTopology", topology);
        const auto  buffer = graph.CreateTransientBuffer(
            "Shared", RenderGraph::BufferDesc{.byte_size = 64, .sharing_mode = sharing}
        );
        const auto producer = graph.AddPass(
            "Produce",
            [buffer](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
                builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
            },
            [] {}
        );
        const auto consumer = graph.AddPass(
            "Consume",
            [buffer](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics);
                builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
            },
            [] {}
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
        RenderGraph::TextureDesc::SharingMode::Exclusive,
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
        RenderGraph::TextureDesc::SharingMode::Exclusive,
        true,
        false,
        true,
        "different native queues in one family require synchronization but no ownership transfer"
    );
    check_case(
        RenderGraph::QueueTopology::DedicatedQueues(),
        RenderGraph::TextureDesc::SharingMode::Exclusive,
        true,
        true,
        true,
        "different families must transfer an exclusive resource"
    );
    check_case(
        RenderGraph::QueueTopology::DedicatedQueues(),
        RenderGraph::TextureDesc::SharingMode::Concurrent,
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
            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Exclusive,
        }
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto graphics_read = graph.AddPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto compute_read = graph.AddPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto copy_read = graph.AddPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy);
            builder.Read(buffer, RenderGraph::BufferState::TransferSource);
        },
        [] {}
    );
    graph.Export(
        buffer,
        RenderGraph::BufferState::ShaderResource,
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
    const auto writer = graph.AddPass(
        "GraphicsWrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto compute_read = graph.AddPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto copy_read = graph.AddPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy);
            builder.Read(buffer, RenderGraph::BufferState::TransferSource);
        },
        [] {}
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

void TestAutomaticReadsPreserveAvailabilityFrontier(TestSuite& suite) {
    constexpr std::string_view test_name = "automatic reads preserve availability frontier";
    RenderGraph graph("AutomaticAvailability", RenderGraph::QueueTopology::DedicatedQueues());
    const auto  buffer = graph.CreateTransientBuffer(
        "Concurrent",
        RenderGraph::BufferDesc{
            .byte_size    = 64,
            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Concurrent,
        }
    );
    const auto writer = graph.AddPass(
        "GraphicsWrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto transition = graph.AddPass(
        "GraphicsTransition",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto automatic_compute_a = graph.AddPass(
        "ComputeAutomaticA",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer);
        },
        [] {}
    );
    graph.AddPass(
        "GraphicsAutomatic",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [] {}
    );
    const auto automatic_compute_b = graph.AddPass(
        "ComputeAutomaticB",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer);
        },
        [] {}
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan,
            transition,
            automatic_compute_a,
            RenderGraph::EdgeReasonKind::StateTransition,
            buffer.Untyped()
        ) &&
            HasEdgeReason(
                plan,
                transition,
                automatic_compute_b,
                RenderGraph::EdgeReasonKind::StateTransition,
                buffer.Untyped()
            ) &&
            !HasEdgeReason(
                plan,
                writer,
                automatic_compute_b,
                RenderGraph::EdgeReasonKind::ReadAfterWrite,
                buffer.Untyped()
            ),
        test_name,
        "Automatic reads must inherit, preserve, and not bypass the transition availability frontier"
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
    const auto writer = graph.AddPass(
        "Write",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto transition = graph.AddPass(
        "TransitionRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto sibling_a = graph.AddPass(
        "CompatibleReadA",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto sibling_b = graph.AddPass(
        "CompatibleReadB",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
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
        plan.dependency_waves.size() == 3 && WaveContains(plan.dependency_waves[0], writer) &&
            WaveContains(plan.dependency_waves[1], transition) &&
            WaveContains(plan.dependency_waves[2], sibling_a) &&
            WaveContains(plan.dependency_waves[2], sibling_b),
        test_name,
        "the writer, transition, and compatible sibling reads must occupy three dependency waves"
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
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    const auto acquire = graph.AddPass(
        "GraphicsAcquire",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto sibling_read = graph.AddPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
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
        RenderGraph::BufferState::UnorderedAccess,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    const auto first_read = graph.AddPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer);
        },
        [] {}
    );
    const auto sibling_read = graph.AddPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer);
        },
        [] {}
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
        "an Automatic sibling access must wait for the pass that acquired external availability"
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
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    const auto old_owner_read = graph.AddPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy);
            builder.Read(buffer, RenderGraph::BufferState::TransferSource);
        },
        [] {}
    );
    const auto acquire = graph.AddPass(
        "GraphicsAcquire",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto transition = graph.AddPass(
        "ComputeTransition",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto sibling_read = graph.AddPass(
        "GraphicsSiblingRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
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
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto transition = graph.AddPass(
        "GraphicsTransition",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto writer = graph.AddPass(
        "ComputeWrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    graph.AddPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto sibling_read = graph.AddPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
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
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto graphics_epoch_zero = graph.AddPass(
        "GraphicsEpochZero",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto compute_epoch_zero = graph.AddPass(
        "ComputeEpochZero",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto graphics_epoch_one = graph.AddPass(
        "GraphicsEpochOne",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    const auto compute_epoch_one = graph.AddPass(
        "ComputeEpochOne",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
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
    const auto  producer = graph.AddPass(
        "Produce",
        [token](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Write(token);
        },
        [] {}
    );
    const auto consumer = graph.AddPass(
        "Consume",
        [token](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics);
            builder.Read(token);
        },
        [] {}
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
    graph.AddPass(
        "ProduceFirst",
        [first](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Write(first, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    graph.AddPass(
        "ProduceSecond",
        [second](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Write(second, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    graph.AddPass(
        "ConsumeBoth",
        [first, second](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics);
            builder.Read(first, RenderGraph::BufferState::ShaderResource);
            builder.Read(second, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
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

void TestPipelineDomainsAndBarrierSources(TestSuite& suite) {
    constexpr std::string_view test_name = "pipeline domains and barrier sources";
    RenderGraph graph("DomainsAndSources", RenderGraph::QueueTopology::DedicatedQueues());
    const auto buffer = graph.CreateTransientBuffer(
        "Shared",
        RenderGraph::BufferDesc{
            .byte_size    = 64,
            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Concurrent,
        }
    );
    const auto producer = graph.AddPass(
        "Producer",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto reader_a = graph.AddPass(
        "ReaderA",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::RayTracing);
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto reader_b = graph.AddPass(
        "ReaderB",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::RayTracing);
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto writer = graph.AddPass(
        "Writer",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::RayTracing);
            builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* producer_access = FindAccess(plan, producer, buffer.Untyped());
    const auto* reader_access   = FindAccess(plan, reader_a, buffer.Untyped());
    suite.Check(
        producer_access != nullptr &&
            producer_access->domain == RenderGraph::ExecutionDomain{
                RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute
            } &&
            reader_access != nullptr &&
            reader_access->domain == RenderGraph::ExecutionDomain{
                RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::RayTracing
            },
        test_name,
        "compiled accesses must retain their declared pipeline domains"
    );

    const auto reader_a_barrier = std::find_if(
        plan.barriers.begin(), plan.barriers.end(), [&](const RenderGraph::CompiledBarrier& barrier) {
            return barrier.resource == buffer.Untyped() && barrier.dst_pass == reader_a;
        }
    );
    suite.Check(
        reader_a_barrier != plan.barriers.end() && reader_a_barrier->src_domain.pipeline ==
                                                          RenderGraph::PipelineType::Compute &&
            reader_a_barrier->dst_domain.pipeline == RenderGraph::PipelineType::RayTracing &&
            HasBarrierSource(*reader_a_barrier, producer, RenderGraph::AccessMode::Write),
        test_name,
        "a fan-out barrier must retain the producer source and both pipeline scopes"
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
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto write = graph.AddPass(
        "WriteMiddle",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(
                buffer,
                RenderGraph::BufferState::UnorderedAccess,
                RenderGraph::BufferRange{.offset = 32, .size = 32}
            );
        },
        [] {}
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto* barrier = FindBarrier(graph.GetCompiledPlan(), buffer.Untyped(), {}, write);
    suite.Check(
        barrier != nullptr && barrier->range.kind == RenderGraph::ResourceKind::Buffer &&
            barrier->range.buffer == RenderGraph::BufferRange{.offset = 32, .size = 32},
        test_name,
        "a partial access must emit one barrier for exactly its canonical byte interval"
    );
}

void TestMixedQueueExecuteRemainsDeclarationOrder(TestSuite& suite) {
    constexpr std::string_view test_name = "mixed-queue serial execution order";
    RenderGraph graph("MixedQueueExecute", RenderGraph::QueueTopology::DedicatedQueues());
    std::vector<int> callbacks;
    const auto first = graph.AddPass(
        "Compute",
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute)
                .SideEffect();
        },
        [&] {
            callbacks.push_back(1);
        }
    );
    const auto second = graph.AddPass(
        "Graphics",
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                .SideEffect();
        },
        [&] {
            callbacks.push_back(2);
        }
    );
    const auto third = graph.AddPass(
        "Copy",
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy).SideEffect();
        },
        [&] {
            callbacks.push_back(3);
        }
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    suite.Check(
        graph.GetCompiledPlan().execution_order ==
            std::vector<RenderGraph::PassHandle>{first, second, third},
        test_name,
        "mixed logical queues must not change the production execution order"
    );
    suite.Check(graph.Execute(), test_name, graph.GetCompileError());
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
            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Exclusive,
        }
    );
    graph.AddPass(
        "CopyProduce",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy);
            builder.Write(
                buffer,
                RenderGraph::BufferState::TransferDestination,
                RenderGraph::BufferRange{.offset = 0, .size = 64}
            );
            builder.Write(
                buffer,
                RenderGraph::BufferState::TransferDestination,
                RenderGraph::BufferRange{.offset = 64, .size = 64}
            );
        },
        [] {}
    );
    graph.AddPass(
        "ComputeConsume",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
    );
    graph.Export(
        buffer,
        RenderGraph::BufferState::ShaderResource,
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
            Contains(first, "queue_syncs:\n") && Contains(first, "pipeline=compute") &&
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
            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Concurrent,
        }
    );
    const auto writer = graph.AddPass(
        "Writer",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto compute_read = graph.AddPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
    );
    const auto graphics_read = graph.AddPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
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
            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Concurrent,
        }
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    const auto copy_read = graph.AddPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy);
            builder.Read(buffer, RenderGraph::BufferState::TransferSource);
        },
        [] {}
    );
    const auto graphics_read = graph.AddPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::TransferSource);
        },
        [] {}
    );
    const auto compute_write = graph.AddPass(
        "ComputeWrite",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Write(buffer, RenderGraph::BufferState::UnorderedAccess);
        },
        [] {}
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
        RenderGraph::BufferState::UnorderedAccess,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    graph.Export(
        buffer,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.AddPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto storage_read = graph.AddPass(
        "StorageRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureState::ShaderResource);
        },
        [] {}
    );
    const auto sampled_read = graph.AddPass(
        "SampledRead",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(texture, RenderGraph::TextureState::Sampled);
        },
        [] {}
    );
    graph.Export(
        texture,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    const auto* barrier =
        FindBarrier(plan, texture.Untyped(), storage_read, sampled_read);
    suite.Check(
        barrier != nullptr && barrier->state_transition &&
            barrier->before_state == RenderGraph::ResourceState::Texture(
                RenderGraph::TextureState::ShaderResource
            ) &&
            barrier->after_state == RenderGraph::ResourceState::Texture(
                RenderGraph::TextureState::Sampled
            ) &&
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
            .sharing_mode = RenderGraph::TextureDesc::SharingMode::Concurrent,
        }
    );
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    const auto copy_read = graph.AddPass(
        "CopyRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy);
            builder.Read(buffer, RenderGraph::BufferState::TransferSource);
        },
        [] {}
    );
    const auto graphics_read = graph.AddPass(
        "GraphicsRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(buffer, RenderGraph::BufferState::TransferSource);
        },
        [] {}
    );
    const auto compute_read = graph.AddPass(
        "ComputeRead",
        [buffer](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute);
            builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
        },
        [] {}
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    read_graph.AddPass(
        "InvalidRead",
        [read_buffer](RenderGraph::PassBuilder& builder) {
            builder.Read(
                read_buffer,
                RenderGraph::BufferState::ShaderResource,
                RenderGraph::BufferRange{.offset = 0, .size = 64}
            );
        },
        [] {}
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    const auto write = write_graph.AddPass(
        "Initialize",
        [write_buffer](RenderGraph::PassBuilder& builder) {
            builder.Write(
                write_buffer,
                RenderGraph::BufferState::UnorderedAccess,
                RenderGraph::BufferRange{.offset = 0, .size = 64}
            );
        },
        [] {}
    );
    write_graph.Export(
        write_buffer,
        RenderGraph::BufferState::ShaderResource,
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange::Whole()
    );
    read_export_graph.Export(
        read_export_buffer,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    read_export_graph.AddPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange::Whole()
    );
    write_export_graph.Export(
        write_export_buffer,
        RenderGraph::BufferState::UnorderedAccess,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Write,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    write_export_graph.AddPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
            write_export_barrier->range.kind == RenderGraph::ResourceKind::Buffer &&
            write_export_barrier->range.buffer.offset == 0 &&
            write_export_barrier->range.buffer.size == 64 &&
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    read_write_export_graph.Export(
        read_write_export_buffer,
        RenderGraph::BufferState::UnorderedAccess,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::ReadWrite,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    read_write_export_graph.AddPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    disjoint_export_graph.Export(
        disjoint_export_buffer,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        RenderGraph::BufferRange{.offset = 64, .size = 64}
    );
    disjoint_export_graph.AddPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        RenderGraph::BufferRange{.offset = 0, .size = 64}
    );
    legacy_export_graph.Export(legacy_export_buffer);
    legacy_export_graph.AddPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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
    graph.AddPass(
        "SideEffect",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [] {}
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

    const auto check_whole_resource_merge = [&](bool automatic_first) {
        RenderGraph graph(
            automatic_first ? "AutomaticThenExplicitState" : "ExplicitThenAutomaticState"
        );
        const auto texture = graph.CreateTransientTexture(
            "Color", RenderGraph::TextureDesc{.mip_count = 1, .layer_count = 1}
        );
        const auto pass = graph.AddPass(
            "WriteColor",
            [=](RenderGraph::PassBuilder& builder) {
                if (automatic_first) {
                    builder.Write(texture);
                    builder.Write(texture, RenderGraph::TextureState::RenderTarget);
                } else {
                    builder.Write(texture, RenderGraph::TextureState::RenderTarget);
                    builder.Write(texture);
                }
            },
            [] {}
        );

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        const auto& plan   = graph.GetCompiledPlan();
        const auto* access = FindAccess(plan, pass, texture.Untyped());
        suite.Check(
            plan.state_plan_complete && plan.accesses.size() == 1 && access != nullptr &&
                access->mode == RenderGraph::AccessMode::Write &&
                access->state == RenderGraph::ResourceState::Texture(
                                     RenderGraph::TextureState::RenderTarget
                                 ),
            test_name,
            "an explicit declaration must canonicalize the same atomic cell regardless of order"
        );
    };

    check_whole_resource_merge(true);
    check_whole_resource_merge(false);

    RenderGraph disjoint_graph("DisjointAutomaticAndExplicitStates");
    const auto  disjoint_texture = disjoint_graph.CreateTransientTexture(
        "MipChain", RenderGraph::TextureDesc{.mip_count = 2, .layer_count = 1}
    );
    disjoint_graph.AddPass(
        "WriteMips",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(
                disjoint_texture,
                RenderGraph::TextureState::RenderTarget,
                RenderGraph::TextureRange::Mips(0, 1)
            );
            builder.Write(disjoint_texture, RenderGraph::TextureRange::Mips(1, 1));
        },
        [] {}
    );

    suite.Check(disjoint_graph.Compile(), test_name, disjoint_graph.GetCompileError());
    suite.Check(
        !disjoint_graph.GetCompiledPlan().state_plan_complete,
        test_name,
        "an automatic declaration on a disjoint atomic cell must keep the plan incomplete"
    );

    RenderGraph history_graph("AutomaticHistoryThenExplicitState");
    const auto  history_texture = history_graph.CreateTransientTexture(
        "History", RenderGraph::TextureDesc{.mip_count = 1, .layer_count = 1}
    );
    history_graph.AddPass(
        "AutomaticWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(history_texture);
        },
        [] {}
    );
    history_graph.AddPass(
        "ExplicitWrite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(history_texture, RenderGraph::TextureState::RenderTarget);
        },
        [] {}
    );

    suite.Check(history_graph.Compile(), test_name, history_graph.GetCompileError());
    suite.Check(
        !history_graph.GetCompiledPlan().state_plan_complete,
        test_name,
        "a later explicit state must not hide an unknown state from an earlier pass"
    );
}

void TestRecordingBatchPlanAndClassification(TestSuite& suite) {
    constexpr std::string_view test_name = "recording batch plan and classification";
    RenderGraph                graph("RecordingBatches");
    const auto                 hiz_token    = graph.CreateTransientToken("HiZ");
    const auto                 shadow_token = graph.CreateTransientToken("ShadowMask");

    const auto hiz = graph.AddRecordPass(
        "HiZBuild",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(hiz_token).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::ParallelRecordEligible,
        8
    );
    const auto shadow = graph.AddRecordPass(
        "DirectionalShadowMask",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(shadow_token).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::ParallelRecordEligible,
        4
    );
    const auto commit = graph.AddPass(
        "CommitHiZHistory",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(hiz_token).DependsOn(hiz).SideEffect();
        },
        [] {}
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.recording_batches.size() == 3 && plan.recording_batches[0].passes == std::vector{hiz} &&
            plan.recording_batches[1].passes == std::vector{shadow} &&
            plan.recording_batches[2].passes == std::vector{commit},
        test_name,
        "the compiler must emit one stable CPU ownership batch per pass"
    );
    suite.Check(
        plan.recording_batches.size() == 3 &&
            plan.recording_batches[0].execution ==
                RenderGraph::PassExecutionClass::ParallelRecordEligible &&
            plan.recording_batches[1].execution ==
                RenderGraph::PassExecutionClass::ParallelRecordEligible &&
            plan.recording_batches[2].execution == RenderGraph::PassExecutionClass::MainThread &&
            plan.recording_batches[0].workload == 8 &&
            plan.recording_batches[1].workload == 4,
        test_name,
        "recording class and workload must survive compilation"
    );
    suite.Check(
        plan.recording_batches.size() == 3 &&
            plan.recording_batches[0].dependency_wave ==
                plan.recording_batches[1].dependency_wave &&
            plan.recording_batches[2].dependency_wave >
                plan.recording_batches[0].dependency_wave,
        test_name,
        "independent recording passes must share a wave while their consumer follows"
    );
    suite.Check(
        Contains(graph.Dump(), "recording_batches:\n") &&
            Contains(
                graph.Dump(),
                "cpu=parallel-record translate=parallel workload=8 wave=0"
            ),
        test_name,
        "the deterministic dump must expose the executable CPU recording schedule"
    );
    suite.Check(
        !graph.Execute() && Contains(graph.GetCompileError(), "owned CommandLists"),
        test_name,
        "legacy serial Execute must reject record callbacks instead of sharing a CommandList"
    );
}

void TestRecordingCallbackClassMismatchFails(TestSuite& suite) {
    constexpr std::string_view test_name = "recording callback class mismatch";

    RenderGraph execute_graph("ExecuteAsRecord");
    execute_graph.AddPass(
        "BadExecute",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect().ParallelRecord();
        },
        [] {}
    );
    suite.Check(
        !execute_graph.Compile() &&
            Contains(execute_graph.GetCompileError(), "cannot use a command-recording class"),
        test_name,
        "an execute callback cannot masquerade as a record callback"
    );

    RenderGraph record_graph("RecordAsMainThread");
    record_graph.AddRecordPass(
        "BadRecord",
        [](RenderGraph::PassBuilder& builder) {
            builder.SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::MainThread
    );
    suite.Check(
        !record_graph.Compile() &&
            Contains(record_graph.GetCompileError(), "must use SerialRecord"),
        test_name,
        "a record callback must own a serial or parallel CommandList"
    );
}

void TestExternalControlIsAnUnmanagedJoinBoundary(TestSuite& suite) {
    constexpr std::string_view test_name = "external control join boundary";
    RenderGraph                graph("ExternalControlBoundary");
    const auto                 produced = graph.CreateTransientToken("Produced");
    const auto                 released = graph.CreateTransientToken("Released");
    std::vector<std::string>   events{};

    graph.AddRecordPass(
        "RecordBeforeExternal",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(produced).SideEffect();
        },
        [&](Moer::Render::CommandList&) { events.emplace_back("record"); },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    const auto external = graph.AddPass(
        "External",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(produced).Write(released).SideEffect().ExternalControl();
        },
        [&] { events.emplace_back("external"); }
    );
    const auto managed = graph.AddPass(
        "ManagedMain",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(released).DependsOn(external).SideEffect();
        },
        [&] { events.emplace_back("main"); }
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.recording_batches.size() == 3 &&
            plan.recording_batches[1].execution ==
                RenderGraph::PassExecutionClass::ExternalControl &&
            Contains(graph.Dump(), "cpu=external-control"),
        test_name,
        "the compiler and dump must retain the explicit external-control policy"
    );
    suite.Check(
        plan.queue_batches.size() == 3 &&
            plan.queue_batches[0].passes == std::vector{plan.recording_batches[0].passes.front()} &&
            !plan.queue_batches[0].external_control &&
            plan.queue_batches[1].passes == std::vector{external} &&
            plan.queue_batches[1].external_control &&
            plan.queue_batches[2].passes == std::vector{managed} &&
            !plan.queue_batches[2].external_control &&
            Contains(graph.Dump(), "external_control=true passes=[External]"),
        test_name,
        "external control must be a standalone queue batch that managed lowering cannot cross"
    );

    size_t published_groups = 0;
    size_t managed_observers = 0;
    const bool executed = graph.ExecuteRecording(
        [&](const RenderGraph::ExecutedPassInfo& pass) {
            ++managed_observers;
            events.emplace_back(std::string("observer:") + std::string(pass.name));
        },
        {},
        false,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
            ++published_groups;
            suite.Check(
                sources.size() == 1 &&
                    sources.front().completion.Status() ==
                        Moer::Render::ERHIRecordingStatus::Pending,
                test_name,
                "recording ownership must be published before the producer completes"
            );
        }
    );

    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        events == std::vector<std::string>{"record", "external", "main", "observer:ManagedMain"},
        test_name,
        "external control must join prior recording and bypass the managed-command observer"
    );
    suite.Check(
        published_groups == 1 && managed_observers == 1,
        test_name,
        "only owned record batches are published and only managed main-thread passes are observed"
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
                    RenderGraph::PipelineType::Compute
                )
                .SideEffect()
                .SerialRecord()
                .TranslateSerialControl();
        },
        [&](Moer::Render::CommandList&) { recorded = true; },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.recording_batches.size() == 1 &&
            plan.recording_batches.front().execution ==
                RenderGraph::PassExecutionClass::SerialRecord &&
            plan.recording_batches.front().translate_execution_class ==
                Moer::Render::ERHITranslateExecutionClass::SerialControl &&
            Contains(graph.Dump(), "translate=serial-control"),
        test_name,
        "the compiler and dump must retain the declared translation frontier"
    );

    const bool executed = graph.ExecuteRecording(
        {},
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
            builder.SideEffect().SerialRecord().TranslateSerialControl();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    suite.Check(weakened.Compile(), test_name, weakened.GetCompileError());
    bool weakened_published = false;
    const bool weakened_executed = weakened.ExecuteRecording(
        {},
        [](const RenderGraph::ExecutedPassInfo&,
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
            builder.SideEffect().SerialRecord().TranslateSerialControl();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    suite.Check(
        publisher_attempt.Compile(),
        test_name,
        publisher_attempt.GetCompileError()
    );
    bool command_list_kept_floor = false;
    const bool publisher_attempt_executed =
        publisher_attempt.ExecuteRecording(
            {},
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

void TestCpuPrepareReferencesIdentityWithoutGpuAccess(TestSuite& suite) {
    constexpr std::string_view test_name = "cpu prepare identity reference";
    RenderGraph                graph("CpuPrepareIdentity");
    int                        physical_texture = 0;
    const auto texture = graph.ImportTexture(
        "PreparedTexture",
        &physical_texture,
        RenderGraph::TextureDesc{.mip_count = 1, .layer_count = 1}
    );
    bool cpu_prepare_ran = false;
    graph.AddPass(
        "Prepare",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Reference(texture).SideEffect().CpuPrepare();
        },
        [&] { cpu_prepare_ran = true; }
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        plan.recording_batches.size() == 1 &&
            plan.recording_batches.front().execution ==
                RenderGraph::PassExecutionClass::CpuPrepare &&
            plan.accesses.empty() && plan.edges.empty() && plan.barriers.empty() &&
            plan.queue_batches.empty(),
        test_name,
        "a CPU identity reference must not synthesize GPU access, version, barrier, or queue metadata"
    );
    suite.Check(
        Contains(graph.Dump(), "cpu=cpu-prepare") &&
            Contains(graph.Dump(), "references=[PreparedTexture]"),
        test_name,
        "the dump must expose CPU-only scheduling and identity retention"
    );

    size_t observer_calls = 0;
    size_t published_groups = 0;
    const bool executed = graph.ExecuteRecording(
        [&](const RenderGraph::ExecutedPassInfo&) { ++observer_calls; },
        {},
        true,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&&) { ++published_groups; }
    );
    suite.Check(executed && cpu_prepare_ran, test_name, graph.GetCompileError());
    suite.Check(
        observer_calls == 0 && published_groups == 0,
        test_name,
        "CPU-only callbacks must bypass both managed CommandList sealing and recording publication"
    );
}

void TestCpuPrepareRejectsGpuAccess(TestSuite& suite) {
    constexpr std::string_view test_name = "cpu prepare rejects GPU access";
    RenderGraph                graph("CpuPrepareGpuAccess");
    int                        physical_texture = 0;
    const auto texture = graph.ImportTexture(
        "GpuTexture",
        &physical_texture,
        RenderGraph::TextureDesc{.mip_count = 1, .layer_count = 1}
    );
    graph.AddPass(
        "InvalidPrepare",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(texture).SideEffect().CpuPrepare();
        },
        [] {}
    );

    suite.Check(
        !graph.Compile() && Contains(graph.GetCompileError(), "may only access token resources"),
        test_name,
        "CPU-only callbacks must use Reference rather than synthesize GPU accesses"
    );
}

void TestCpuPrepareIsExcludedFromGpuQueuePlan(TestSuite& suite) {
    constexpr std::string_view test_name = "cpu prepare is excluded from GPU queue plan";
    RenderGraph graph("CpuPrepareQueuePlan", RenderGraph::QueueTopology::DedicatedQueues());
    const auto  token = graph.CreateTransientToken("CpuPreparedToken");
    const auto  prepare = graph.AddPass(
        "PrepareOnCpu",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Compute, RenderGraph::PipelineType::Compute)
                .Write(token)
                .SideEffect()
                .CpuPrepare();
        },
        [] {}
    );
    const auto consume = graph.AddPass(
        "ConsumeOnGpu",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                .Read(token)
                .SideEffect();
        },
        [] {}
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& plan = graph.GetCompiledPlan();
    suite.Check(
        HasEdgeReason(
            plan, prepare, consume, RenderGraph::EdgeReasonKind::ReadAfterWrite, token.Untyped()
        ) &&
            plan.recording_batches.size() == 2 && plan.queue_batches.size() == 1 &&
            plan.queue_batches.front().passes == std::vector<RenderGraph::PassHandle>{consume} &&
            plan.queue_syncs.empty(),
        test_name,
        "CPU preparation must order callbacks without requiring a nonexistent GPU signal"
    );
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
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    };
    add_record("First", first_token, 1);
    add_record("Second", second_token, 2);

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    Moer::Array<Moer::Render::RHIRecordingGateView> published_gates{};
    const bool executed = graph.ExecuteRecording(
        {},
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
        published_gates.size() == 2 &&
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
            builder.Write(gpu_dependency).SideEffect();
        },
        make_record(1),
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    const auto second = graph.AddRecordPass(
        "SecondRecord",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(gpu_dependency).Write(second_token).SideEffect();
        },
        make_record(2),
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    bool joined_before_main = false;
    graph.AddPass(
        "JoinBoundary",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(gpu_dependency)
                .Read(second_token)
                .DependsOn(first)
                .DependsOn(second)
                .SideEffect();
        },
        [&] {
            joined_before_main = rendezvous.inflight.load() == 0 && rendezvous.entered == 2;
        }
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto& recording_plan = graph.GetCompiledPlan().recording_batches;
    suite.Check(
        recording_plan.size() == 3 &&
            recording_plan[1].dependency_wave > recording_plan[0].dependency_wave,
        test_name,
        "the fixture must place the two record callbacks in different GPU dependency waves"
    );

    Moer::Array<Moer::Array<Moer::Render::RHIRecordingSource>> published{};
    bool published_pending = false;
    bool distinct_command_lists = false;
    Moer::TaskSystem::Init();
    const bool executed = graph.ExecuteRecording(
        {},
        {},
        true,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
            published_pending = sources.size() == 2 &&
                                sources[0].completion.Status() ==
                                    Moer::Render::ERHIRecordingStatus::Pending &&
                                sources[1].completion.Status() ==
                                    Moer::Render::ERHIRecordingStatus::Pending;
            distinct_command_lists = sources.size() == 2 &&
                                     sources[0].command_list != sources[1].command_list;
            published.emplace_back(std::move(sources));
        }
    );
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
        published.size() == 1 && published.front().size() == 2 && published_pending,
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
        joined_before_main,
        test_name,
        "a caller-thread pass must not run until the recording wave has joined"
    );
    suite.Check(
        avoided_named_thread_reentry && explicitly_drained_named_task,
        test_name,
        "joining recording workers must not pump unrelated named-thread work"
    );
    suite.Check(
        published.size() == 1 &&
            published.front()[0].completion.Status() ==
                Moer::Render::ERHIRecordingStatus::Succeeded &&
            published.front()[1].completion.Status() ==
                Moer::Render::ERHIRecordingStatus::Succeeded,
        test_name,
        "every published source gate must reach a successful terminal state"
    );
}

void TestCpuRecordingDependenciesSplitParallelGroups(TestSuite& suite) {
    constexpr std::string_view test_name = "CPU recording dependencies split parallel groups";

    std::atomic<int>        inflight{0};
    std::atomic<int>        max_inflight{0};
    std::mutex              order_mutex{};
    std::vector<int>        record_order{};
    std::vector<size_t>     published_group_sizes{};

    const auto make_record = [&](int id) {
        return [&, id](Moer::Render::CommandList&) {
            const int active = inflight.fetch_add(1) + 1;
            int       observed = max_inflight.load();
            while (observed < active &&
                   !max_inflight.compare_exchange_weak(observed, active)) {}
            {
                std::lock_guard lock(order_mutex);
                record_order.push_back(id);
            }
            inflight.fetch_sub(1);
        };
    };

    RenderGraph graph("CpuRecordingDependencies");
    const auto  cpu_token = graph.CreateTransientToken("CpuToken");
    const auto  first = graph.AddRecordPass(
        "TokenProducer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(cpu_token).SideEffect();
        },
        make_record(1),
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    const auto second = graph.AddRecordPass(
        "TokenConsumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(cpu_token).SideEffect();
        },
        make_record(2),
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "ExplicitConsumer",
        [=](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(second).SideEffect();
        },
        make_record(3),
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    Moer::TaskSystem::Init();
    const bool executed = graph.ExecuteRecording(
        {},
        {},
        true,
        [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
            published_group_sizes.push_back(sources.size());
        }
    );
    Moer::TaskSystem::ShutDown();

    suite.Check(executed, test_name, graph.GetCompileError());
    suite.Check(
        published_group_sizes == std::vector<size_t>{1, 1, 1},
        test_name,
        "token and explicit dependencies must form CPU recording group boundaries"
    );
    suite.Check(
        max_inflight.load() == 1 && record_order == std::vector<int>{1, 2, 3},
        test_name,
        "CPU-dependent callbacks must record in dependency order without overlap"
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
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());

    Moer::Array<Moer::Render::RHIRecordingSource> published{};
    const bool executed = graph.ExecuteRecording(
        {},
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
        RenderGraph::PassExecutionClass::SerialRecord
    );
    suite.Check(ownership_graph.Compile(), test_name, ownership_graph.GetCompileError());
    bool publisher_called = false;
    const bool ownership_executed = ownership_graph.ExecuteRecording(
        {},
        [](const RenderGraph::ExecutedPassInfo&, Moer::Render::RHIRecordingSource& source) {
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
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.SetInitialState(
        instances,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.SetInitialState(
        scene,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    const auto update = graph.AddRecordPass(
        "UpdateBindless",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(bindless).SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    const auto normalize = graph.AddRecordPass(
        "NormalizeScene",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(bindless)
                .Read(scene, RenderGraph::BufferState::ShaderResource)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    const auto build = graph.AddRecordPass(
        "BuildTLAS",
        [=](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(normalize)
                .Read(bindless)
                .Write(
                    instances,
                    RenderGraph::BufferState::UnorderedAccess
                )
                .Write(
                    tlas,
                    RenderGraph::BufferState::AccelerationStructureWrite
                );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    const auto finalize = graph.AddRecordPass(
        "FinalizeFrameSetup",
        [=](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(normalize)
                .DependsOn(build)
                .Read(bindless)
                .Read(
                    tlas,
                    RenderGraph::BufferState::AccelerationStructureRead
                )
                .Write(ready)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    const auto prepare_lights = graph.AddRecordPass(
        "PrepareLights",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(ready)
                .Read(bindless)
                .Read(
                    previous_tlas,
                    RenderGraph::BufferState::AccelerationStructureRead
                )
                .Read(scene, RenderGraph::BufferState::ShaderResource)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.Export(
        tlas,
        RenderGraph::BufferState::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        instances,
        RenderGraph::BufferState::AccelerationStructureBuildInput,
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
                RenderGraph::ResourceState::Buffer(
                    RenderGraph::BufferState::AccelerationStructureWrite
                ),
        test_name,
        "BuildTLAS must acquire an AS-write destination from the import boundary"
    );
    suite.Check(
        consumer_barrier != nullptr && consumer_barrier->state_transition &&
            consumer_barrier->after_state ==
                RenderGraph::ResourceState::Buffer(
                    RenderGraph::BufferState::AccelerationStructureRead
                ),
        test_name,
        "FinalizeFrameSetup must establish the AS-readable consumer boundary"
    );
    const auto* instance_import_barrier =
        FindBarrier(plan, instances.Untyped(), {}, build);
    suite.Check(
        instance_import_barrier != nullptr &&
            instance_import_barrier->state_transition &&
            instance_import_barrier->after_state ==
                RenderGraph::ResourceState::Buffer(
                    RenderGraph::BufferState::UnorderedAccess
                ),
        test_name,
        "a fresh TLAS instance payload must be a write-only producer"
    );

    RenderGraph next_graph("PrimaryContract");
    const auto  next_bindless =
        next_graph.ImportToken("Bindless", &bindless_identity);
    suite.Check(
        next_bindless.Untyped().owner_id != bindless.Untyped().owner_id,
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
        RenderGraph::BufferState::TransferDestination,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    scene_update_graph.SetInitialState(
        stable_scene,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    const auto scene_read_boundary = scene_update_graph.AddRecordPass(
        "PublishSceneReads",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(updated_scene, RenderGraph::BufferState::ShaderResource)
                .Read(stable_scene, RenderGraph::BufferState::ShaderResource)
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
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
                RenderGraph::ResourceState::Buffer(
                    RenderGraph::BufferState::TransferDestination
                ) &&
            updated_scene_barrier->after_state ==
                RenderGraph::ResourceState::Buffer(
                    RenderGraph::BufferState::ShaderResource
                ),
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
        RenderGraph::BufferState::AccelerationStructureWrite,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    const auto external_normalize = external_graph.AddRecordPass(
        "NormalizeExternalTLAS",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(
                    external_tlas,
                    RenderGraph::BufferState::AccelerationStructureRead
                )
                .SideEffect();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    external_graph.Export(
        external_tlas,
        RenderGraph::BufferState::AccelerationStructureRead,
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
                RenderGraph::ResourceState::Buffer(
                    RenderGraph::BufferState::AccelerationStructureWrite
                ) &&
            external_import_barrier->after_state ==
                RenderGraph::ResourceState::Buffer(
                    RenderGraph::BufferState::AccelerationStructureRead
                ),
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
            RenderGraph::TextureState::Sampled,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    graph.AddRecordPass(
        "FrameSetup",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(setup_ready).SideEffect().SerialRecord();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    const auto lighting = graph.AddRecordPass(
        "Lighting",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(setup_ready)
                .Write(
                    diffuse,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(
                    specular,
                    RenderGraph::TextureState::UnorderedAccess
                );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    const auto nrd = graph.AddRecordPass(
        "NRD",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(setup_ready)
                .Read(motion, RenderGraph::TextureState::Sampled)
                .Read(normal, RenderGraph::TextureState::Sampled)
                .Read(depth, RenderGraph::TextureState::Sampled)
                .Read(diffuse, RenderGraph::TextureState::Sampled)
                .Read(specular, RenderGraph::TextureState::Sampled)
                .ReadWrite(
                    denoised_diffuse,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .ReadWrite(
                    denoised_specular,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(nrd_ready)
                .SideEffect()
                .SerialRecord()
                .TranslateSerialControl();
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );
    const auto composition = graph.AddRecordPass(
        "Composition",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .Read(nrd_ready)
                .Read(
                    denoised_diffuse,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    denoised_specular,
                    RenderGraph::TextureState::Sampled
                );
        },
        [](Moer::Render::CommandList&) {},
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.Export(
        denoised_diffuse,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        denoised_specular,
        RenderGraph::TextureState::Sampled,
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
                RenderGraph::ResourceState::Texture(
                    RenderGraph::TextureState::UnorderedAccess
                ) &&
            denoised_barrier->after_state ==
                RenderGraph::ResourceState::Texture(
                    RenderGraph::TextureState::Sampled
                ),
        test_name,
        "NRD storage outputs must normalize to Sampled before Composition"
    );
    suite.Check(
        plan.recording_batches[nrd.index].execution ==
                RenderGraph::PassExecutionClass::SerialRecord &&
            plan.recording_batches[nrd.index].translate_execution_class ==
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
    TestPassCompletionObserverRunsAfterEachCallback(suite);
    TestImportedAliasIdentityAndDump(suite);
    TestHazardsAndDeterministicDump(suite);
    TestTransientReadBeforeProducerFailsWithoutCallbacks(suite);
    TestCrossGraphHandlesAreRejected(suite);
    TestCompileAndExecuteAreOneShot(suite);
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
    TestTypedAliasDescriptorMismatchIsRejected(suite);
    TestMultipleReadersProduceAllWarEdges(suite);
    TestExplicitStateValidation(suite);
    TestSameStateMemoryDependencies(suite);
    TestImportAndExportBoundaries(suite);
    TestQueueTopologySynchronization(suite);
    TestExclusiveOwnershipUsesCurrentOwnerFamily(suite);
    TestOwnershipWriterChainUsesCurrentFrontier(suite);
    TestAutomaticReadsPreserveAvailabilityFrontier(suite);
    TestSameNativeReadsDependOnTransitionFrontier(suite);
    TestOwnershipAcquireOrdersSiblingNativeQueue(suite);
    TestImportAvailabilityOrdersSiblingNativeQueue(suite);
    TestOwnershipTransitionCollapsesReaderFrontier(suite);
    TestWriterAdvancesAvailabilityFrontiers(suite);
    TestOwnershipEpochDoesNotReusePriorFamilySources(suite);
    TestTokenCrossQueueSyncHasNoOwnership(suite);
    TestBatchPairSyncDeduplication(suite);
    TestPipelineDomainsAndBarrierSources(suite);
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
    TestRecordingBatchPlanAndClassification(suite);
    TestRecordingCallbackClassMismatchFails(suite);
    TestExternalControlIsAnUnmanagedJoinBoundary(suite);
    TestSerialControlTranslationIsADeclaredRecordingPolicy(suite);
    TestCpuPrepareReferencesIdentityWithoutGpuAccess(suite);
    TestCpuPrepareRejectsGpuAccess(suite);
    TestCpuPrepareIsExcludedFromGpuQueuePlan(suite);
    TestParallelRecordingFallsBackWithoutTaskGraph(suite);
    TestParallelRecordingDispatchAndJoin(suite);
    TestCpuRecordingDependenciesSplitParallelGroups(suite);
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
