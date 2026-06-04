#pragma once

#include "log/LogSystem.h"

#include <array>
#include <string>

#include <nfd.hpp>

namespace Moer {

enum class ESceneFileDialogResult {
    Selected,
    Cancelled,
    Error,
};

inline ESceneFileDialogResult OpenSceneFileDialog(std::string& out_selected_path) {
    NFD::UniquePath                      selected_path = nullptr;
    const std::array<nfdfilteritem_t, 5> filters       = {{
        {"All Support Formats", "glb,gltf,fbx,obj,dae"},
        {"glTF 2.0", "glb,gltf"},
        {"FBX", "fbx"},
        {"Wavefront", "obj"},
        {"Moer Renderer Scene (WIP)", "json"},
    }};

    const nfdresult_t result = NFD::OpenDialog(selected_path, filters.data(), filters.size());
    if (result == NFD_OKAY) {
        out_selected_path = selected_path.get();
        return ESceneFileDialogResult::Selected;
    }

    if (result == NFD_CANCEL) {
        return ESceneFileDialogResult::Cancelled;
    }

    const char* error_message = NFD_GetError();
    LOG_ERROR("NFD Error: {}", error_message ? error_message : "Unknown NFD error.");
    return ESceneFileDialogResult::Error;
}

} // namespace Moer