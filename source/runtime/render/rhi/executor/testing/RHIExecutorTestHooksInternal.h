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
// immutable while installed. Successful removal waits for every callback that
// already acquired the override, so the caller may then destroy both override
// and context safely.
[[nodiscard]] RENDER_API bool TryInstallRHIDirectCommandListMaterializationOverrideForTesting(
    const RHIDirectCommandListMaterializationOverrideForTesting* _override
) noexcept;
[[nodiscard]] RENDER_API bool RemoveRHIDirectCommandListMaterializationOverrideForTesting(
    const RHIDirectCommandListMaterializationOverrideForTesting* _override
) noexcept;

// Runtime call site; the installed override is optional.
void InvokeDirectCommandListMaterializationOverrideForTesting(size_t _source_index);

} // namespace Moer::Render
