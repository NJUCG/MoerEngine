#pragma once
#include "misc/Traits.h"
#include <cmath>
#include <string>
namespace Moer {
enum class EGaussianDisplayMode : uint {
    Reference,
    Engine
};
enum class EGaussianDebugView : uint {
    Color,
    Transmittance,
    TileCoverage
};
struct GaussianViewSettings {
    float3               background         = float3(0.f);
    float                exposure_ev        = 0.f;
    EGaussianDisplayMode display_mode       = EGaussianDisplayMode::Reference;
    EGaussianDebugView   debug_view         = EGaussianDebugView::Color;
    uint                 tile_pair_capacity = 4u * 1024u * 1024u;
};
inline bool IsValidGaussianViewSettings(const GaussianViewSettings& value) {
    return std::isfinite(value.background.x) && std::isfinite(value.background.y) &&
           std::isfinite(value.background.z) && value.background.x >= 0 && value.background.y >= 0 &&
           value.background.z >= 0 && value.background.x <= 1 && value.background.y <= 1 &&
           value.background.z <= 1 && std::isfinite(value.exposure_ev) && value.exposure_ev >= -16 &&
           value.exposure_ev <= 16 &&
           static_cast<uint>(value.display_mode) <= static_cast<uint>(EGaussianDisplayMode::Engine) &&
           static_cast<uint>(value.debug_view) <= static_cast<uint>(EGaussianDebugView::TileCoverage) &&
           value.tile_pair_capacity > 0 && value.tile_pair_capacity <= (1u << 24);
}
struct GaussianRasterStatus {
    bool   is_active             = false;
    bool   has_completed_counts  = false;
    bool   has_overflow          = false;
    bool   has_gpu_time          = false;
    uint   unique_gaussian_count = 0;
    uint   instance_count        = 0;
    uint   work_count            = 0;
    uint   visible_count         = 0;
    uint   tile_pair_count       = 0;
    uint64 scene_revision        = 0;
    uint64 geometry_bytes        = 0;
    uint64 workspace_bytes       = 0;
    double gpu_ms                = 0;
};
} // namespace Moer
