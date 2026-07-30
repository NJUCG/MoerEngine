#pragma once

#include "RenderAPI.h"
#include "window/WindowInput.h"

#include <cstdint>

namespace Moer::Render {

/**
 * Captures the Dear ImGui IO state after ImGui::NewFrame().
 *
 * The caller owns capture_sequence so independent UI backends never share a
 * mutable global generation. This function and the returned snapshot are
 * game-thread input state; render-thread packets must receive an explicit copy
 * if they ever need the data.
 */
[[nodiscard]] RENDER_API WindowInputSourceSnapshot
CaptureImGuiIOInput(uint64_t capture_sequence);

} // namespace Moer::Render
