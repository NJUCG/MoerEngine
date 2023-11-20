#include "math/Transform.h"
#include "math/Function.h"
#include <assert.h>

namespace {
    // from Piccolo
    Moer::Matrix4x4f AffineInverse(const Moer::Matrix4x4f& m) noexcept {
        float m10 = m[1][0], m11 = m[1][1], m12 = m[1][2];
        float m20 = m[2][0], m21 = m[2][1], m22 = m[2][2];

        float t00 = m22 * m11 - m21 * m12;
        float t10 = m20 * m12 - m22 * m10;
        float t20 = m21 * m10 - m20 * m11;

        float m00 = m[0][0], m01 = m[0][1], m02 = m[0][2];

        float inv_det = 1.f / (m00 * t00 + m01 * t10 + m02 * t20);

        t00 *= inv_det;
        t10 *= inv_det;
        t20 *= inv_det;

        m00 *= inv_det;
        m01 *= inv_det;
        m02 *= inv_det;

        float r00 = t00;
        float r01 = m02 * m21 - m01 * m22;
        float r02 = m01 * m12 - m02 * m11;

        float r10 = t10;
        float r11 = m00 * m22 - m02 * m20;
        float r12 = m02 * m10 - m00 * m12;

        float r20 = t20;
        float r21 = m01 * m20 - m00 * m21;
        float r22 = m00 * m11 - m01 * m10;

        float m03 = m[0][3], m13 = m[1][3], m23 = m[2][3];

        float r03 = -(r00 * m03 + r01 * m13 + r02 * m23);
        float r13 = -(r10 * m03 + r11 * m13 + r12 * m23);
        float r23 = -(r20 * m03 + r21 * m13 + r22 * m23);

        return Moer::Matrix4x4f(r00, r01, r02, r03, r10, r11, r12, r13, r20, r21, r22, r23, 0.f, 0.f, 0.f, 1.f);
    }
}// namespace

namespace Moer {
    Transform::Transform(const Matrix3x4f& matrix) noexcept {
        this->matrix = Matrix4x4f(matrix.r0, matrix.r1, matrix.r2, Vector4f(0.f, 0.f, 0.f, 1.f));
    }
    Transform::Transform(const Vector3f& translation, const Vector3f& scaling, const Quaternion& quaternion) noexcept {
        matrix = Moer::IDENTITY_4X4F;

        auto rotation_matrix = quaternion.GetRotation();

        matrix[0][0] = scaling.x * rotation_matrix[0][0];
        matrix[0][1] = scaling.y * rotation_matrix[0][1];
        matrix[0][2] = scaling.z * rotation_matrix[0][2];
        matrix[1][0] = scaling.x * rotation_matrix[1][0];
        matrix[1][1] = scaling.y * rotation_matrix[1][1];
        matrix[1][2] = scaling.z * rotation_matrix[1][2];
        matrix[2][0] = scaling.x * rotation_matrix[2][0];
        matrix[2][1] = scaling.y * rotation_matrix[2][1];
        matrix[2][2] = scaling.z * rotation_matrix[2][2];

        matrix[0][3] = translation.x;
        matrix[1][3] = translation.y;
        matrix[2][3] = translation.z;
    }

    Transform::Transform(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept {
        matrix = Moer::MakeLookatViewMatrixRH(origin, look_at, up_dir);
    }

    Transform::Transform(const Moer::Angle& fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept {
        matrix = Moer::MakePerspectiveMatrixRH(fov_y.GetRadian(), aspect_ratio, near_clip, far_clip);
    }

    void Transform::AppendTransformation(const Transform& new_transform) noexcept {
        matrix = new_transform.matrix * matrix;
    }
    Transform Transform::operator*(const Transform& rhs) const noexcept {
        Transform ret;
        ret.matrix = matrix * rhs.matrix;
        return ret;
    }

    Matrix3x4f Transform::GetMatrix3x4() const noexcept {
        return Matrix3x4f(matrix.r0, matrix.r1, matrix.r2);
    }

    Transform Transform::Inverse() const noexcept {
        Transform inv_trans;
        inv_trans.matrix = IsAffine() ? AffineInverse(matrix) : Moer::Inverse(matrix);
        return inv_trans;
    }

    bool Transform::IsAffine() const noexcept {
        return ApproxEqual(matrix.r3, Vector4f(0.f, 0.f, 0.f, 1.f), 0.0001f);
    }

    AffineTransformation Transform::AffineDecomposition() const noexcept {
        auto m3x3 = Moer::GetDiagonal3x3(matrix);

        Matrix3x3f Q;
        Vector3f   D;
        Vector3f   U;
        Moer::QDUDecomposition(m3x3, Q, D, U);

        AffineTransformation affine_trans;
        affine_trans.quaternion  = Quaternion(Q);
        affine_trans.scaling     = D;
        affine_trans.translation = Vector3f(matrix.GetColumn(3));

        return affine_trans;
    }

    Vector4f Transform::operator*(const Vector4f& v) const noexcept {
        return matrix * v;
    }
    Vector3f Transform::operator*(const Vector3f& v) const noexcept {
        auto  p     = Vector4f(v, 1.f);
        auto  tp    = matrix * p;
        float inv_w = 1.f / tp.w;
        return Vector3f(tp) * inv_w;
    }

}// namespace Moer