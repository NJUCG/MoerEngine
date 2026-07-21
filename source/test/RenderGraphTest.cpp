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

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraph: " << suite.FailureCount() << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraph: all checks passed\n";
    return EXIT_SUCCESS;
}
