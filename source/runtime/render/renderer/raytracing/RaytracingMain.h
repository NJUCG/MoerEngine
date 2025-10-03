#pragma once

#include "common/EditorAssets.h"
#include "misc/MMemory.h"
#include "ui/EditorUI.h"

namespace Moer::Render::Raytracing {

void RaytracingMain(SharedPtr<EditorUI> _editor_ui, EditorAssets& _editor_assets);

} // namespace Moer::Render::Raytracing