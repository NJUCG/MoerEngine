#pragma once

#include "RenderGraphAllocator.h"
#include "RenderGraphDefinitions.h"
#include "rhi/RHICommand.h"

namespace Moer::Render::RenderGraph {

class FRDGEventScope {
public:
    RENDER_API
    FRDGEventScope(CommandList& InCmdList, const FRDGEventName& InName, bool bInTimeScope = false) :
        CmdList(&InCmdList),
        bTimeScope(bInTimeScope) {
        const char* name = InName.GetCStr();
        if (name && name[0] != '\0') {
            bActive = true;
            if (bTimeScope) {
                CmdList->PushScopeWithTimeScope(name);
            } else {
                CmdList->PushScope(name);
            }
        }
    }

    FRDGEventScope(const FRDGEventScope&)            = delete;
    FRDGEventScope& operator=(const FRDGEventScope&) = delete;

    RENDER_API ~FRDGEventScope() {
        if (!bActive || !CmdList) {
            return;
        }
        if (bTimeScope) {
            CmdList->PopScopeWithTimeScope();
        } else {
            CmdList->PopScope();
        }
    }

private:
    CommandList* CmdList    = nullptr;
    bool         bTimeScope = false;
    bool         bActive    = false;
};

/// Simplified scope state base class for FRDGBuilder.
/// Holds the graph-lifetime allocators and command list reference.
/// Complex UE scope/CSV-stat logic is removed; event emission is always enabled.
class FRDGScopeState {
protected:
    struct FState {
        bool const bImmediate;
        bool const bParallelExecute;

        FState(bool bInImmediate, bool bInParallelExecute)
            : bImmediate(bInImmediate)
            , bParallelExecute(bInParallelExecute) {}

    } ScopeState;

    /// Linear arenas that back the allocators (owned by this object).
    FLinearArena RootArena;
    FLinearArena TaskArena;
    FLinearArena TransitionArena;

    struct {
        /// Allocator for all root graph allocations on the graph builder thread.
        FRDGAllocator Root;

        /// Allocator for async pass and parallel execute setup.
        FRDGAllocator Task;

        /// Allocator for all allocations related to states / transitions.
        FRDGAllocator Transition;

        int32_t GetByteCount() const {
            return 0; // TODO: implement if profiling needed
        }

    } Allocators;

public:
    /// The RHI command list used for the render graph.
    CommandList& RHICmdList;

public:
    FRDGScopeState(CommandList& InRHICmdList, bool bImmediate, bool bParallelExecute)
        : ScopeState(bImmediate, bParallelExecute)
        , Allocators{FRDGAllocator(RootArena), FRDGAllocator(TaskArena), FRDGAllocator(TransitionArena)}
        , RHICmdList(InRHICmdList) {}

    /// Whether scope events (debug markers) should be emitted. Always true in this simplified version.
    bool ShouldEmitEvents() const { return true; }
};

} // namespace Moer::Render::RenderGraph
