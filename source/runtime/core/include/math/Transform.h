#pragma once

#include "Base.h"
#include "Constant.h"
#include "Matrix.h"
#include "Quaternion.h"

namespace Moer {
/** Affine transformation
     * @details linear transformation(scaling + rotation) + translation
     * TransformVector(v) = matrix * [vx, vy, vz, 0]^T
     * TransformPoint(p)  = matrix * [px, py, pz, 1]^T
    */
struct AffineTransformation {
    Vector3f   translation;
    Vector3f   scaling;
    Quaternion quaternion;
};

struct CORE_API Transform {
    Matrix4x4f matrix;

    Transform() noexcept : matrix(IDENTITY_4X4F) {}
    Transform(const Matrix4x4f& matrix) noexcept : matrix(matrix) {}
    Transform(const Transform& trans) noexcept : matrix(trans.matrix) {}

    Transform(const Matrix3x4f& matrix) noexcept;
    Transform(const Vector3f& translation, const Vector3f& scaling, const Quaternion& quaternion) noexcept;
    Transform(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept;

    // perspective transformation
    Transform(const Angle& fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept;

    // this.AppendTransformation(new_transform) == new_transform * this * v => (new_transform * this) * v
    void      AppendTransformation(const Transform& new_transform) noexcept;
    Transform operator*(const Transform& rhs) const noexcept;

    Matrix4x4f GetMatrix4x4() const noexcept {
        return matrix;
    }
    Matrix3x4f GetMatrix3x4() const noexcept;
    Matrix3x4f GetMatrix3x4ColumnMajor() const noexcept;
    Transform  Inverse() const noexcept;

    bool                 IsAffine() const noexcept;
    AffineTransformation AffineDecomposition() const noexcept;

    // vector transformation
    Vector4f operator*(const Vector4f& v) const noexcept;
    /**
         * point transformation
         * @note 
         * the result will be divided by w
         */
    Vector3f operator*(const Vector3f& v) const noexcept;
};
} // namespace Moer