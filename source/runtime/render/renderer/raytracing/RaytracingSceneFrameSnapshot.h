#pragma once

#include "misc/STL.h"
#include "misc/Traits.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"

namespace Moer {
class Scene;
}

namespace Moer::Render::Raytracing {

enum class EAnalyticLightClass : uint {
    Finite,
    Infinite,
    Environment
};

struct EmissivePrimitiveFrameInput {
    uint64 stable_key         = 0;
    uint   primitive_id       = 0;
    uint   num_triangles      = 0;
    uint   index_start_idx    = 0;
    uint   first_instance_idx = 0;
};

struct AnalyticLightFrameInput {
    uint64               stable_key = 0;
    EAnalyticLightClass  light_class{EAnalyticLightClass::Finite};
    PolymorphicLightInfo light_info{};
};

struct RaytracingSceneFrameSnapshot {
    uint primitive_count         = 0;
    uint light_count             = 0;
    uint emissive_instance_count = 0;
    uint emissive_triangle_count = 0;

    Array<EmissivePrimitiveFrameInput> emissive_primitives;
    Array<AnalyticLightFrameInput>     analytic_lights;
};

RaytracingSceneFrameSnapshot CaptureRaytracingSceneFrameSnapshot(const Scene& scene);

} // namespace Moer::Render::Raytracing
