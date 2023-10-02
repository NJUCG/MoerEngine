#pragma once

#include <cmath>
#include <limits>

#include "Base.h"

namespace Moer {
    static constexpr int   MAX_INT   = std::numeric_limits<int>::max();
    static constexpr int   MIN_INT   = std::numeric_limits<int>::lowest();
    static constexpr float MAX_FLOAT = std::numeric_limits<float>::max();
    static constexpr float MIN_FLOAT = std::numeric_limits<float>::lowest();
    static constexpr float EPS       = 1e-6f;
    static constexpr float PI        = 3.14159265358979323846f;
    static constexpr float INV_PI    = 1.f / PI;
    static constexpr float TWO_PI    = 2.f * PI;
    static constexpr float HALF_PI   = 0.5f * PI;

    static const Vector2f ONE_2F = Vector2f{1.f};
    static const Vector3f ONE_3F = Vector3f{1.f};
    static const Vector4f ONE_4F = Vector4f{1.f};

    static const Vector2d ONE_2D = Vector2d{1.};
    static const Vector3d ONE_3D = Vector3d{1.};
    static const Vector4d ONE_4D = Vector4d{1.};

    static const Vector2i  ONE_2I  = Vector2i{1};
    static const Vector3i  ONE_3I  = Vector3i{1};
    static const Vector4i  ONE_4I  = Vector4i{1};
    static const Vector2ui ONE_2UI = Vector2ui{1u};
    static const Vector3ui ONE_3UI = Vector3ui{1u};
    static const Vector4ui ONE_4UI = Vector4ui{1u};
}// namespace Moer