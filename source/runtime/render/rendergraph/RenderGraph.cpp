#include "rendergraph/RenderGraph.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <sstream>
#include <utility>

namespace Moer::Render {

namespace {

std::atomic<uint64_t> s_next_graph_id{1};

const char* ToString(RenderGraph::ResourceKind kind) {
    switch (kind) {
        case RenderGraph::ResourceKind::Texture:
            return "texture";
        case RenderGraph::ResourceKind::Buffer:
            return "buffer";
        case RenderGraph::ResourceKind::Token:
            return "token";
    }
    return "unknown";
}

const char* ToString(uint8_t access_mode) {
    switch (access_mode) {
        case 0:
            return "R";
        case 1:
            return "W";
        case 2:
            return "RW";
        default:
            return "?";
    }
}

void AppendUnique(std::vector<std::string>& values, std::string value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.emplace_back(std::move(value));
    }
}

} // namespace

RenderGraph::RenderGraph(std::string_view graph_name) :
    name(graph_name), graph_id(s_next_graph_id.fetch_add(1, std::memory_order_relaxed)) {
    assert(graph_id != 0 && "RenderGraph id counter wrapped.");
}

RenderGraph::~RenderGraph() = default;

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::Read(ResourceHandle resource, std::string_view range) {
    graph.AddAccess(pass_index, resource, AccessMode::Read, range);
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::Write(ResourceHandle resource, std::string_view range) {
    graph.AddAccess(pass_index, resource, AccessMode::Write, range);
    return *this;
}

RenderGraph::PassBuilder&
RenderGraph::PassBuilder::ReadWrite(ResourceHandle resource, std::string_view range) {
    graph.AddAccess(pass_index, resource, AccessMode::ReadWrite, range);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::DependsOn(PassHandle dependency) {
    graph.AddDependency(pass_index, dependency);
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::SideEffect() {
    graph.MarkSideEffect(pass_index);
    return *this;
}

RenderGraph::ResourceHandle
RenderGraph::Import(std::string_view resource_name, ResourceKind kind, const void* physical_identity) {
    if (!InvalidateCompile()) {
        return {};
    }
    if (resource_name.empty()) {
        declaration_errors.emplace_back("resource name cannot be empty");
        return {};
    }

    for (uint32_t index = 0; index < resources.size(); ++index) {
        auto& resource = resources[index];
        if (resource.name == resource_name ||
            std::find(resource.aliases.begin(), resource.aliases.end(), resource_name) !=
                resource.aliases.end()) {
            declaration_errors.emplace_back("duplicate resource or alias name: " + std::string(resource_name));
            return {};
        }
        if (physical_identity != nullptr && resource.physical_identity == physical_identity) {
            if (!resource.imported) {
                declaration_errors.emplace_back(
                    "physical identity aliases transient resource: " + std::string(resource_name)
                );
                return {};
            }
            if (resource.kind != kind) {
                declaration_errors.emplace_back(
                    "physical identity imported with different resource kinds: " + std::string(resource_name)
                );
                return {};
            }
            resource.aliases.emplace_back(resource_name);
            return ResourceHandle{index, graph_id};
        }
    }

    ResourceDeclaration resource{};
    resource.name              = resource_name;
    resource.kind              = kind;
    resource.physical_identity = physical_identity;
    resource.imported          = true;
    resources.emplace_back(std::move(resource));
    return ResourceHandle{static_cast<uint32_t>(resources.size() - 1), graph_id};
}

RenderGraph::ResourceHandle RenderGraph::CreateTransient(
    std::string_view resource_name,
    ResourceKind kind
) {
    if (!InvalidateCompile()) {
        return {};
    }
    if (resource_name.empty()) {
        declaration_errors.emplace_back("resource name cannot be empty");
        return {};
    }
    for (const auto& resource : resources) {
        if (resource.name == resource_name ||
            std::find(resource.aliases.begin(), resource.aliases.end(), resource_name) !=
                resource.aliases.end()) {
            declaration_errors.emplace_back("duplicate resource name: " + std::string(resource_name));
            return {};
        }
    }

    ResourceDeclaration resource{};
    resource.name      = resource_name;
    resource.kind      = kind;
    resource.imported  = false;
    resources.emplace_back(std::move(resource));
    return ResourceHandle{static_cast<uint32_t>(resources.size() - 1), graph_id};
}

void RenderGraph::Export(ResourceHandle resource) {
    if (!InvalidateCompile()) {
        return;
    }
    if (!IsValidResource(resource)) {
        declaration_errors.emplace_back("Export received an invalid resource handle");
        return;
    }
    resources[resource.index].exported = true;
}

RenderGraph::PassHandle RenderGraph::AddPass(
    std::string_view pass_name,
    const SetupCallback& setup,
    ExecuteCallback execute
) {
    if (!InvalidateCompile()) {
        return {};
    }
    if (pass_name.empty()) {
        declaration_errors.emplace_back("pass name cannot be empty");
        return {};
    }
    if (!execute) {
        declaration_errors.emplace_back("pass has no execute callback: " + std::string(pass_name));
        return {};
    }
    if (std::any_of(passes.begin(), passes.end(), [&](const PassDeclaration& pass) {
            return pass.name == pass_name;
        })) {
        declaration_errors.emplace_back("duplicate pass name: " + std::string(pass_name));
        return {};
    }

    PassDeclaration pass{};
    pass.name    = pass_name;
    pass.execute = std::move(execute);
    passes.emplace_back(std::move(pass));
    const uint32_t pass_index = static_cast<uint32_t>(passes.size() - 1);
    PassBuilder builder(*this, pass_index);
    if (setup) {
        setup(builder);
    }
    return PassHandle{pass_index, graph_id};
}

bool RenderGraph::Compile() {
    if (executed) {
        return FailCompile("graph has already executed and cannot be compiled again");
    }
    compiled = false;
    compile_error.clear();
    execution_order.clear();
    compiled_edges.clear();
    for (auto& resource : resources) {
        resource.first_use = PassHandle::InvalidIndex;
        resource.last_use  = PassHandle::InvalidIndex;
    }

    if (!declaration_errors.empty()) {
        return FailCompile(declaration_errors.front());
    }
    if (passes.empty()) {
        return FailCompile("graph contains no passes");
    }

    auto add_edge = [&](uint32_t src, uint32_t dst, std::string reason) {
        if (src == dst) {
            return;
        }
        auto edge = std::find_if(compiled_edges.begin(), compiled_edges.end(), [&](const CompiledEdge& item) {
            return item.src == src && item.dst == dst;
        });
        if (edge == compiled_edges.end()) {
            compiled_edges.push_back(CompiledEdge{src, dst, {std::move(reason)}});
        } else {
            AppendUnique(edge->reasons, std::move(reason));
        }
    };

    // Phase 8's production policy is intentionally serial. Adjacent edges make
    // the old declaration order an explicit invariant while the graph still
    // compiles and reports the underlying resource hazards.
    for (uint32_t pass_index = 1; pass_index < passes.size(); ++pass_index) {
        add_edge(pass_index - 1, pass_index, "serial");
    }

    for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
        for (const PassHandle dependency : passes[pass_index].explicit_dependencies) {
            if (!IsValidPass(dependency)) {
                return FailCompile("pass '" + passes[pass_index].name + "' has an invalid explicit dependency");
            }
            add_edge(dependency.index, pass_index, "explicit");
        }
    }

    struct HazardState {
        uint32_t              last_writer = PassHandle::InvalidIndex;
        std::vector<uint32_t> readers;
        bool                  ever_written = false;
    };
    std::vector<HazardState> hazards(resources.size());

    for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
        struct CombinedAccess {
            bool read = false;
            bool write = false;
        };
        std::vector<CombinedAccess> combined(resources.size());
        for (const auto& access : passes[pass_index].accesses) {
            if (!IsValidResource(access.resource)) {
                return FailCompile("pass '" + passes[pass_index].name + "' references an invalid resource");
            }
            auto& item = combined[access.resource.index];
            item.read |= access.mode == AccessMode::Read || access.mode == AccessMode::ReadWrite;
            item.write |= access.mode == AccessMode::Write || access.mode == AccessMode::ReadWrite;
        }

        for (uint32_t resource_index = 0; resource_index < combined.size(); ++resource_index) {
            const CombinedAccess access = combined[resource_index];
            if (!access.read && !access.write) {
                continue;
            }
            auto& hazard  = hazards[resource_index];
            auto& resource = resources[resource_index];

            if (access.read) {
                if (!resource.imported && hazard.last_writer == PassHandle::InvalidIndex) {
                    return FailCompile(
                        "transient resource '" + resource.name + "' is read before its first producer in pass '" +
                        passes[pass_index].name + "'"
                    );
                }
                if (hazard.last_writer != PassHandle::InvalidIndex) {
                    add_edge(hazard.last_writer, pass_index, "RAW:" + resource.name);
                }
            }

            if (access.write) {
                if (hazard.last_writer != PassHandle::InvalidIndex) {
                    add_edge(hazard.last_writer, pass_index, "WAW:" + resource.name);
                }
                for (const uint32_t reader : hazard.readers) {
                    add_edge(reader, pass_index, "WAR:" + resource.name);
                }
                hazard.readers.clear();
                hazard.last_writer = pass_index;
                hazard.ever_written = true;
            } else if (access.read &&
                       std::find(hazard.readers.begin(), hazard.readers.end(), pass_index) == hazard.readers.end()) {
                hazard.readers.push_back(pass_index);
            }
        }
    }

    for (uint32_t index = 0; index < resources.size(); ++index) {
        if (!resources[index].imported && !hazards[index].ever_written) {
            return FailCompile("transient resource has no producer: " + resources[index].name);
        }
    }

    std::sort(compiled_edges.begin(), compiled_edges.end(), [](const CompiledEdge& lhs, const CompiledEdge& rhs) {
        return lhs.src < rhs.src || (lhs.src == rhs.src && lhs.dst < rhs.dst);
    });
    for (auto& edge : compiled_edges) {
        std::sort(edge.reasons.begin(), edge.reasons.end());
    }

    std::vector<uint32_t> indegree(passes.size(), 0);
    for (const auto& edge : compiled_edges) {
        ++indegree[edge.dst];
    }
    std::vector<bool> scheduled(passes.size(), false);
    while (execution_order.size() < passes.size()) {
        uint32_t next = PassHandle::InvalidIndex;
        for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
            if (!scheduled[pass_index] && indegree[pass_index] == 0) {
                next = pass_index;
                break;
            }
        }
        if (next == PassHandle::InvalidIndex) {
            return FailCompile("pass dependency cycle detected");
        }
        scheduled[next] = true;
        execution_order.push_back(next);
        for (const auto& edge : compiled_edges) {
            if (edge.src == next) {
                assert(indegree[edge.dst] > 0);
                --indegree[edge.dst];
            }
        }
    }

    std::vector<uint32_t> execution_position(passes.size(), PassHandle::InvalidIndex);
    for (uint32_t position = 0; position < execution_order.size(); ++position) {
        execution_position[execution_order[position]] = position;
    }
    for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
        const uint32_t position = execution_position[pass_index];
        for (const auto& access : passes[pass_index].accesses) {
            auto& resource = resources[access.resource.index];
            resource.first_use = std::min(resource.first_use, position);
            if (resource.last_use == PassHandle::InvalidIndex) {
                resource.last_use = position;
            } else {
                resource.last_use = std::max(resource.last_use, position);
            }
        }
    }
    // An imported resource may be forwarded unchanged to a consumer outside
    // the graph. That is a valid import/export boundary even when no graph pass
    // touches it. Transients are already required to have a producer above.

    compiled = true;
    return true;
}

bool RenderGraph::Execute() {
    if (!compiled) {
        compile_error = "Execute called before a successful Compile";
        return false;
    }
    if (executed) {
        compile_error = "a per-frame RenderGraph can only be executed once";
        return false;
    }
    executed = true;

    for (uint32_t position = 0; position < execution_order.size(); ++position) {
        auto& pass = passes[execution_order[position]];
        assert(pass.execute);
        pass.execute();
    }
    return true;
}

std::string RenderGraph::Dump() const {
    std::ostringstream stream;
    stream << "graph='" << name << "' mode=serial barrier_owner=rhi_command_preprocess compiled="
           << (compiled ? "true" : "false") << " executed=" << (executed ? "true" : "false")
           << " passes=" << passes.size() << " resources=" << resources.size();
    if (!compile_error.empty()) {
        stream << " error='" << compile_error << "'";
    }
    stream << '\n';

    stream << "passes:\n";
    const bool has_complete_order = execution_order.size() == passes.size();
    for (uint32_t item_index = 0; item_index < passes.size(); ++item_index) {
        const uint32_t pass_index = has_complete_order ? execution_order[item_index] : item_index;
        const auto& pass = passes[pass_index];
        stream << "  [" << item_index << "] " << pass.name << " declared=" << pass_index
               << " scheduled=" << (has_complete_order ? "true" : "false")
               << " side_effect=" << (pass.side_effect ? "true" : "false") << " accesses=[";
        for (uint32_t access_index = 0; access_index < pass.accesses.size(); ++access_index) {
            const auto& access = pass.accesses[access_index];
            if (access_index != 0) {
                stream << ", ";
            }
            stream << ToString(static_cast<uint8_t>(access.mode)) << ':'
                   << resources[access.resource.index].name << '(' << access.range << ')';
        }
        stream << "]\n";
    }

    stream << "resources:\n";
    for (uint32_t index = 0; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        stream << "  [" << index << "] " << resource.name << " kind=" << ToString(resource.kind)
               << " lifetime=" << (resource.imported ? "imported" : "transient")
               << " first=";
        if (resource.first_use == PassHandle::InvalidIndex) {
            stream << "unused";
        } else {
            stream << resource.first_use;
        }
        stream << " last=";
        if (resource.last_use == PassHandle::InvalidIndex) {
            stream << "unused";
        } else {
            stream << resource.last_use;
        }
        stream << " exported=" << (resource.exported ? "true" : "false") << " aliases=[";
        auto aliases = resource.aliases;
        std::sort(aliases.begin(), aliases.end());
        for (uint32_t alias_index = 0; alias_index < aliases.size(); ++alias_index) {
            if (alias_index != 0) {
                stream << ", ";
            }
            stream << aliases[alias_index];
        }
        stream << "]\n";
    }

    stream << "edges:\n";
    for (const auto& edge : compiled_edges) {
        stream << "  " << passes[edge.src].name << " -> " << passes[edge.dst].name << " reasons=[";
        for (uint32_t reason_index = 0; reason_index < edge.reasons.size(); ++reason_index) {
            if (reason_index != 0) {
                stream << ", ";
            }
            stream << edge.reasons[reason_index];
        }
        stream << "]\n";
    }
    return stream.str();
}

void RenderGraph::AddAccess(
    uint32_t pass_index,
    ResourceHandle resource,
    AccessMode mode,
    std::string_view range
) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size()) {
        declaration_errors.emplace_back("access declaration has invalid pass index");
        return;
    }
    if (!IsValidResource(resource)) {
        declaration_errors.emplace_back("pass '" + passes[pass_index].name + "' declared an invalid resource");
        return;
    }
    passes[pass_index].accesses.push_back(AccessDeclaration{resource, mode, std::string(range)});
}

void RenderGraph::AddDependency(uint32_t pass_index, PassHandle dependency) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size() || !IsValidPass(dependency)) {
        declaration_errors.emplace_back("explicit dependency contains an invalid pass handle");
        return;
    }
    if (dependency.index == pass_index) {
        declaration_errors.emplace_back("pass cannot depend on itself: " + passes[pass_index].name);
        return;
    }
    auto& dependencies = passes[pass_index].explicit_dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), dependency) == dependencies.end()) {
        dependencies.push_back(dependency);
    }
}

void RenderGraph::MarkSideEffect(uint32_t pass_index) {
    if (!InvalidateCompile()) {
        return;
    }
    if (pass_index >= passes.size()) {
        declaration_errors.emplace_back("side-effect declaration has invalid pass index");
        return;
    }
    passes[pass_index].side_effect = true;
}

bool RenderGraph::InvalidateCompile() {
    if (executed) {
        declaration_errors.emplace_back("graph declarations cannot be mutated after execution");
        return false;
    }
    compiled = false;
    compile_error.clear();
    execution_order.clear();
    compiled_edges.clear();
    for (auto& resource : resources) {
        resource.first_use = PassHandle::InvalidIndex;
        resource.last_use  = PassHandle::InvalidIndex;
    }
    return true;
}

bool RenderGraph::FailCompile(std::string message) {
    compile_error = std::move(message);
    compiled      = false;
    return false;
}

bool RenderGraph::IsValidResource(ResourceHandle resource) const {
    return resource.IsValid() && resource.owner_id == graph_id && resource.index < resources.size();
}

bool RenderGraph::IsValidPass(PassHandle pass) const {
    return pass.IsValid() && pass.owner_id == graph_id && pass.index < passes.size();
}

} // namespace Moer::Render
