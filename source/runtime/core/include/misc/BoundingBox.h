#pragma once

#include "math/Function.h"
#include "misc/Traits.h"

namespace Moer {

struct Box3D {
    float3 min;
    float3 max;

    Box3D() noexcept : min(float3(MAX_FLOAT)), max(float3(MIN_FLOAT)) {}
    Box3D(const float3& _min, const float3& _max) noexcept : min(_min), max(_max) {}

    float3 GetCenter() const noexcept {
        return (min + max) * 0.5f;
    }
    float3 GetExtent() const noexcept {
        return max - min;
    }
    bool IsValid() const noexcept {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    void Expand(const float3& _point) noexcept {
        min = Min(min, _point);
        max = Max(max, _point);
    }
    void Expand(const Box3D& _box) noexcept {
        min = Min(min, _box.min);
        max = Max(max, _box.max);
    }
};

} // namespace Moer