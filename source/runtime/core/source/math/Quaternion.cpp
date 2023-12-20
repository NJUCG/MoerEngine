#include "math/Quaternion.h"
#include "math/Function.h"
#include <cmath>

namespace {
    /**
     * Algorithm in Ken Shoemake's article in 1987 SIGGRAPH course notes
     * article "Quaternion Calculus and Fast Animation".
     */
    Moer::Vector4f QuaternionFromRotation(const Moer::Matrix3x3f& rotation) noexcept {
        Moer::Vector4f vec;

        float trace = rotation[0][0] + rotation[1][1] + rotation[2][2];
        float root;

        if (trace > 0.0) {
            // |w| > 1/2, may as well choose w > 1/2
            root  = std::sqrt(trace + 1.0f);// 2w
            vec.w = 0.5f * root;
            root  = 0.5f / root;// 1/(4w)
            vec.x = (rotation[2][1] - rotation[1][2]) * root;
            vec.y = (rotation[0][2] - rotation[2][0]) * root;
            vec.z = (rotation[1][0] - rotation[0][1]) * root;
        } else {
            // |w| <= 1/2
            constexpr size_t next[3] = {1, 2, 0};
            size_t           i       = 0;
            if (rotation[1][1] > rotation[0][0])
                i = 1;
            if (rotation[2][2] > rotation[i][i])
                i = 2;
            size_t j = next[i];
            size_t k = next[j];

            root   = std::sqrt(rotation[i][i] - rotation[j][j] - rotation[k][k] + 1.0f);
            vec[i] = 0.5f * root;
            root   = 0.5f / root;
            vec.w  = (rotation[k][j] - rotation[j][k]) * root;
            vec[j] = (rotation[j][i] + rotation[i][j]) * root;
            vec[k] = (rotation[k][i] + rotation[i][k]) * root;
        }

        return vec;
    }
}// namespace

namespace Moer {
    Quaternion::Quaternion(const Vector3f& axis, const Angle& angle) noexcept {
        auto half_angle = angle.GetRadian() * 0.5f;
        vec             = Vector4f(axis * std::sin(half_angle), std::cos(half_angle));
    }

    Quaternion::Quaternion(const Matrix3x3f& rotation) noexcept {
        vec = QuaternionFromRotation(rotation);
    }

    Quaternion::Quaternion(const Vector3f& x_axis, const Vector3f& y_axis, const Vector3f& z_axis) noexcept {
        /**
         * rotation matrix:
         *  x_axis.x y_axis.x z_axis.x
         *  x_axis.y y_axis.y z_axis.y
         *  x_axis.z y_axis.z z_axis.z
        */
        Matrix3x3f rotation(x_axis.x, y_axis.x, z_axis.x, x_axis.y, y_axis.y, z_axis.y, x_axis.z, y_axis.z, z_axis.z);
        vec = QuaternionFromRotation(rotation);
    }

    Quaternion Quaternion::Inverse() const noexcept {
        return Quaternion(-vec.x, -vec.y, -vec.z, vec.w);
    }

    Matrix3x3f Quaternion::GetRotation() const noexcept {
        float a = vec.w;
        float b = vec.x;
        float c = vec.y;
        float d = vec.z;
        // clang-format off
        return Matrix3x3f(1.f - 2.f * c * c - 2.f * d * d, 2.f * b * c - 2.f * a * d, 2.f * a * c + 2.f * b * d,
                          2.f * b * c + 2.f * a * d, 1.f - 2.f * b * b - 2.f * d * d, 2.f * c * d - 2.f * a * b,
                          2.f * b * d - 2.f * a * c, 2.f * a * b + 2.f * c * d, 1.f - 2.f * b * b - 2.f * c * c);
        // clang-format on
    }

    void Quaternion::GetLocalAxes(Vector3f& x_axis, Vector3f& y_axis, Vector3f& z_axis) const noexcept {
        auto rotation = GetRotation();
        x_axis        = rotation.GetColumn(0);
        y_axis        = rotation.GetColumn(1);
        z_axis        = rotation.GetColumn(2);
    }

    std::pair<Vector3f, Angle> Quaternion::GetAxisAngle() const noexcept {
        Angle    angle = std::acos(vec.w) * 2.f;
        Vector3f axis  = Vector3f(vec) / std::sin(angle * 0.5f);
        return {axis, angle};
    }

    Quaternion Quaternion::operator+() const noexcept {
        return Quaternion(vec);
    }
    Quaternion Quaternion::operator-() const noexcept {
        return Quaternion(-vec);
    }

    Quaternion Quaternion::operator+(const Quaternion& p) const noexcept {
        return Quaternion(vec + p.vec);
    }
    Quaternion Quaternion::operator-(const Quaternion& p) const noexcept {
        return Quaternion(vec - p.vec);
    }

    // /** for two quaternions: q = [s, vx, vy, vz], p = [t, ux, uy, uz]
    //  * v = [vx, vy, vz], u = [ux, uy, uz]
    //  * q * p = [st - vu, su + tv + cross(v, u)]
    // */
    // Quaternion Quaternion::operator*(const Quaternion& p) const noexcept {
    //     float s = vec.w;
    //     float t = p.vec.w;
    //     auto  v = Vector3f(vec);
    //     auto  u = Vector3f(p.vec);
    //     return Quaternion(Vector4f(s * u + t * v + Cross(v, u), s * t - Dotf(v, u)));
    // }

    Quaternion Quaternion::operator*(const Quaternion& p) const noexcept {
        return Quaternion(
            vec.x * p.vec.w + vec.w * p.vec.x - vec.z * p.vec.y + vec.y * p.vec.z,
            vec.y * p.vec.w + vec.z * p.vec.x + vec.w * p.vec.y - vec.x * p.vec.z,
            vec.z * p.vec.w - vec.y * p.vec.x + vec.x * p.vec.y + vec.w * p.vec.z,
            vec.w * p.vec.w - vec.x * p.vec.x - vec.y * p.vec.y - vec.z * p.vec.z);
    }

    Quaternion Quaternion::operator/(float scalar) const noexcept {
        return Quaternion(vec / scalar);
    }
    Quaternion Quaternion::operator*(float scalar) const noexcept {
        return Quaternion(scalar * vec);
    }
    Quaternion operator*(float scalar, const Quaternion& rhs) noexcept {
        return Quaternion(scalar / rhs.vec);
    }

    Vector3f Quaternion::Rotate(const Vector3f& v) const noexcept {
        // Quaternion v_quat(v.x, v.y, v.z, 0.f);
        // return Vector3f(((*this) * v_quat * Inverse()).vec);

        // nVidia SDK implementation
        Vector3f uv, uuv;
        uv  = Cross(Vector3f(vec), v);
        uuv = Cross(Vector3f(vec), uv);
        uv *= (2.0f * vec.w);
        uuv *= 2.0f;

        return v + uv + uuv;
    }

    Quaternion Quaternion::SLerp(const Quaternion& p, const Quaternion& q, float t) noexcept {
        float cos_theta = Dotf(p.vec, q.vec);
        if (ApproxEqual(Abs(cos_theta), 1.f, 0.001f))
            return NLerp(p, q, t);

        float sin_theta = sqrtf(1 - cos_theta * cos_theta);
        float radian    = std::atan2(sin_theta, cos_theta);
        float inv_sin   = 1.f / sin_theta;
        float coeff0    = std::sin((1.f - t) * radian) * inv_sin;
        float coeff1    = std::sin(t * radian) * inv_sin;

        auto ret = coeff0 * p + coeff1 * (cos_theta < 0.f ? -q : q);
        // ret.vec  = Normalizef(ret.vec);
        return ret;
    }

    Quaternion Quaternion::NLerp(const Quaternion& p, const Quaternion& q, float t) noexcept {
        Quaternion ret;
        ret     = p + t * ((Dotf(p.vec, q.vec) < 0.f ? -q : q) - p);
        ret.vec = Normalizef(ret.vec);
        return ret;
    }
}// namespace Moer