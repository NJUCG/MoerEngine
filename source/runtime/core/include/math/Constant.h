#pragma once

#include <cmath>
#include <limits>

#include "Base.h"
#include "Matrix.h"
namespace Moer {
    static constexpr int   MAX_INT   = (std::numeric_limits<int>::max)();
    static constexpr int   MIN_INT   = std::numeric_limits<int>::lowest();
    static constexpr float MAX_FLOAT = (std::numeric_limits<float>::max)();
    static constexpr float MIN_FLOAT = std::numeric_limits<float>::lowest();
    static constexpr float EPS       = 1e-5f;
    static constexpr float PI        = 3.14159265358979323846f;
    static constexpr float INV_PI    = 1.f / PI;
    static constexpr float TWO_PI    = 2.f * PI;
    static constexpr float HALF_PI   = 0.5f * PI;

    static const Vector2f  ONE_2F  = Vector2f{1.f};
    static const Vector3f  ONE_3F  = Vector3f{1.f};
    static const Vector4f  ONE_4F  = Vector4f{1.f};
    static const Vector2d  ONE_2D  = Vector2d{1.};
    static const Vector3d  ONE_3D  = Vector3d{1.};
    static const Vector4d  ONE_4D  = Vector4d{1.};
    static const Vector2i  ONE_2I  = Vector2i{1};
    static const Vector3i  ONE_3I  = Vector3i{1};
    static const Vector4i  ONE_4I  = Vector4i{1};
    static const Vector2ui ONE_2UI = Vector2ui{1u};
    static const Vector3ui ONE_3UI = Vector3ui{1u};
    static const Vector4ui ONE_4UI = Vector4ui{1u};

    static const Matrix2x2f  IDENTITY_2X2F  = Matrix2x2f::Identity();
    static const Matrix3x3f  IDENTITY_3X3F  = Matrix3x3f::Identity();
    static const Matrix4x4f  IDENTITY_4X4F  = Matrix4x4f::Identity();
    static const Matrix2x2d  IDENTITY_2X2D  = Matrix2x2d::Identity();
    static const Matrix3x3d  IDENTITY_3X3D  = Matrix3x3d::Identity();
    static const Matrix4x4d  IDENTITY_4X4D  = Matrix4x4d::Identity();
    static const Matrix2x2i  IDENTITY_2X2I  = Matrix2x2i::Identity();
    static const Matrix3x3i  IDENTITY_3X3I  = Matrix3x3i::Identity();
    static const Matrix4x4i  IDENTITY_4X4I  = Matrix4x4i::Identity();
    static const Matrix2x2ui IDENTITY_2X2UI = Matrix2x2ui::Identity();
    static const Matrix3x3ui IDENTITY_3X3UI = Matrix3x3ui::Identity();
    static const Matrix4x4ui IDENTITY_4X4UI = Matrix4x4ui::Identity();
}// namespace Moer