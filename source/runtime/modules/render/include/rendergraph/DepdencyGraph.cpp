#include "DepdencyGraph.h"

#include <stack>
namespace Moer {
    void DepdencyGraph::Link(DepdencyGraph::Edge* edge) {
    }
    void DepdencyGraph::RegisterNode(Node* node) {
    }
    void DepdencyGraph::Cull() {
        auto & nodes = m_nodes;
        auto & edges = m_edges;

        for(auto & edge : edges) {
            edge->src->AddRef();
        }

        
        auto stack = std::stack<Node*>();
        for(auto & node : nodes) {
            if(node->IsCulled()) {
                stack.emplace(node);
            }
        }
        while(!stack.empty()) {
            auto node = stack.top();
            stack.pop();

            auto in_coming_edges = GetInComingEdges(node);
            for(auto & edge : in_coming_edges) {
                edge->src->DecRef();
                if(edge->src->IsCulled()) {
                    stack.emplace(edge->src);
                }
            }
        }
        
    }
}