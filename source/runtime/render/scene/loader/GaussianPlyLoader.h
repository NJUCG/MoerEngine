#pragma once

#include "scene/gaussian/GaussianAsset.h"

namespace Moer {

// Strict standard trained 3DGS PLY: ASCII / binary little-endian, SH degree 0..3.
// Errors return no asset; unknown scalar fields are consumed without changing semantics.
RENDER_API GaussianLoadResult LoadGaussianPly(const std::filesystem::path& path);

} // namespace Moer
