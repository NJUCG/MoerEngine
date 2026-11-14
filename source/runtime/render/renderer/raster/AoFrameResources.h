#pragma once

#include "RasterTextures.h"

#include <array>

namespace Moer::Render::Raster {

// Selected once for a frame. The final pair has stable physical identities for
// temporal sampling and CUDA imports; write_index advances only on acceptance.
struct AoFrameResources {
    std::array<TextureWithHandle, 2> final_ao;
    std::array<TextureWithHandle, 2> geometry;
    TextureWithHandle                noisy_ao;
    TextureWithHandle                camera_motion;
    TextureWithHandle                indirect;
    uint32                           write_index{};
    bool                             half_resolution{};
    bool                             denoise{};
    bool                             has_previous_output{};
    bool                             has_denoised_history{};

    const TextureWithHandle& Output() const {
        return final_ao[write_index];
    }
    const TextureWithHandle& PreviousOutput() const {
        return final_ao[write_index ^ 1u];
    }
    const TextureWithHandle& EvaluationOutput() const {
        return denoise ? noisy_ao : Output();
    }
    const TextureWithHandle& CurrentGeometry() const {
        return geometry[write_index];
    }
    const TextureWithHandle& PreviousGeometry() const {
        return geometry[write_index ^ 1u];
    }
};

} // namespace Moer::Render::Raster
