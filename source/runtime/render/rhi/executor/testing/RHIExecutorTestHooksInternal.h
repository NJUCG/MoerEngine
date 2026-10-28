#pragma once

#include "RenderAPI.h"
#include <cstddef>

namespace Moer::Render {

using RHIDirectCommandListMaterializationCallbackForTesting = void (*)(void*, size_t);

struct RHIDirectCommandListMaterializationOverrideForTesting {
    void*                                                 context{nullptr};
    RHIDirectCommandListMaterializationCallbackForTesting before_source{nullptr};
};

// Narrow deterministic seam for exercising the mixed direct-submit rollback:
// the callback runs immediately before a non-empty CommandList is sealed. It
// may throw to emulate CommandList::Submit allocation failure, but must not
// re-enter RHI or remove its own override. Storage is caller-owned and
// immutable until successful removal finishes.
//
// Installation returns true only when this call installs the override. false
// means an invalid override, an existing installation, or removal in progress;
// it gives no callback-completion or storage-lifetime guarantee.
[[nodiscard]] RENDER_API bool TryInstallRHIDirectCommandListMaterializationOverrideForTesting(
    const RHIDirectCommandListMaterializationOverrideForTesting* _override
) noexcept;

// Removal returns true only when this call removes the matching override and
// waits for every callback that acquired it. The caller may then destroy the
// override and context. false means null, a different/no installed override, or
// removal already in progress; it does not guarantee that callbacks have ended
// or that either caller-owned object can be destroyed.
[[nodiscard]] RENDER_API bool RemoveRHIDirectCommandListMaterializationOverrideForTesting(
    const RHIDirectCommandListMaterializationOverrideForTesting* _override
) noexcept;

// Runtime call site; the installed override is optional.
void InvokeDirectCommandListMaterializationOverrideForTesting(size_t _source_index);

} // namespace Moer::Render
