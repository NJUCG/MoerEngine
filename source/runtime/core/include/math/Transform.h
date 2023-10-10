#pragma once

#include "Base.h"
#include "Matrix.h"
#include "Quaternion.h"
#include "Constant.h"

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

    struct Transform {
        Matrix4x4f matrix;

        Transform() noexcept : matrix(Moer::IDENTITY_4X4F) {}
        Transform(const Matrix4x4f& matrix) noexcept : matrix(matrix) {}

        Transform(const Matrix3x4f& matrix) noexcept;
        Transform(const Vector3f& translation, const Vector3f& scaling, const Quaternion& quaternion) noexcept;
        Transform(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept;

        // this.AppendTransformation(new_transform) == new_transform * this * v => (new_transform * this) * v
        void AppendTransformation(const Transform& new_transform) noexcept;

        Matrix4x4f GetMatrix4x4() const noexcept { return matrix; }
        Matrix3x4f GetMatrix3x4() const noexcept;
        Transform  Inverse() const noexcept;

        bool                 IsAffine() const noexcept;
        AffineTransformation Decomposition() const noexcept;
    };
}// namespace Moer