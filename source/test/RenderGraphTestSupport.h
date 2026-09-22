#pragma once

#include "rendergraph/RenderGraph.h"

#include <functional>
#include <type_traits>
#include <utility>

namespace Moer::Render {

// Compiler tests often need a pass body only to make a declaration executable.
// Keep their no-argument fixtures out of the production RenderGraph API.
template<typename Callback>
RenderGraph::PassHandle AddTestRecordPass(
    RenderGraph& graph,
    std::string_view name,
    const RenderGraph::SetupCallback& setup,
    Callback&& callback
) {
    using CallbackType = std::decay_t<Callback>;
    if constexpr (std::is_invocable_v<CallbackType&, CommandList&>) {
        return graph.AddRecordPass(
            name, setup, std::forward<Callback>(callback)
        );
    } else {
        static_assert(std::is_invocable_v<CallbackType&>);
        return graph.AddRecordPass(
            name,
            setup,
            [callback = std::forward<Callback>(callback)](CommandList&) mutable {
                std::invoke(callback);
            }
        );
    }
}

inline bool RecordTestGraph(RenderGraph& graph) {
    return graph.ExecuteFrontendRecordingPlan({}, false);
}

} // namespace Moer::Render
