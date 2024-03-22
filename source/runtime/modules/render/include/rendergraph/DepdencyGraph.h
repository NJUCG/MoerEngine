#pragma once
// #include "RenderGraph.h"
#include "RenderGraphHandle.h"
#include "misc/STL.h"
namespace Moer {

    class DepdencyGraph {
    public:
        class Node {
        protected:
            uint32_t    refcount{0};
            std::string name;

        public:
            Node(const std::string& name) : name(name) {}
            Node() = default;
            bool               IsCulled() const { return refcount == 0; }
            void               AddRef() { refcount++; }
            uint32_t           GetRefCount() const { return refcount; }
            void               DecRef() { refcount--; }
            const std::string& GetName() const { return name; }
            virtual ~Node() = default;
        };
        using NodeId = Node*;
        struct Edge {
            // may be texture usage or buffer usage
            uint32_t usage{0};
            NodeId   src;
            NodeId   dst;
            Edge(DepdencyGraph& graph, NodeId src, NodeId dst, uint32_t usage) : usage(usage), src(src), dst(dst) {
                graph.Link(this);
            }
        };

        using NodeContainer = Moer::Array<Node*>;
        using EdgeContainer = Moer::Array<Edge*>;

        void          Link(DepdencyGraph::Edge* edge);
        void          RegisterNode(Node* node);
        void          Cull();
        EdgeContainer GetInComingEdges(Node const* node) const;
        EdgeContainer GetOutGoingEdges(Node const* node) const;
        EdgeContainer getEdges(Node const* node) const;
        NodeContainer GetInComingNodes(Node const* node) const;
        NodeContainer GetOutGoingNodes(Node const* node) const;
        bool          IsWriteResource(Node* pass_node, Node* resource_node) const;
        bool          IsReadResource(Node* pass_node, Node* resource_node) const;
        ~DepdencyGraph();
        void          Reset();
        NodeContainer m_nodes;
        EdgeContainer m_edges;
    };
}// namespace Moer