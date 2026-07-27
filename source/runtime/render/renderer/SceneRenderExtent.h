#pragma once

#include "misc/Traits.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Moer::Render {

// Raster bloom currently owns six explicit mip levels. Keeping the native
// scene extent at least 32 pixels makes that fixed chain valid while a docked
// SceneColor viewport is being collapsed or resized.
inline constexpr uint k_min_scene_render_extent = 32u;
inline constexpr uint8_t k_scene_render_extent_stable_observations = 2u;

struct SceneRenderExtentRequest {
    uint2 extent{};
    bool  valid     = false;
    bool  immediate = false;
};

[[nodiscard]] inline bool IsValidRenderExtent(uint2 extent) noexcept {
    return extent.x > 0u && extent.y > 0u;
}

[[nodiscard]] inline bool EqualRenderExtent(uint2 lhs, uint2 rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

[[nodiscard]] inline uint2 NormalizeSceneRenderExtent(uint2 extent) noexcept {
    return uint2(
        std::max(k_min_scene_render_extent, extent.x),
        std::max(k_min_scene_render_extent, extent.y)
    );
}

[[nodiscard]] inline SceneRenderExtentRequest CaptureSceneRenderExtentRequest(
    bool   ui_composition_enabled,
    float2 scene_color_resolution,
    uint2  window_extent
) noexcept {
    if (!ui_composition_enabled) {
        if (!IsValidRenderExtent(window_extent)) {
            return {};
        }
        return SceneRenderExtentRequest{
            .extent    = NormalizeSceneRenderExtent(window_extent),
            .valid     = true,
            .immediate = true
        };
    }

    if (!std::isfinite(scene_color_resolution.x) || !std::isfinite(scene_color_resolution.y) ||
        scene_color_resolution.x <= 0.f || scene_color_resolution.y <= 0.f) {
        return {};
    }

    const auto quantize = [](float value) noexcept {
        const double bounded = std::min(
            static_cast<double>(value),
            static_cast<double>(std::numeric_limits<uint>::max())
        );
        return std::max(k_min_scene_render_extent, static_cast<uint>(bounded));
    };

    return SceneRenderExtentRequest{
        .extent    = uint2(quantize(scene_color_resolution.x), quantize(scene_color_resolution.y)),
        .valid     = true,
        .immediate = false
    };
}

// Render-thread-owned admission policy for UI-driven native render extents.
// Invalid (including 0x0) requests retain the last allocated extent. Docked UI
// changes must remain stable for two observations so a drag does not allocate
// a full render target set for every intermediate pixel. Headless/UI-disabled
// window extents are accepted immediately.
class SceneRenderExtentTracker {
public:
    explicit SceneRenderExtentTracker(uint2 initial_extent) noexcept :
        active_extent(
            NormalizeSceneRenderExtent(
                IsValidRenderExtent(initial_extent) ?
                    initial_extent :
                    uint2(k_min_scene_render_extent, k_min_scene_render_extent)
            )
        ) {}

    [[nodiscard]] bool Observe(SceneRenderExtentRequest request) noexcept {
        if (!request.valid || !IsValidRenderExtent(request.extent)) {
            ClearPending();
            return false;
        }

        request.extent = NormalizeSceneRenderExtent(request.extent);
        if (EqualRenderExtent(request.extent, active_extent)) {
            ClearPending();
            return false;
        }

        if (request.immediate) {
            active_extent = request.extent;
            ClearPending();
            return true;
        }

        if (!pending_valid || !EqualRenderExtent(request.extent, pending_extent)) {
            pending_extent       = request.extent;
            pending_observations = 1u;
            pending_valid        = true;
            return false;
        }

        pending_observations++;
        if (pending_observations < k_scene_render_extent_stable_observations) {
            return false;
        }

        active_extent = pending_extent;
        ClearPending();
        return true;
    }

    [[nodiscard]] uint2 GetActiveExtent() const noexcept {
        return active_extent;
    }

    [[nodiscard]] bool HasPendingExtent() const noexcept {
        return pending_valid;
    }

private:
    void ClearPending() noexcept {
        pending_extent       = uint2(0u, 0u);
        pending_observations = 0u;
        pending_valid        = false;
    }

    uint2   active_extent{};
    uint2   pending_extent{};
    uint8_t pending_observations = 0u;
    bool    pending_valid        = false;
};

} // namespace Moer::Render
