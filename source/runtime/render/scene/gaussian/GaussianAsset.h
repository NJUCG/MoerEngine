#pragma once

#include "RenderAPI.h"
#include "misc/BoundingBox.h"
#include "shaderheaders/shared/gaussian/GaussianParameters.h"
#include "GaussianRenderSettings.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Moer {

// Immutable after publication. Instances and in-flight frames share its lifetime.
struct GaussianAsset {
    std::filesystem::path          source_path;
    uint32                         sh_degree = 0;
    Box3D                          bounds;
    std::vector<Render::GGaussian> gaussians;
};

using GaussianAssetRef = std::shared_ptr<const GaussianAsset>;

struct GaussianLoadResult {
    GaussianAssetRef asset;
    std::string      error_message;
    explicit         operator bool() const {
        return asset != nullptr;
    }
};

struct GaussianInstanceSettings {
    float3 color_multiplier   = float3(1.f);
    float  opacity_multiplier = 1.f;
    bool   is_enabled         = true;
    bool   cast_shadows       = false;
    bool   receive_shadows    = false;
};

struct GaussianInstanceSnapshot {
    uint64                   key = 0;
    GaussianAssetRef         asset;
    float4x4                 world_transform = float4x4::Identity();
    Box3D                    world_bounds;
    GaussianInstanceSettings settings;
    bool                     is_visible_in_game   = true;
    bool                     is_visible_in_editor = true;
};

enum class EGaussianSceneContent {
    Empty,
    MeshOnly,
    GaussianOnly,
    Mixed
};

struct GaussianSceneSnapshot {
    uint64                                revision = 0;
    std::vector<GaussianInstanceSnapshot> instances;
    Box3D                                 bounds;
};

} // namespace Moer
