#include "rendergraph/RenderGraph.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
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
    const auto  buffer = graph.CreateTransientBuffer("Shared", RenderGraph::BufferDesc{.byte_size = 64});
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
        Contains(first, "barrier_owner=existing_rhi_vulkan_path") &&
            Contains(first, "sync_plan=shadow external_endpoints=unbound") &&
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

[[nodiscard]] std::string BuildCompatibleStateOrderDump(
    TestSuite& suite,
    std::string_view test_name,
    bool reverse
) {
    RenderGraph graph("CompatibleStateOrder");
    int         physical = 0;
    const auto  texture = graph.ImportTexture("Texture", &physical, RenderGraph::TextureDesc{});
    graph.SetInitialState(
        texture,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.AddPass(
        "Read",
        [=](RenderGraph::PassBuilder& builder) {
            if (reverse) {
                builder.Read(texture, RenderGraph::TextureState::Sampled);
                builder.Read(texture, RenderGraph::TextureState::ShaderResource);
            } else {
                builder.Read(texture, RenderGraph::TextureState::ShaderResource);
                builder.Read(texture, RenderGraph::TextureState::Sampled);
            }
        },
        [] {}
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    suite.Check(
        !graph.GetCompiledPlan().accesses.empty() &&
            graph.GetCompiledPlan().accesses.front().state == RenderGraph::ResourceState::Texture(
                RenderGraph::TextureState::ShaderResource
            ),
        test_name,
        "compatible shader-read states must canonicalize to one stable state"
    );
    const std::string dump = graph.Dump();
    const auto        compiled_plan_offset = dump.find("compiled_accesses:\n");
    return compiled_plan_offset == std::string::npos ? dump : dump.substr(compiled_plan_offset);
}

void TestCompatibleStateCanonicalization(TestSuite& suite) {
    constexpr std::string_view test_name = "compatible state canonicalization";
    const auto forward = BuildCompatibleStateOrderDump(suite, test_name, false);
    const auto reverse = BuildCompatibleStateOrderDump(suite, test_name, true);
    suite.Check(
        forward == reverse,
        test_name,
        "swapping compatible declarations in one pass must not change the compiled-plan dump"
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
    suite.Check(write_graph.Compile(), test_name, write_graph.GetCompileError());
    const auto* barrier = FindBarrier(write_graph.GetCompiledPlan(), write_buffer.Untyped(), {}, write);
    suite.Check(
        barrier != nullptr && barrier->discard_previous_contents,
        test_name,
        "the first write after Undefined must be represented as a discard transition"
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

} // namespace

int main() {
    TestSuite suite;
    TestStableSerialCallbackOrder(suite);
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
    TestTypedBufferRangeHazards(suite);
    TestLogicalResourceVersions(suite);
    TestInvalidTypedRangeIsRejected(suite);
    TestUnknownTextureAspectIsRejected(suite);
    TestTypedAliasDescriptorMismatchIsRejected(suite);
    TestMultipleReadersProduceAllWarEdges(suite);
    TestExplicitStateValidation(suite);
    TestSameStateMemoryDependencies(suite);
    TestImportAndExportBoundaries(suite);
    TestQueueTopologySynchronization(suite);
    TestTokenCrossQueueSyncHasNoOwnership(suite);
    TestBatchPairSyncDeduplication(suite);
    TestPipelineDomainsAndBarrierSources(suite);
    TestPartialRangeBarrier(suite);
    TestMixedQueueExecuteRemainsDeclarationOrder(suite);
    TestStageTwoDumpDeterminism(suite);
    TestBarrierSourcesIgnoreUnrelatedLastRead(suite);
    TestFanInBarrierPlacementCoversEverySourceBatch(suite);
    TestUntouchedBoundaryWriteRequiresMemoryDependency(suite);
    TestCompatibleStateCanonicalization(suite);
    TestReadTransitionWaitsForEveryActiveReader(suite);
    TestUndefinedImportMustBeInitializedBeforeRead(suite);
    TestUnknownExportMakesStatePlanIncomplete(suite);

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraph: " << suite.FailureCount() << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraph: all checks passed\n";
    return EXIT_SUCCESS;
}
