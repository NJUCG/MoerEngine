#pragma once

#include "misc/Types.h"
#include "scene/gaussian/GaussianRenderSettings.h"

#include <string>

namespace Moer {

// Latest completed GPU culling counters; readback may lag the frame carrying them.
struct CullingStats {
    uint32 total_instances_before     = 0;
    uint32 total_instances_after      = 0;
    uint32 visible_draws              = 0;
    uint32 total_draws                = 0;
    uint32 frustum_culled_instances   = 0;
    uint32 occlusion_culled_instances = 0;
    uint32 lod_culled_instances       = 0;

    float GetCullingRatio() const {
        if (total_instances_before == 0) {
            return 0.0f;
        }
        return 1.0f - static_cast<float>(total_instances_after) / static_cast<float>(total_instances_before);
    }

    float GetFrustumCullingRatio() const {
        if (total_instances_before == 0) {
            return 0.0f;
        }
        return static_cast<float>(frustum_culled_instances) / static_cast<float>(total_instances_before);
    }

    float GetOcclusionCullingRatio() const {
        if (total_instances_before == 0) {
            return 0.0f;
        }
        return static_cast<float>(occlusion_culled_instances) / static_cast<float>(total_instances_before);
    }

    float GetRenderedRatio() const {
        if (total_instances_before == 0) {
            return 0.0f;
        }
        return static_cast<float>(total_instances_after) / static_cast<float>(total_instances_before);
    }
};

struct CooperativeOpsStatus {
    bool extension_enabled                     = false;
    bool inference_ready                       = false;
    bool matrix_supported                      = false;
    bool matrix_robust_buffer_access_supported = false;
    bool vector_supported                      = false;
    bool vector_training_supported             = false;
    bool low_precision_supported               = false;
    bool storage_supported                     = false;
    bool vulkan_memory_model_supported         = false;

    uint32 matrix_mode_count       = 0;
    uint32 vector_mode_count       = 0;
    uint32 matrix_supported_stages = 0;
    uint32 vector_supported_stages = 0;
    uint32 max_vector_components   = 0;
    uint32 frames_evaluated        = 0;

    std::string overview              = "Waiting for cooperative snapshot...";
    std::string matrix_summary        = "Waiting for cooperative snapshot...";
    std::string matrix_runtime_status = "Idle";
    std::string vector_summary        = "Waiting for cooperative snapshot...";
    std::string vector_runtime_status = "Idle";
};

// Render output copied back to Game Thread state through the frame feedback.
struct RasterFrameStatus {
    GaussianRasterStatus gaussian_status{};
    CullingStats         culling_stats{};
    CooperativeOpsStatus cooperative_ops_status{};
};

} // namespace Moer
