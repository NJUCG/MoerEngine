#pragma once

#include "misc/Traits.h"

namespace Moer::ecs {

enum class ECLightType : uint8 {
    None = 0,
    Directional,
    Point,
    Spot,
    Environment,
    Ambient,
};

} // namespace Moer::ecs