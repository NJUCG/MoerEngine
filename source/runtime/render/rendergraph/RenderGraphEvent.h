#pragma once

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

} // namespace Moer::Render::RenderGraph
