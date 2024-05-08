#include "rendergraph/DepdencyGraph.h"

#include "log/LogSystem.h"

#include <stack>
namespace Moer {
    void DepdencyGraph::Link(DepdencyGraph::Edge* _edge) {
        m_edges.emplace_back(_edge);
    }
    void DepdencyGraph::RegisterNode(Node* _node) {
        m_nodes.emplace_back(_node);
    }
    void DepdencyGraph::Cull() {
        auto& nodes = m_nodes;
        auto& edges = m_edges;

        for (auto& edge : edges) {
            edge->src->AddRef();
        }

        auto stack = std::stack<Node*>();
        for (auto& node : nodes) {
            if (node->IsCulled()) {
                stack.emplace(node);
            }
        }
        while (!stack.empty()) {
            auto node = stack.top();
            stack.pop();

            LOG_INFO("Culling node: {}", node->GetName());

            auto in_coming_edges = GetInComingEdges(node);
            for (auto& edge : in_coming_edges) {
                edge->src->DeRef();
                if (edge->src->IsCulled()) {
                    stack.emplace(edge->src);
                }
            }
        }
    }
    DepdencyGraph::EdgeContainer DepdencyGraph::GetInComingEdges(Node const* node) const {
        EdgeContainer in_coming_edges;
        for (auto& edge : m_edges) {
            if (edge->dst == node) {
                in_coming_edges.emplace_back(edge);
            }
        }
        return in_coming_edges;
    }
    DepdencyGraph::EdgeContainer DepdencyGraph::GetOutGoingEdges(Node const* node) const {
        EdgeContainer out_going_edges;
        for (auto& edge : m_edges) {
            if (edge->src == node) {
                out_going_edges.emplace_back(edge);
            }
        }
        return out_going_edges;
    }
    DepdencyGraph::EdgeContainer DepdencyGraph::getEdges(Node const* node) const {
        EdgeContainer edges;
        for (auto& edge : m_edges) {
            if (edge->src == node || edge->dst == node) {
                edges.emplace_back(edge);
            }
        }
        return edges;
    }
    DepdencyGraph::NodeContainer DepdencyGraph::GetInComingNodes(Node const* node) const {
        NodeContainer in_coming_nodes;
        for (auto& edge : m_edges) {
            if (edge->dst == node) {
                in_coming_nodes.emplace_back(edge->src);
            }
        }
        return in_coming_nodes;
    }
    DepdencyGraph::NodeContainer DepdencyGraph::GetOutGoingNodes(Node const* node) const {
        NodeContainer out_coming_nodes;
        for (auto& edge : m_edges) {
            if (edge->src == node) {
                out_coming_nodes.emplace_back(edge->dst);
            }
        }
        return out_coming_nodes;
    }
    bool DepdencyGraph::IsWriteResource(Node* pass_node, Node* resource_node) const {
        for (auto& edge : m_edges) {
            if (edge->src == pass_node && edge->dst == resource_node) {
                return true;
            }
        }
        return false;
    }
    bool DepdencyGraph::IsReadResource(Node* pass_node, Node* resource_node) const {
        for (auto& edge : m_edges) {
            if (edge->dst == pass_node && edge->src == resource_node) {
                return true;
            }
        }
        return false;
    }
    DepdencyGraph::~DepdencyGraph() {
        for (auto& edge : m_edges) {
            MoerDelete(edge);
        }
    }

    void DepdencyGraph::Reset() {
        for (auto& edge : m_edges) {
            MoerDelete(edge);
        }
        m_edges.clear();
        m_nodes.clear();
    }
}// namespace Moer
