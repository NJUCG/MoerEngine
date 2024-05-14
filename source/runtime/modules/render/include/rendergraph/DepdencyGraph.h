#pragma once
// #include "RenderGraph.h"
#include "RenderGraphHandle.h"
#include "misc/STL.h"
#include "misc/CountableRef.h"
#include "rhi/RHICommon.h"
#include <variant>
namespace Moer {

    class DepdencyGraph {
    public:
        class Node {
        protected:
            uint32_t    refcount{0};
            std::string name;

        public:
            Node(std::string_view name) : name(name) {}
            Node() = default;
            void Destroy() {}
            COUNTABLE_IMPLEMENTATION
            bool               IsCulled() const { return GetRefCount() == 0; }
            const std::string_view GetName() const { return name; }
            virtual ~Node() = default;
        };
        using NodeId = Node*;
        struct TextureSubDesc {
            uint32_t           mip_level : 16   = 0;
            uint32_t           num_mips : 16    = 1;
            uint32_t           array_index : 16 = 0;
            uint32_t           array_count : 16 = 1;
            ETextureStateFlags usage;
        };
        struct BufferSubDesc {
            uint32_t      offset = 0;
            uint32_t      size   = 0;
            EBufferLayout layout;
        };
        using ResourceDesc = std::variant<TextureSubDesc, BufferSubDesc>;

        struct Edge {
            // may be texture usage or buffer usage
            NodeId       src;
            NodeId       dst;
            ResourceDesc desc;
            Edge(DepdencyGraph& graph, NodeId src, NodeId dst, ResourceDesc _desc) : src(src), dst(dst), desc(_desc) {
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
        EdgeContainer GetEdges(Node const* node) const;
        NodeContainer GetInComingNodes(Node const* node) const;
        NodeContainer GetOutGoingNodes(Node const* node) const;
        bool          IsWriteResource(Node* pass_node, Node* resource_node) const;
        bool          IsReadResource(Node* pass_node, Node* resource_node) const;
        ~DepdencyGraph();
        void          Reset();
        NodeContainer m_nodes;
        EdgeContainer m_edges;
    };
    inline bool operator==(DepdencyGraph::ResourceDesc const& _lhs, DepdencyGraph::ResourceDesc const& _rhs) {
        return _lhs.index() == _rhs.index() && std::memcmp(&_lhs, &_rhs, sizeof(DepdencyGraph::ResourceDesc)) == 0;
    }

}// namespace Moer

namespace std {
    template<>
    struct hash<Moer::DepdencyGraph::ResourceDesc> {
        std::size_t operator()(Moer::DepdencyGraph::ResourceDesc const& _desc) const noexcept {
            using namespace Moer;

            if (_desc.index() == 0) {
                auto& desc = std::get<DepdencyGraph::TextureSubDesc>(_desc);
                return desc.mip_level | desc.num_mips << 8 | desc.array_index << 16 | desc.array_count << 24;
            }
            auto& desc = std::get<DepdencyGraph::BufferSubDesc>(_desc);
            return desc.offset | desc.size << 8 | desc.layout << 16;
        }
    };
}// namespace std