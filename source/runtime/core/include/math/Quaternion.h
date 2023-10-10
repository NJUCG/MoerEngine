#pragma once

#include "Base.h"
#include "Matrix.h"

#include <utility>

namespace Moer {
    /** 
     * @attention Quaternion vector must be normalized. Length(vec) must be 1.
     * @details math formula:
     * 
     * Rotation axis: Vector u = normalize([ux, uy, uz])
     * Rotation angle: \theta = angle / 180 * PI
     * Target vector: v = [0, vx, vy, vz]
     * Quaternion: q = [cos(0.5 * \theta), sin(0.5 * \theta) * u]
     * 
     * a = cos(0.5 * \theta)        -> vec.w
     * b = sin(0.5 * \theta)u_x     -> vec.x
     * c = sin(0.5 * \theta)u_y     -> vec.y
     * d = sin(0.5 * \theta)u_z     -> vec.z
     * 
     * calculation formula:
     * qvq^{-1} = [1   0           0           0
     *             0   1-2cc-2dd   2bc-2ad     2ac+2bd
     *             0   2bc+2ad     1-2bb-2dd   2cd-2ab
     *             0   2bd-2ac     2ab+2cd     1-2bb-2cc] * (v^T)
    */
    struct Quaternion {
        Vector4f vec;

        Quaternion() noexcept : vec(0.f, 0.f, 0.f, 1.f) {}
        Quaternion(float w, float x, float y, float z) noexcept : vec(x, y, z, w) {}

        explicit Quaternion(const Vector4f& q) noexcept : vec(q) {}
        explicit Quaternion(const Vector3f& axis, const Angle& angle) noexcept;
        explicit Quaternion(const Matrix3x3f& rotation) noexcept;
        // construct from local orthonormal coordinate system
        explicit Quaternion(const Vector3f& x_axis, const Vector3f& y_axis, const Vector3f& z_axis) noexcept;

        Matrix3x3f                 GetRotation() const noexcept;
        std::pair<Vector3f, Angle> GetAxisAngle() const noexcept;
        void                       GetLocalAxes(Vector3f& x_axis, Vector3f& y_axis, Vector3f& z_axis) const noexcept;

        Quaternion Inverse() const noexcept;
        Quaternion operator+() const noexcept;
        Quaternion operator-() const noexcept;
        Quaternion operator+(const Quaternion& p) const noexcept;
        Quaternion operator-(const Quaternion& p) const noexcept;

        // (q1 * q2).Rotate(v) == q1.Rotate(q2.Rotate(v))
        Quaternion operator*(const Quaternion& p) const noexcept;
        Vector3f   Rotate(const Vector3f& v) const noexcept;

        Quaternion        operator/(float scalar) const noexcept;
        Quaternion        operator*(float scalar) const noexcept;
        friend Quaternion operator*(float scalar, const Quaternion& rhs) noexcept;

        // Spherical Linear Interpolation
        static Quaternion SLerp(const Quaternion& p, const Quaternion& q, float t) noexcept;
        // Normalized Linear Interpolation
        static Quaternion NLerp(const Quaternion& p, const Quaternion& q, float t) noexcept;
    };
}// namespace Moer