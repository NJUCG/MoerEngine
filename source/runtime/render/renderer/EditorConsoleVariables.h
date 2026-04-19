#pragma once

#include "RenderAPI.h"
#include "renderer/EditorConfig.h"

namespace Moer::Render::EditorConsoleVariables {

RENDER_API void CaptureFromEditorConfig(const EditorConfig& config);
RENDER_API void ApplyToEditorConfig(EditorConfig& config);

} // namespace Moer::Render::EditorConsoleVariables