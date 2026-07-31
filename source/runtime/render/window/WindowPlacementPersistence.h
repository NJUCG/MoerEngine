#pragma once

#include "RenderAPI.h"

#include <filesystem>
#include <optional>
#include <span>

namespace Moer::Render {

struct WindowPlacement {
    int  x         = 0;
    int  y         = 0;
    int  width     = 0;
    int  height    = 0;
    bool maximized = false;

    bool operator==(const WindowPlacement&) const = default;
};

struct MonitorWorkArea {
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
};

RENDER_API std::optional<WindowPlacement> LoadWindowPlacement(const std::filesystem::path& path) noexcept;

RENDER_API bool
SaveWindowPlacement(const std::filesystem::path& path, const WindowPlacement& placement) noexcept;

RENDER_API std::optional<WindowPlacement> SanitizeWindowPlacement(
    const WindowPlacement&           placement,
    std::span<const MonitorWorkArea> monitor_work_areas
) noexcept;

// Copies the first usable legacy file into target without overwriting either
// the target or any legacy file. The returned path identifies the migrated source.
RENDER_API std::optional<std::filesystem::path> MigrateLegacyImGuiSettings(
    const std::filesystem::path&           target,
    std::span<const std::filesystem::path> legacy_candidates
) noexcept;

} // namespace Moer::Render
