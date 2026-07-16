#include "rendergraph/RenderGraph.h"

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

void TestStableSerialCallbackOrder(TestSuite& suite) {
    constexpr std::string_view test_name = "stable serial callback order";
    RenderGraph                graph("StableSerial");
    std::vector<int>           callback_order;

    graph.AddPass("First", [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); }, [&] {
        callback_order.push_back(1);
    });
    graph.AddPass("Second", [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); }, [&] {
        callback_order.push_back(2);
    });
    graph.AddPass("Third", [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); }, [&] {
        callback_order.push_back(3);
    });

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

    const auto texture = graph.Import(
        "SceneColor",
        RenderGraph::ResourceKind::Texture,
        &physical_resource
    );
    const auto texture_view = graph.Import(
        "SceneColorView",
        RenderGraph::ResourceKind::Texture,
        &physical_resource
    );
    suite.Check(texture == texture_view, test_name, "aliases of one physical identity must return one handle");

    graph.AddPass(
        "ReadSceneColor",
        [texture_view](RenderGraph::PassBuilder& builder) { builder.Read(texture_view); },
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
    const auto  shared = graph.Import("Shared", RenderGraph::ResourceKind::Buffer, &physical_buffer);

    graph.AddPass(
        "Produce",
        [shared](RenderGraph::PassBuilder& builder) { builder.Write(shared); },
        [] {}
    );
    graph.AddPass(
        "Consume",
        [shared](RenderGraph::PassBuilder& builder) { builder.Read(shared); },
        [] {}
    );
    graph.AddPass(
        "Overwrite",
        [shared](RenderGraph::PassBuilder& builder) { builder.Write(shared); },
        [] {}
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    return graph.Dump();
}

void TestHazardsAndDeterministicDump(TestSuite& suite) {
    constexpr std::string_view test_name = "hazards and deterministic dump";
    const std::string          first_dump = BuildHazardDump(suite, test_name);
    const std::string          second_dump = BuildHazardDump(suite, test_name);

    suite.Check(first_dump == second_dump, test_name, "equivalent graph declarations must produce identical dumps");
    suite.Check(
        Contains(first_dump, "Produce -> Consume reasons=[RAW:Shared, serial]"),
        test_name,
        "dump must report the RAW dependency"
    );
    suite.Check(
        Contains(first_dump, "Consume -> Overwrite reasons=[WAR:Shared, serial]"),
        test_name,
        "dump must report the WAR dependency"
    );
    suite.Check(
        Contains(first_dump, "Produce -> Overwrite reasons=[WAW:Shared]"),
        test_name,
        "dump must report the WAW dependency"
    );
}

void TestTransientReadBeforeProducerFailsWithoutCallbacks(TestSuite& suite) {
    constexpr std::string_view test_name = "transient first read rejection";
    RenderGraph                graph("TransientFirstRead");
    const auto transient = graph.CreateTransient("Scratch", RenderGraph::ResourceKind::Texture);
    int        callback_count = 0;

    graph.AddPass(
        "InvalidRead",
        [transient](RenderGraph::PassBuilder& builder) { builder.Read(transient); },
        [&] { ++callback_count; }
    );

    suite.Check(!graph.Compile(), test_name, "Compile must reject a transient read before its first producer");
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
        [foreign_resource](RenderGraph::PassBuilder& builder) { builder.Read(foreign_resource); },
        [&] { ++resource_callback_count; }
    );
    suite.Check(!consumer_graph.Compile(), resource_test_name, "Compile must reject a foreign resource handle");
    suite.Check(
        Contains(consumer_graph.GetCompileError(), "invalid resource"),
        resource_test_name,
        "foreign resource rejection must be diagnosed"
    );
    suite.Check(!consumer_graph.Execute(), resource_test_name, "a graph with a foreign resource must not execute");
    suite.Check(resource_callback_count == 0, resource_test_name, "foreign resource graph must run zero callbacks");

    constexpr std::string_view pass_test_name = "cross-graph pass handle rejection";
    RenderGraph                pass_owner_graph("PassOwner");
    const auto foreign_pass = pass_owner_graph.AddPass("ForeignPass", {}, [] {});

    RenderGraph dependent_graph("PassConsumer");
    int         pass_callback_count = 0;
    dependent_graph.AddPass(
        "InvalidDependent",
        [foreign_pass](RenderGraph::PassBuilder& builder) { builder.DependsOn(foreign_pass); },
        [&] { ++pass_callback_count; }
    );
    suite.Check(!dependent_graph.Compile(), pass_test_name, "Compile must reject a foreign pass handle");
    suite.Check(
        Contains(dependent_graph.GetCompileError(), "invalid pass handle") ||
            Contains(dependent_graph.GetCompileError(), "invalid pass"),
        pass_test_name,
        "foreign pass rejection must be diagnosed"
    );
    suite.Check(!dependent_graph.Execute(), pass_test_name, "a graph with a foreign dependency must not execute");
    suite.Check(pass_callback_count == 0, pass_test_name, "foreign pass graph must run zero callbacks");
}

void TestCompileAndExecuteAreOneShot(TestSuite& suite) {
    constexpr std::string_view test_name = "Compile and Execute one-shot";
    RenderGraph                graph("OneShot");
    int                        callback_count = 0;
    graph.AddPass("OnlyPass", {}, [&] { ++callback_count; });

    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    suite.Check(graph.Execute(), test_name, graph.GetCompileError());
    suite.Check(callback_count == 1, test_name, "the first Execute must run the callback once");
    suite.Check(!graph.Execute(), test_name, "a second Execute must be rejected");
    suite.Check(callback_count == 1, test_name, "a rejected second Execute must not repeat the callback");
    suite.Check(!graph.Compile(), test_name, "Compile after execution must be rejected");
    suite.Check(callback_count == 1, test_name, "a rejected second Compile must not repeat the callback");
}

void TestLifetimeAndExportDump(TestSuite& suite) {
    constexpr std::string_view test_name = "transient lifetime and export dump";
    RenderGraph                graph("Lifetime");
    const auto transient = graph.CreateTransient("Intermediate", RenderGraph::ResourceKind::Texture);
    graph.Export(transient);

    graph.AddPass(
        "CreateIntermediate",
        [transient](RenderGraph::PassBuilder& builder) { builder.Write(transient); },
        [] {}
    );
    graph.AddPass(
        "ReadIntermediate",
        [transient](RenderGraph::PassBuilder& builder) { builder.Read(transient); },
        [] {}
    );
    graph.AddPass("UnrelatedTail", [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); }, [] {});

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const std::string dump = graph.Dump();
    suite.Check(
        Contains(
            dump,
            "Intermediate kind=texture lifetime=transient first=0 last=1 exported=true aliases=[]"
        ),
        test_name,
        "dump must report the transient's first use, last use and export state"
    );
}

void TestUntouchedImportedResourceCanBeExported(TestSuite& suite) {
    constexpr std::string_view test_name = "untouched imported export boundary";
    RenderGraph                graph("ImportedForwarding");
    int                        physical_output = 0;
    const auto output = graph.Import(
        "Output",
        RenderGraph::ResourceKind::Texture,
        &physical_output
    );
    graph.Export(output);
    graph.AddPass(
        "ExternalWindowWrite",
        [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); },
        [] {}
    );

    const bool compiled = graph.Compile();
    suite.Check(compiled, test_name, graph.GetCompileError());
    const std::string dump = graph.Dump();
    suite.Check(
        Contains(dump, "Output kind=texture lifetime=imported first=unused last=unused exported=true"),
        test_name,
        "an imported resource must be exportable unchanged without a fake graph access"
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

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraph: " << suite.FailureCount() << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraph: all checks passed\n";
    return EXIT_SUCCESS;
}
