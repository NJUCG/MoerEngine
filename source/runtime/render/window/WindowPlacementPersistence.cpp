#include "window/WindowPlacementPersistence.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace Moer::Render {

namespace {

constexpr std::int64_t k_window_state_version = 1;
constexpr int          k_min_window_width     = 320;
constexpr int          k_min_window_height    = 200;
constexpr int          k_max_window_extent    = 65535;
constexpr int          k_max_coordinate       = 1'000'000;

std::atomic_uint64_t g_temporary_file_nonce{1};

bool IsUsableFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
           std::filesystem::file_size(path, error) > 0 && !error;
}

bool IsSanePlacement(const WindowPlacement& placement) {
    return placement.width >= k_min_window_width && placement.height >= k_min_window_height &&
           placement.width <= k_max_window_extent && placement.height <= k_max_window_extent &&
           placement.x >= -k_max_coordinate && placement.x <= k_max_coordinate &&
           placement.y >= -k_max_coordinate && placement.y <= k_max_coordinate;
}

std::filesystem::path MakeTemporarySibling(const std::filesystem::path& target) {
    const std::uint64_t nonce     = g_temporary_file_nonce.fetch_add(1, std::memory_order_relaxed);
    const auto          timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const auto process_id = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process_id = static_cast<std::uint64_t>(getpid());
#endif

    std::filesystem::path temporary = target;
    temporary += ".tmp-" + std::to_string(process_id) + "-" +
                 std::to_string(static_cast<std::uint64_t>(timestamp)) + "-" + std::to_string(nonce);
    return temporary;
}

bool PublishTemporaryFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target,
    bool                         replace_existing
) {
#if defined(_WIN32)
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace_existing) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    return MoveFileExW(temporary.c_str(), target.c_str(), flags) != FALSE;
#else
    if (!replace_existing) {
        if (::link(temporary.c_str(), target.c_str()) != 0) {
            return false;
        }
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return true;
    }

    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return !error;
#endif
}

std::int64_t IntersectionArea(const WindowPlacement& placement, const MonitorWorkArea& area) {
    const std::int64_t left  = std::max<std::int64_t>(placement.x, area.x);
    const std::int64_t top   = std::max<std::int64_t>(placement.y, area.y);
    const std::int64_t right = std::min<std::int64_t>(
        static_cast<std::int64_t>(placement.x) + placement.width,
        static_cast<std::int64_t>(area.x) + area.width
    );
    const std::int64_t bottom = std::min<std::int64_t>(
        static_cast<std::int64_t>(placement.y) + placement.height,
        static_cast<std::int64_t>(area.y) + area.height
    );
    return right > left && bottom > top ? (right - left) * (bottom - top) : 0;
}

std::uint64_t CenterDistanceSquared(const WindowPlacement& placement, const MonitorWorkArea& area) {
    const std::int64_t placement_center_x = static_cast<std::int64_t>(placement.x) * 2 + placement.width;
    const std::int64_t placement_center_y = static_cast<std::int64_t>(placement.y) * 2 + placement.height;
    const std::int64_t area_center_x      = static_cast<std::int64_t>(area.x) * 2 + area.width;
    const std::int64_t area_center_y      = static_cast<std::int64_t>(area.y) * 2 + area.height;
    const std::int64_t dx                 = placement_center_x - area_center_x;
    const std::int64_t dy                 = placement_center_y - area_center_y;
    return static_cast<std::uint64_t>(dx * dx + dy * dy);
}

} // namespace

std::optional<WindowPlacement> LoadWindowPlacement(const std::filesystem::path& path) noexcept {
    if (!IsUsableFile(path)) {
        return std::nullopt;
    }

    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return std::nullopt;
        }
        const toml::table state = toml::parse(stream);
        if (state["version"].value<std::int64_t>() != std::optional<std::int64_t>(k_window_state_version)) {
            return std::nullopt;
        }

        const auto x         = state["x"].value<std::int64_t>();
        const auto y         = state["y"].value<std::int64_t>();
        const auto width     = state["width"].value<std::int64_t>();
        const auto height    = state["height"].value<std::int64_t>();
        const auto maximized = state["maximized"].value<bool>();
        if (!x || !y || !width || !height || !maximized) {
            return std::nullopt;
        }

        constexpr std::int64_t int_min = std::numeric_limits<int>::min();
        constexpr std::int64_t int_max = std::numeric_limits<int>::max();
        if (*x < int_min || *x > int_max || *y < int_min || *y > int_max || *width < int_min ||
            *width > int_max || *height < int_min || *height > int_max) {
            return std::nullopt;
        }

        const WindowPlacement placement{
            .x         = static_cast<int>(*x),
            .y         = static_cast<int>(*y),
            .width     = static_cast<int>(*width),
            .height    = static_cast<int>(*height),
            .maximized = *maximized,
        };
        return IsSanePlacement(placement) ? std::optional(placement) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool SaveWindowPlacement(const std::filesystem::path& path, const WindowPlacement& placement) noexcept {
    if (!IsSanePlacement(placement)) {
        return false;
    }

    const std::filesystem::path temporary = MakeTemporarySibling(path);
    try {
        std::error_code directory_error;
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            return false;
        }

        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                return false;
            }
            stream << "version = " << k_window_state_version << '\n'
                   << "x = " << placement.x << '\n'
                   << "y = " << placement.y << '\n'
                   << "width = " << placement.width << '\n'
                   << "height = " << placement.height << '\n'
                   << "maximized = " << (placement.maximized ? "true" : "false") << '\n';
            stream.flush();
            if (!stream) {
                stream.close();
                std::error_code cleanup_error;
                std::filesystem::remove(temporary, cleanup_error);
                return false;
            }
        }

        if (PublishTemporaryFile(temporary, path, true)) {
            return true;
        }
    } catch (...) {
    }

    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    return false;
}

std::optional<WindowPlacement> SanitizeWindowPlacement(
    const WindowPlacement&           placement,
    std::span<const MonitorWorkArea> monitor_work_areas
) noexcept {
    if (!IsSanePlacement(placement)) {
        return std::nullopt;
    }

    const MonitorWorkArea* selected_area     = nullptr;
    std::int64_t           selected_overlap  = -1;
    std::uint64_t          selected_distance = std::numeric_limits<std::uint64_t>::max();
    for (const MonitorWorkArea& area : monitor_work_areas) {
        if (area.width <= 0 || area.height <= 0) {
            continue;
        }

        const std::int64_t  overlap  = IntersectionArea(placement, area);
        const std::uint64_t distance = CenterDistanceSquared(placement, area);
        if (overlap > selected_overlap ||
            (overlap == selected_overlap && overlap == 0 && distance < selected_distance)) {
            selected_area     = &area;
            selected_overlap  = overlap;
            selected_distance = distance;
        }
    }
    if (selected_area == nullptr) {
        return std::nullopt;
    }

    WindowPlacement result = placement;
    result.width           = std::min(result.width, selected_area->width);
    result.height          = std::min(result.height, selected_area->height);

    const int max_x = selected_area->x + selected_area->width - result.width;
    const int max_y = selected_area->y + selected_area->height - result.height;
    if (selected_overlap == 0) {
        result.x = selected_area->x + (selected_area->width - result.width) / 2;
        result.y = selected_area->y + (selected_area->height - result.height) / 2;
    } else {
        result.x = std::clamp(result.x, selected_area->x, max_x);
        result.y = std::clamp(result.y, selected_area->y, max_y);
    }
    return result;
}

std::optional<std::filesystem::path> MigrateLegacyImGuiSettings(
    const std::filesystem::path&           target,
    std::span<const std::filesystem::path> legacy_candidates
) noexcept {
    if (IsUsableFile(target)) {
        return std::nullopt;
    }

    try {
        std::error_code directory_error;
        std::filesystem::create_directories(target.parent_path(), directory_error);
        if (directory_error) {
            return std::nullopt;
        }

        for (const std::filesystem::path& candidate : legacy_candidates) {
            std::error_code equivalence_error;
            if (std::filesystem::equivalent(candidate, target, equivalence_error) && !equivalence_error) {
                continue;
            }
            if (!IsUsableFile(candidate)) {
                continue;
            }

            const std::filesystem::path temporary = MakeTemporarySibling(target);
            std::error_code             copy_error;
            std::filesystem::copy_file(
                candidate, temporary, std::filesystem::copy_options::overwrite_existing, copy_error
            );
            if (copy_error) {
                std::filesystem::remove(temporary, copy_error);
                continue;
            }

            if (PublishTemporaryFile(temporary, target, false)) {
                return candidate;
            }

            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            if (IsUsableFile(target)) {
                return std::nullopt;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

} // namespace Moer::Render
