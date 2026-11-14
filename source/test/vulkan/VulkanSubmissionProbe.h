#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"

namespace Moer::Render {

// Narrow for-testing seam for the real multi-segment completion aggregate.
// It deliberately exposes only observable callback ordering/counts, not the
// backend-private aggregate or its synchronization storage.
struct VulkanMultiSegmentCompletionProbeResult {
    bool   suffix_retirement_deferred_callbacks{false};
    bool   prefix_retirement_completed_callbacks{false};
    bool   repeated_retirement_suppressed{false};
    uint32 ordinary_callback_count{0};
    uint32 success_callback_count{0};
    uint32 materialized_prefix_signal_count{0};
    uint32 materialized_prefix_keepalive_count{0};
    uint32 materialized_suffix_signal_count{0};
    uint32 materialized_suffix_keepalive_count{0};
    bool   materialized_signal_identity_matches{false};
    bool   source_signal_remained_unrejected{false};
};

[[nodiscard]] RENDER_API VulkanMultiSegmentCompletionProbeResult
RunVulkanMultiSegmentCompletionProbeForTesting();

} // namespace Moer::Render
