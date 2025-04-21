#include "math/Function.h"
#include "math/Constant.h"

namespace {
    template<Moer::NumericType T>
    inline Moer::Matrix<T, 2, 2> Inverse(const Moer::Matrix<T, 2, 2>& m) noexcept {
        Moer::Matrix<T, 2, 2> ret;

        T inv_det = (T)1 / (m[0][0] * m[1][1] - m[0][1] * m[1][0]);
        ret[0][0] = m[1][1] * inv_det;
        ret[0][1] = -m[0][1] * inv_det;
        ret[1][0] = -m[1][0] * inv_det;
        ret[1][1] = m[0][0] * inv_det;

        return ret;
    }
    template<Moer::NumericType T>
    inline Moer::Matrix<T, 3, 3> Inverse(const Moer::Matrix<T, 3, 3>& m) noexcept {
        Moer::Matrix<T, 3, 3> ret;

        T cofactor00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
        T cofactor10 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
        T cofactor20 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
        T inv_det    = (T)1 / (m[0][0] * cofactor00 + m[0][1] * cofactor10 + m[0][2] * cofactor20);

        ret[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv_det;
        ret[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv_det;
        ret[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv_det;
        ret[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv_det;
        ret[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv_det;
        ret[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv_det;
        ret[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv_det;
        ret[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv_det;
        ret[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv_det;

        return ret;
    }
    template<Moer::NumericType T>
    inline Moer::Matrix<T, 4, 4> Inverse(const Moer::Matrix<T, 4, 4>& m) noexcept {
        Moer::Matrix<T, 4, 4> ret;

        T v0 = m[2][0] * m[3][1] - m[2][1] * m[3][0];
        T v1 = m[2][0] * m[3][2] - m[2][2] * m[3][0];
        T v2 = m[2][0] * m[3][3] - m[2][3] * m[3][0];
        T v3 = m[2][1] * m[3][2] - m[2][2] * m[3][1];
        T v4 = m[2][1] * m[3][3] - m[2][3] * m[3][1];
        T v5 = m[2][2] * m[3][3] - m[2][3] * m[3][2];

        T t00 = +(v5 * m[1][1] - v4 * m[1][2] + v3 * m[1][3]);
        T t10 = -(v5 * m[1][0] - v2 * m[1][2] + v1 * m[1][3]);
        T t20 = +(v4 * m[1][0] - v2 * m[1][1] + v0 * m[1][3]);
        T t30 = -(v3 * m[1][0] - v1 * m[1][1] + v0 * m[1][2]);

        T det     = t00 * m[0][0] + t10 * m[0][1] + t20 * m[0][2] + t30 * m[0][3];
        T inv_det = (T)1 / det;

        ret[0][0] = t00 * inv_det;
        ret[1][0] = t10 * inv_det;
        ret[2][0] = t20 * inv_det;
        ret[3][0] = t30 * inv_det;

        ret[0][1] = -(v5 * m[0][1] - v4 * m[0][2] + v3 * m[0][3]) * inv_det;
        ret[1][1] = +(v5 * m[0][0] - v2 * m[0][2] + v1 * m[0][3]) * inv_det;
        ret[2][1] = -(v4 * m[0][0] - v2 * m[0][1] + v0 * m[0][3]) * inv_det;
        ret[3][1] = +(v3 * m[0][0] - v1 * m[0][1] + v0 * m[0][2]) * inv_det;

        v0 = m[1][0] * m[3][1] - m[1][1] * m[3][0];
        v1 = m[1][0] * m[3][2] - m[1][2] * m[3][0];
        v2 = m[1][0] * m[3][3] - m[1][3] * m[3][0];
        v3 = m[1][1] * m[3][2] - m[1][2] * m[3][1];
        v4 = m[1][1] * m[3][3] - m[1][3] * m[3][1];
        v5 = m[1][2] * m[3][3] - m[1][3] * m[3][2];

        ret[0][2] = +(v5 * m[0][1] - v4 * m[0][2] + v3 * m[0][3]) * inv_det;
        ret[1][2] = -(v5 * m[0][0] - v2 * m[0][2] + v1 * m[0][3]) * inv_det;
        ret[2][2] = +(v4 * m[0][0] - v2 * m[0][1] + v0 * m[0][3]) * inv_det;
        ret[3][2] = -(v3 * m[0][0] - v1 * m[0][1] + v0 * m[0][2]) * inv_det;

        v0 = m[2][1] * m[1][0] - m[2][0] * m[1][1];
        v1 = m[2][2] * m[1][0] - m[2][0] * m[1][2];
        v2 = m[2][3] * m[1][0] - m[2][0] * m[1][3];
        v3 = m[2][2] * m[1][1] - m[2][1] * m[1][2];
        v4 = m[2][3] * m[1][1] - m[2][1] * m[1][3];
        v5 = m[2][3] * m[1][2] - m[2][2] * m[1][3];

        ret[0][3] = -(v5 * m[0][1] - v4 * m[0][2] + v3 * m[0][3]) * inv_det;
        ret[1][3] = +(v5 * m[0][0] - v2 * m[0][2] + v1 * m[0][3]) * inv_det;
        ret[2][3] = -(v4 * m[0][0] - v2 * m[0][1] + v0 * m[0][3]) * inv_det;
        ret[3][3] = +(v3 * m[0][0] - v1 * m[0][1] + v0 * m[0][2]) * inv_det;
        return ret;
    }

    template<Moer::NumericType T>
    void QDUDecomposition(const Moer::Matrix<T, 3, 3>& m, Moer::Matrix<T, 3, 3>& Q, Moer::Vector<T, 3>& D, Moer::Vector<T, 3>& U) noexcept {
        T inv_length = m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0];
        if (!Moer::ApproxEqual(inv_length, (T)0, std::numeric_limits<T>::epsilon()))
            inv_length = 1.f / std::sqrt(inv_length);

        Q[0][0] = m[0][0] * inv_length;
        Q[1][0] = m[1][0] * inv_length;
        Q[2][0] = m[2][0] * inv_length;

        T dot      = Q[0][0] * m[0][1] + Q[1][0] * m[1][1] + Q[2][0] * m[2][1];
        Q[0][1]    = m[0][1] - dot * Q[0][0];
        Q[1][1]    = m[1][1] - dot * Q[1][0];
        Q[2][1]    = m[2][1] - dot * Q[2][0];
        inv_length = Q[0][1] * Q[0][1] + Q[1][1] * Q[1][1] + Q[2][1] * Q[2][1];
        if (!Moer::ApproxEqual(inv_length, (T)0, std::numeric_limits<T>::epsilon()))
            inv_length = 1.f / std::sqrt(inv_length);

        Q[0][1] *= inv_length;
        Q[1][1] *= inv_length;
        Q[2][1] *= inv_length;

        dot     = Q[0][0] * m[0][2] + Q[1][0] * m[1][2] + Q[2][0] * m[2][2];
        Q[0][2] = m[0][2] - dot * Q[0][0];
        Q[1][2] = m[1][2] - dot * Q[1][0];
        Q[2][2] = m[2][2] - dot * Q[2][0];
        dot     = Q[0][1] * m[0][2] + Q[1][1] * m[1][2] + Q[2][1] * m[2][2];
        Q[0][2] -= dot * Q[0][1];
        Q[1][2] -= dot * Q[1][1];
        Q[2][2] -= dot * Q[2][1];
        inv_length = Q[0][2] * Q[0][2] + Q[1][2] * Q[1][2] + Q[2][2] * Q[2][2];
        if (!Moer::ApproxEqual(inv_length, (T)0, std::numeric_limits<T>::epsilon()))
            inv_length = 1.f / std::sqrt(inv_length);

        Q[0][2] *= inv_length;
        Q[1][2] *= inv_length;
        Q[2][2] *= inv_length;

        // guarantee that orthogonal matrix has determinant 1 (no reflections)
        T det = Q[0][0] * Q[1][1] * Q[2][2] + Q[0][1] * Q[1][2] * Q[2][0] +
                Q[0][2] * Q[1][0] * Q[2][1] - Q[0][2] * Q[1][1] * Q[2][0] -
                Q[0][1] * Q[1][0] * Q[2][2] - Q[0][0] * Q[1][2] * Q[2][1];

        if (det < 0.0) {
            for (size_t row_index = 0; row_index < 3; row_index++)
                for (size_t rol_index = 0; rol_index < 3; rol_index++)
                    Q[row_index][rol_index] = -Q[row_index][rol_index];
        }

        // build "right" matrix R
        Moer::Matrix<T, 3, 3> R;
        R[0][0] = Q[0][0] * m[0][0] + Q[1][0] * m[1][0] + Q[2][0] * m[2][0];
        R[0][1] = Q[0][0] * m[0][1] + Q[1][0] * m[1][1] + Q[2][0] * m[2][1];
        R[1][1] = Q[0][1] * m[0][1] + Q[1][1] * m[1][1] + Q[2][1] * m[2][1];
        R[0][2] = Q[0][0] * m[0][2] + Q[1][0] * m[1][2] + Q[2][0] * m[2][2];
        R[1][2] = Q[0][1] * m[0][2] + Q[1][1] * m[1][2] + Q[2][1] * m[2][2];
        R[2][2] = Q[0][2] * m[0][2] + Q[1][2] * m[1][2] + Q[2][2] * m[2][2];

        // the scaling component
        D[0] = R[0][0];
        D[1] = R[1][1];
        D[2] = R[2][2];

        // the shear component
        T inv_d0 = 1.0f / D[0];
        U[0]     = R[0][1] * inv_d0;
        U[1]     = R[0][2] * inv_d0;
        U[2]     = R[1][2] / D[1];
    }
}// namespace

namespace Moer {
    Matrix2x2f Inverse(const Matrix2x2f& m) noexcept { return ::Inverse(m); }
    Matrix2x2d Inverse(const Matrix2x2d& m) noexcept { return ::Inverse(m); }
    Matrix3x3f Inverse(const Matrix3x3f& m) noexcept { return ::Inverse(m); }
    Matrix3x3d Inverse(const Matrix3x3d& m) noexcept { return ::Inverse(m); }
    Matrix4x4f Inverse(const Matrix4x4f& m) noexcept { return ::Inverse(m); }
    Matrix4x4d Inverse(const Matrix4x4d& m) noexcept { return ::Inverse(m); }

    /**
     * camera system(right-handed):
     *      z_axis = origin - look_at
     *      x_axis = Cross(up, z_axis)
     *      y_axis = Cross(z_axis, up)
     * let vector X = [x_axis.x, x_axis.y, x_axis.z]^{T}
     *     vector Y = [y_axis.x, y_axis.y, y_axis.z]^{T}
     *     vector Z = [z_axis.x, z_axis.y, z_axis.z]^{T}
     * camera space coordinate system to world space:
     *    Rotation matrix : [X Y Z] (3x3)
     * world space to camera space:
     *    Rotation matrix : R = [X Y Z]^{T} (3x3)
     * translate origin point to camera origin:
     *    Translation matrix T :
     *      | 1 0 0 -origin.x |
     *      | 0 1 0 -origin.y |
     *      | 0 0 1 -origin.z |
     *      | 0 0 0 1         |
     * 
     * The View matrix == R * T
    */
    Matrix4x4f MakeLookatViewMatrixRH(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept {
        auto y = Normalizef(up_dir);
        auto z = Normalizef(origin - look_at);
        auto x = Normalizef(Cross(y, z));
        y      = Cross(z, x);

        Matrix4x4f view;
        view.r0 = Vector4f(x, Dotf(x, -origin));
        view.r1 = Vector4f(y, Dotf(y, -origin));
        view.r2 = Vector4f(z, Dotf(z, -origin));
        view.r3 = Vector4f(0.f, 0.f, 0.f, 1.f);

        return view;
    }
    Matrix4x4d MakeLookatViewMatrixRH(const Vector3d& origin, const Vector3d& look_at, const Vector3d& up_dir) noexcept {
        auto y = Normalize(up_dir);
        auto z = Normalize(origin - look_at);
        auto x = Normalize(Cross(y, z));
        y      = Cross(z, x);

        Matrix4x4d view;
        view.r0 = Vector4d(x, Dot(x, -origin));
        view.r1 = Vector4d(y, Dot(y, -origin));
        view.r2 = Vector4d(z, Dot(z, -origin));
        view.r3 = Vector4d(0., 0., 0., 1.);

        return view;
    }

    /**
     * (view)^{-1} = T^{-1} * R^{T}
    */
    Matrix4x4f MakeLookatToWorldMatrixRH(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept {
        auto y = Normalizef(up_dir);
        auto z = Normalizef(origin - look_at);
        auto x = Normalizef(Cross(y, z));
        y      = Cross(z, x);

        Matrix4x4f camera_to_world;
        camera_to_world.r0 = Vector4f(x.x, y.x, z.x, origin.x);
        camera_to_world.r1 = Vector4f(x.y, y.y, z.y, origin.y);
        camera_to_world.r2 = Vector4f(x.z, y.z, z.z, origin.z);
        camera_to_world.r3 = Vector4f(0.f, 0.f, 0.f, 1.f);

        return camera_to_world;
    }
    Matrix4x4d MakeLookatToWorldMatrixRH(const Vector3d& origin, const Vector3d& look_at, const Vector3d& up_dir) noexcept {
        auto y = Normalize(up_dir);
        auto z = Normalize(origin - look_at);
        auto x = Normalize(Cross(y, z));
        y      = Cross(z, x);

        Matrix4x4d camera_to_world;
        camera_to_world.r0 = Vector4d(x.x, y.x, z.x, origin.x);
        camera_to_world.r1 = Vector4d(x.y, y.y, z.y, origin.y);
        camera_to_world.r2 = Vector4d(x.z, y.z, z.z, origin.z);
        camera_to_world.r3 = Vector4d(0., 0., 0., 1.);

        return camera_to_world;
    }

    std::pair<Matrix4x4f, Matrix4x4f> MakeLookatViewMatrixWithInverseRH(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept {
        auto y = Normalizef(up_dir);
        auto z = Normalizef(origin - look_at);
        auto x = Normalizef(Cross(y, z));
        y      = Cross(z, x);

        Matrix4x4f view;
        view.r0 = Vector4f(x, Dotf(x, -origin));
        view.r1 = Vector4f(y, Dotf(y, -origin));
        view.r2 = Vector4f(z, Dotf(z, -origin));
        view.r3 = Vector4f(0.f, 0.f, 0.f, 1.f);

        Matrix4x4f camera_to_world;
        camera_to_world.r0 = Vector4f(x.x, y.x, z.x, origin.x);
        camera_to_world.r1 = Vector4f(x.y, y.y, z.y, origin.y);
        camera_to_world.r2 = Vector4f(x.z, y.z, z.z, origin.z);
        camera_to_world.r3 = Vector4f(0.f, 0.f, 0.f, 1.f);

        return {view, camera_to_world};
    }
    std::pair<Matrix4x4d, Matrix4x4d> MakeLookatViewMatrixWithInverseRH(const Vector3d& origin, const Vector3d& look_at, const Vector3d& up_dir) noexcept {
        auto y = Normalize(up_dir);
        auto z = Normalize(origin - look_at);
        auto x = Normalize(Cross(y, z));
        y      = Cross(z, x);

        Matrix4x4d view;
        view.r0 = Vector4d(x, Dot(x, -origin));
        view.r1 = Vector4d(y, Dot(y, -origin));
        view.r2 = Vector4d(z, Dot(z, -origin));
        view.r3 = Vector4d(0., 0., 0., 1.);

        Matrix4x4d camera_to_world;
        camera_to_world.r0 = Vector4d(x.x, y.x, z.x, origin.x);
        camera_to_world.r1 = Vector4d(x.y, y.y, z.y, origin.y);
        camera_to_world.r2 = Vector4d(x.z, y.z, z.z, origin.z);
        camera_to_world.r3 = Vector4d(0., 0., 0., 1.);

        return {view, camera_to_world};
    }

    /**
     * Factor M = QR = QDU where Q is orthogonal, D is diagonal,
     * and U is upper triangular with ones on its diagonal.  Algorithm uses
     * Gram-Schmidt orthogonalization (the QR algorithm).
     * 
     * If M = [ m0 | m1 | m2 ] and Q = [ q0 | q1 | q2 ], then
     *   q0 = m0/|m0|
     *   q1 = (m1-(q0*m1)q0)/|m1-(q0*m1)q0|
     *   q2 = (m2-(q0*m2)q0-(q1*m2)q1)/|m2-(q0*m2)q0-(q1*m2)q1|
     * 
     * where |V| indicates length of vector V and A*B indicates dot
     * product of vectors A and B.  The matrix R has entries
     * 
     *   r00 = q0*m0  r01 = q0*m1  r02 = q0*m2
     *   r10 = 0      r11 = q1*m1  r12 = q1*m2
     *   r20 = 0      r21 = 0      r22 = q2*m2
     * 
     * so D = diag(r00,r11,r22) and U has entries u01 = r01/r00,
     * u02 = r02/r00, and u12 = r12/r11.
     * 
     * Q = rotation
     * D = scaling
     * U = shear
     * 
     * D stores the three diagonal entries r00, r11, r22
     * U stores the entries U[0] = u01, U[1] = u02, U[2] = u12
    */
    void QDUDecomposition(const Matrix3x3f& m, Matrix3x3f& Q, Vector3f& D, Vector3f& U) noexcept {
        ::QDUDecomposition(m, Q, D, U);
    }
    void QDUDecomposition(const Matrix3x3d& m, Matrix3x3d& Q, Vector3d& D, Vector3d& U) noexcept {
        ::QDUDecomposition(m, Q, D, U);
    }

    Matrix4x4f MakePerspectiveMatrixRH(float fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept {
        float tan_half_fov = tanf(fov_y * 0.5f);

        float      inv_tan = 1.f / tan_half_fov;
        float      f_range = far_clip / (near_clip - far_clip);
        Matrix4x4f perspective;
        perspective[0][0] = inv_tan / aspect_ratio;
        perspective[1][1] = inv_tan;
        perspective[2][2] = f_range;
        perspective[3][2] = -1.f;
        perspective[2][3] = near_clip * f_range;
        return perspective;

        //is this maybe better?
        // Matrix4x4f Result;
        // Result[0][0] = 1 / (aspect_ratio * tan_half_fov);
        // Result[1][1] = 1 / (tan_half_fov);
        // Result[2][2] = -(far_clip + near_clip) / (far_clip - near_clip);
        // Result[3][2] = -1;
        // Result[2][3] = -(2 * far_clip * near_clip) / (far_clip - near_clip);
        // return Result;
    }
    Matrix4x4d MakePerspectiveMatrixRH(double fov_y, double aspect_ratio, double near_clip, double far_clip) noexcept {
        double     tan_half_fov = std::tan(fov_y * 0.5f);
        double     inv_tan      = 1.f / tan_half_fov;
        double     f_range      = far_clip / (near_clip - far_clip);
        Matrix4x4d perspective;
        perspective[0][0] = inv_tan / aspect_ratio;
        perspective[1][1] = inv_tan;
        perspective[2][2] = f_range;
        perspective[3][2] = -1.f;
        perspective[2][3] = near_clip * f_range;

        return perspective;
    }

    // Reference: glm/ext/matrix_clip_space.inl: othroRH_ZO
    Matrix4x4f MakeOrthoMatrixRH(float left, float right, float bottom, float top, float near_clip, float far_clip) noexcept {
        Matrix4x4f ortho;
        ortho[0][0] = 2.0f / (right - left);
        ortho[1][1] = 2.0f / (top - bottom);
        ortho[2][2] = -1.0f / (far_clip - near_clip);
        ortho[0][3] = -(right + left) / (right - left);
        ortho[1][3] = -(top + bottom) / (top - bottom);
        ortho[2][3] = -near_clip / (far_clip - near_clip);
        ortho[3][3] = 1.0f;
        return ortho;
    }
    Matrix4x4d MakeOrthoMatrixRH(double left, double right, double bottom, double top, double near_clip, double far_clip) noexcept {
        Matrix4x4d ortho;
        ortho[0][0] = 2.0 / (right - left);
        ortho[1][1] = 2.0 / (top - bottom);
        ortho[2][2] = -1.0 / (far_clip - near_clip);
        ortho[0][3] = -(right + left) / (right - left);
        ortho[1][3] = -(top + bottom) / (top - bottom);
        ortho[2][3] = -near_clip / (far_clip - near_clip);
        ortho[3][3] = 1.0;
        return ortho;
    }
}// namespace Moer