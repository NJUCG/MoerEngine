#pragma once

#include "Base.h"
#include "Matrix.h"
#include "Constant.h"

#include <cmath>
#include <utility>

// vector functions
namespace Moer {
    // clang-format off
    template<NumericType T> inline T Max(T lhs, T rhs) noexcept;
    template<NumericType T> inline T Min(T lhs, T rhs) noexcept;
    template<VectorType  T> inline T Max(const T& lhs, const T& rhs) noexcept;
    template<VectorType  T> inline T Min(const T& lhs, const T& rhs) noexcept;
    
    template<VectorType T> inline T operator+(const T& v) noexcept;
    template<VectorType T> inline T operator-(const T& v) noexcept;

    template<VectorType T> inline bool operator==(const T& lhs, const T& rhs) noexcept;
    template<VectorType T> inline bool operator!=(const T& lhs, const T& rhs) noexcept;

    template<VectorType T, VectorType U> inline T operator+(const T& lhs, const U& rhs) noexcept;
    template<VectorType T, VectorType U> inline T operator-(const T& lhs, const U& rhs) noexcept;
    template<VectorType T, VectorType U> inline T operator*(const T& lhs, const U& rhs) noexcept;
    template<VectorType T, VectorType U> inline T operator/(const T& lhs, const U& rhs) noexcept;

    template<VectorType T, NumericType U> inline T operator+(const T& lhs, const U rhs) noexcept;
    template<VectorType T, NumericType U> inline T operator-(const T& lhs, const U rhs) noexcept;
    template<VectorType T, NumericType U> inline T operator*(const T& lhs, const U rhs) noexcept;
    template<VectorType T, NumericType U> inline T operator/(const T& lhs, const U rhs) noexcept;

    template<NumericType T, VectorType U> inline U operator+(const T lhs, const U& rhs) noexcept;
    template<NumericType T, VectorType U> inline U operator-(const T lhs, const U& rhs) noexcept;
    template<NumericType T, VectorType U> inline U operator*(const T lhs, const U& rhs) noexcept;
    template<NumericType T, VectorType U> inline U operator/(const T lhs, const U& rhs) noexcept;

    template<VectorType T, VectorType U> inline void operator+=(T& lhs, const U& rhs) noexcept;
    template<VectorType T, VectorType U> inline void operator-=(T& lhs, const U& rhs) noexcept;
    template<VectorType T, VectorType U> inline void operator*=(T& lhs, const U& rhs) noexcept;
    template<VectorType T, VectorType U> inline void operator/=(T& lhs, const U& rhs) noexcept;

    template<VectorType T, NumericType U> inline void operator+=(T& lhs, const U rhs) noexcept;
    template<VectorType T, NumericType U> inline void operator-=(T& lhs, const U rhs) noexcept;
    template<VectorType T, NumericType U> inline void operator*=(T& lhs, const U rhs) noexcept;
    template<VectorType T, NumericType U> inline void operator/=(T& lhs, const U rhs) noexcept;

    template<NumericType T> inline bool ApproxEqual(T lhs, T rhs, float eps = Moer::EPS) noexcept;
    template<NumericType T> inline bool ApproxEqual(T lhs, T rhs, double eps = (double)Moer::EPS) noexcept;
    template<VectorType  T> inline bool ApproxEqual(const T& lhs, const T& rhs, float eps = Moer::EPS) noexcept;
    template<VectorType  T> inline bool ApproxEqual(const T& lhs, const T& rhs, double eps = (double)Moer::EPS) noexcept;

    template<NumericType T> inline T Abs(T v) noexcept;
    template<VectorType  T> inline T Abs(const T& v) noexcept;

                       inline float      Lerp(float a, float b, float t) noexcept;
    template<size_t N> inline Vectorf<N> Lerp(const Vectorf<N>& a, const Vectorf<N>& b, const Vectorf<N>& t) noexcept;
                       inline double     Lerp(double a, double b, double t) noexcept;
    template<size_t N> inline Vectord<N> Lerp(const Vectord<N>& a, const Vectord<N>& b, const Vectord<N>& t) noexcept;

                       inline float      Pow(float base, float power) noexcept;
    template<size_t N> inline Vectorf<N> Pow(const Vectorf<N>& base, float& power) noexcept;
    template<size_t N> inline Vectorf<N> Pow(const Vectorf<N>& base, const Vectorf<N>& power) noexcept;
                       inline double     Pow(double base, double power) noexcept;
    template<size_t N> inline Vectord<N> Pow(const Vectord<N>& base, double& power) noexcept;
    template<size_t N> inline Vectord<N> Pow(const Vectord<N>& base, const Vectord<N>& power) noexcept;

    template<NumericType T> inline T Clamp(T v, T a, T b) noexcept;
    template<VectorType  T> inline T Clamp(const T& v, const T& a, const T& b) noexcept;

    template<VectorType T> inline float Dotf(const T& lhs, const T& rhs) noexcept;
    template<VectorType T> inline float Lengthf(const T& v) noexcept;
    template<VectorType T> inline float SquaredLengthf(const T& v) noexcept;
    template<VectorType T> inline Vectorf<T::size> Normalizef(const T& v) noexcept;

    template<VectorType T> inline double Dot(const T& lhs, const T& rhs) noexcept;
    template<VectorType T> inline double Length(const T& v) noexcept;
    template<VectorType T> inline double SquaredLength(const T& v) noexcept;
    template<VectorType T> inline Vectord<T::size> Normalize(const T& v) noexcept;

    template<NumericType T> inline Vector<T, 3> Cross(const Vector<T, 3>& lhs, const Vector<T, 3>& rhs) noexcept;

    template<VectorType T> inline T Reflect(const T& v, const T& n) noexcept;

    // clang-format on
}// namespace Moer

// matrix functions
namespace Moer {
    // clang-format off
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<T, ROW, COL> operator+(const Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<T, ROW, COL> operator-(const Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<T, ROW, COL> operator/(const Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;

    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<T, ROW, COL> operator+(const Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<T, ROW, COL> operator-(const Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<T, ROW, COL> operator*(const Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<T, ROW, COL> operator/(const Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<U, ROW, COL> operator+(const T lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<U, ROW, COL> operator-(const T lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<U, ROW, COL> operator*(const T lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<U, ROW, COL> operator/(const T lhs, const Matrix<U, ROW, COL>& rhs) noexcept;

    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator+=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator-=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator/=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;

    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator+=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator-=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator*=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator/=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;

    // matrix multiplication: mat1(n x m) * mat2(m x k) = mat3(n x k)
    template<NumericType T, NumericType U, size_t N, size_t M, size_t K> inline Matrix<T, N, K> operator*(const Matrix<T, N, M>& lhs, const Matrix<U, M, K>& rhs) noexcept;
  
    // column major matrix(n x m) * vector(m x 1) = vector(n x 1)
    template<NumericType T, size_t ROW, size_t COL> inline Vector<T, ROW> operator*(const Matrix<T, ROW, COL>& m, const Vector<T, COL>& v) noexcept;
    // row major vector(1 x n) * matrix(n x m) = vector(1 x m)
    template<NumericType T, size_t ROW, size_t COL> inline Vector<T, COL> operator*(const Vector<T, ROW>& v, const Matrix<T, ROW, COL>& m) noexcept;

    // transpose matrix(n x m) to matrix(m x n)
    template<NumericType T, size_t N, size_t M> inline Matrix<T, M, N> Transpose(const Matrix<T, N, M>& m) noexcept;

    // inverse of a matrix
    Matrix2x2f Inverse(const Matrix2x2f& m) noexcept;
    Matrix2x2d Inverse(const Matrix2x2d& m) noexcept;
    Matrix3x3f Inverse(const Matrix3x3f& m) noexcept;
    Matrix3x3d Inverse(const Matrix3x3d& m) noexcept;
    Matrix4x4f Inverse(const Matrix4x4f& m) noexcept;
    Matrix4x4d Inverse(const Matrix4x4d& m) noexcept;

    /** get a 2x2 matrix from diagonal
       @example:
        a b c       a b c d        a b c d        a b
        e f g   or  e f g h   or   e f g h   ->   c d
        i j k       i j k l        i j k l
                                   m n o p
    */
    template<NumericType T, size_t ROW, size_t COL> inline Matrix<T, 2, 2> GetDiagonal2x2(const Matrix<T, ROW, COL>& m) noexcept;
    
    // get a 3x3 matrix from diagonal
    template<NumericType T, size_t ROW, size_t COL> inline Matrix<T, 3, 3> GetDiagonal3x3(const Matrix<T, ROW, COL>& m) noexcept;
    
    /** fill a 2x2 matrix into the upper left corner of a 3x3 matrix
        @param diagonal_element the number used to fill into diagonal
        @example:
            diagonal_element == 1
            a b             a b 0
            c d     ->      c d 0
                            0 0 1
    */
    template<NumericType T> inline Matrix<T, 3, 3> FillDiagonal3x3(const Matrix<T, 2, 2>& m, T diagonal_element) noexcept;
    /** fill a 2x2 or 3x3 or 3x4 matrix into the upper left corner of a 4x4 matrix
        @param diagonal_element the number used to fill into diagonal
        @example:
            diagonal_element == 1
            a b             a b 0 0
            c d     ->      c d 0 0
                            0 0 1 0
                            0 0 0 1
    */
    template<NumericType T, size_t ROW, size_t COL> inline Matrix<T, 4, 4> FillDiagonal4x4(const Matrix<T, ROW, COL>& m, T diagonal_element) noexcept;

    // make the view matrix in right-handed system
    Matrix4x4f MakeLookatViewMatrixRH(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept;
    Matrix4x4d MakeLookatViewMatrixRH(const Vector3d& origin, const Vector3d& look_at, const Vector3d& up_dir) noexcept;
    // make the camera to world matrix in right-handed system
    Matrix4x4f MakeLookatToWorldMatrixRH(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept;
    Matrix4x4d MakeLookatToWorldMatrixRH(const Vector3d& origin, const Vector3d& look_at, const Vector3d& up_dir) noexcept;
    // return world to camera view matrix and camera to world matrix in right-handed system
    std::pair<Matrix4x4f, Matrix4x4f> MakeLookatViewMatrixWithInverseRH(const Vector3f& origin, const Vector3f& look_at, const Vector3f& up_dir) noexcept;
    std::pair<Matrix4x4d, Matrix4x4d> MakeLookatViewMatrixWithInverseRH(const Vector3d& origin, const Vector3d& look_at, const Vector3d& up_dir) noexcept;

    void QDUDecomposition(const Matrix3x3f& m, Matrix3x3f& Q, Vector3f& D, Vector3f& U) noexcept;
    void QDUDecomposition(const Matrix3x3d& m, Matrix3x3d& Q, Vector3d& D, Vector3d& U) noexcept;

    /**
     * make the perspective matrix in right-handed system
     * @param fov_y y-axis field of view(radian)
     */ 
    Matrix4x4f MakePerspectiveMatrixRH(float fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept;
    Matrix4x4d MakePerspectiveMatrixRH(double fov_y, double aspect_ratio, double near_clip, double far_clip) noexcept;
    // clang-format on
}// namespace Moer

// implementation
namespace Moer {
    template<NumericType T>
    inline T Max(T lhs, T rhs) noexcept { return std::max(lhs, rhs); }
    template<NumericType T>
    inline T Min(T lhs, T rhs) noexcept { return std::min(lhs, rhs); }
    template<VectorType T>
    inline T Max(const T& lhs, const T& rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = Max(lhs[i], rhs[i]);
        return ret;
    }
    template<VectorType T>
    inline T Min(const T& lhs, const T& rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = Min(lhs[i], rhs[i]);
        return ret;
    }

    template<VectorType T>
    inline T operator+(const T& v) noexcept {
        return v;
    }
    template<VectorType T>
    inline T operator-(const T& v) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = -v[i];
        return ret;
    }

    template<VectorType T>
    inline bool operator==(const T& lhs, const T& rhs) noexcept {
        bool ret = true;
        for (int i = 0; i < T::size; i++) ret &= (lhs[i] == rhs[i]);
        return ret;
    }
    template<VectorType T>
    inline bool operator!=(const T& lhs, const T& rhs) noexcept {
        return !(lhs == rhs);
    }

    template<VectorType T, VectorType U>
    inline T operator+(const T& lhs, const U& rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = lhs[i] + rhs[i];
        return ret;
    }
    template<VectorType T, VectorType U>
    inline T operator-(const T& lhs, const U& rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = lhs[i] - rhs[i];
        return ret;
    }
    template<VectorType T, VectorType U>
    inline T operator*(const T& lhs, const U& rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = lhs[i] * rhs[i];
        return ret;
    }
    template<VectorType T, VectorType U>
    inline T operator/(const T& lhs, const U& rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = lhs[i] / rhs[i];
        return ret;
    }

    template<VectorType T, NumericType U>
    inline T operator+(const T& lhs, const U rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = lhs[i] + rhs;
        return ret;
    }
    template<VectorType T, NumericType U>
    inline T operator-(const T& lhs, const U rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = lhs[i] - rhs;
        return ret;
    }
    template<VectorType T, NumericType U>
    inline T operator*(const T& lhs, const U rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = lhs[i] * rhs;
        return ret;
    }
    template<VectorType T, NumericType U>
    inline T operator/(const T& lhs, const U rhs) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = lhs[i] / rhs;
        return ret;
    }

    template<NumericType T, VectorType U>
    inline U operator+(const T lhs, const U& rhs) noexcept {
        U ret;
        for (int i = 0; i < U::size; i++) ret[i] = lhs + rhs[i];
        return ret;
    }
    template<NumericType T, VectorType U>
    inline U operator-(const T lhs, const U& rhs) noexcept {
        U ret;
        for (int i = 0; i < U::size; i++) ret[i] = lhs - rhs[i];
        return ret;
    }
    template<NumericType T, VectorType U>
    inline U operator*(const T lhs, const U& rhs) noexcept {
        U ret;
        for (int i = 0; i < U::size; i++) ret[i] = lhs * rhs[i];
        return ret;
    }
    template<NumericType T, VectorType U>
    inline U operator/(const T lhs, const U& rhs) noexcept {
        U ret;
        for (int i = 0; i < U::size; i++) ret[i] = lhs / rhs[i];
        return ret;
    }

    template<VectorType T, VectorType U>
    inline void operator+=(T& lhs, const U& rhs) noexcept {
        for (int i = 0; i < T::size; i++) lhs[i] += rhs[i];
    }
    template<VectorType T, VectorType U>
    inline void operator-=(T& lhs, const U& rhs) noexcept {
        for (int i = 0; i < T::size; i++) lhs[i] -= rhs[i];
    }
    template<VectorType T, VectorType U>
    inline void operator*=(T& lhs, const U& rhs) noexcept {
        for (int i = 0; i < T::size; i++) lhs[i] *= rhs[i];
    }
    template<VectorType T, VectorType U>
    inline void operator/=(T& lhs, const U& rhs) noexcept {
        for (int i = 0; i < T::size; i++) lhs[i] /= rhs[i];
    }

    template<VectorType T, NumericType U>
    inline void operator+=(T& lhs, const U rhs) noexcept {
        for (int i = 0; i < T::size; i++) lhs[i] += rhs;
    }
    template<VectorType T, NumericType U>
    inline void operator-=(T& lhs, const U rhs) noexcept {
        for (int i = 0; i < T::size; i++) lhs[i] -= rhs;
    }
    template<VectorType T, NumericType U>
    inline void operator*=(T& lhs, const U rhs) noexcept {
        for (int i = 0; i < T::size; i++) lhs[i] *= rhs;
    }
    template<VectorType T, NumericType U>
    inline void operator/=(T& lhs, const U rhs) noexcept {
        for (int i = 0; i < T::size; i++) lhs[i] /= rhs;
    }

    template<NumericType T>
    inline bool ApproxEqual(T lhs, T rhs, float eps) noexcept {
        return (abs(lhs - rhs) < eps);
    }
    template<NumericType T>
    inline bool ApproxEqual(T lhs, T rhs, double eps) noexcept {
        return (abs(lhs - rhs) < eps);
    }

    template<VectorType T>
    inline bool ApproxEqual(const T& lhs, const T& rhs, float eps) noexcept {
        bool ret = true;
        for (int i = 0; i < T::size; i++) ret &= (abs(lhs[i] - rhs[i]) < eps);
        return ret;
    }
    template<VectorType T>
    inline bool ApproxEqual(const T& lhs, const T& rhs, double eps) noexcept {
        bool ret = true;
        for (int i = 0; i < T::size; i++) ret &= (abs(lhs[i] - rhs[i]) < eps);
        return ret;
    }

    template<NumericType T>
    inline T Abs(T v) noexcept {
        return std::abs(v);
    }

    template<VectorType T>
    inline T Abs(const T& v) noexcept {
        T ret;
        for (int i = 0; i < T::size; i++) ret[i] = std::abs(v[i]);
        return ret;
    }

    inline float Lerp(float a, float b, float t) noexcept { return a + t * (b - a); }
    template<size_t N>
    inline Vectorf<N> Lerp(const Vectorf<N>& a, const Vectorf<N>& b, const Vectorf<N>& t) noexcept {
        Vectorf<N> ret;
        for (int i = 0; i < N; i++) ret[i] = Lerp(a[i], b[i], t[i]);
        return ret;
    }
    inline double Lerp(double a, double b, double t) noexcept { return a + t * (b - a); }
    template<size_t N>
    inline Vectord<N> Lerp(const Vectord<N>& a, const Vectord<N>& b, const Vectord<N>& t) noexcept {
        Vectord<N> ret;
        for (int i = 0; i < N; i++) ret[i] = Lerp(a[i], b[i], t[i]);
        return ret;
    }

    inline float Pow(float base, float power) noexcept { return std::pow(base, power); }
    template<size_t N>
    inline Vectorf<N> Pow(const Vectorf<N>& base, float& power) noexcept {
        Vectorf<N> ret;
        for (int i = 0; i < N; i++) ret[i] = Pow(base[i], power);
        return ret;
    }
    template<size_t N>
    inline Vectorf<N> Pow(const Vectorf<N>& base, const Vectorf<N>& power) noexcept {
        Vectorf<N> ret;
        for (int i = 0; i < N; i++) ret[i] = Pow(base[i], power[i]);
        return ret;
    }
    inline double Pow(double base, double power) noexcept { return std::pow(base, power); }
    template<size_t N>
    inline Vectord<N> Pow(const Vectord<N>& base, double& power) noexcept {
        Vectord<N> ret;
        for (int i = 0; i < N; i++) ret[i] = Pow(base[i], power);
        return ret;
    }
    template<size_t N>
    inline Vectord<N> Pow(const Vectord<N>& base, const Vectord<N>& power) noexcept {
        Vectord<N> ret;
        for (int i = 0; i < N; i++) ret[i] = Pow(base[i], power[i]);
        return ret;
    }

    template<NumericType T>
    inline T Clamp(T v, T a, T b) noexcept { return Max(a, Min(v, b)); }
    template<VectorType T>
    inline T Clamp(const T& v, const T& a, const T& b) noexcept { return Max(a, Min(v, b)); }

    template<VectorType T>
    inline float Dotf(const T& lhs, const T& rhs) noexcept {
        float ret = 0.f;
        for (int i = 0; i < T::size; i++) ret += lhs[i] * rhs[i];
        return ret;
    }

    template<VectorType T>
    inline float Lengthf(const T& v) noexcept {
        return std::sqrtf(Dotf(v, v));
    }

    template<VectorType T>
    inline float SquaredLengthf(const T& v) noexcept {
        return Dotf(v, v);
    }

    template<VectorType T>
    inline Vectorf<T::size> Normalizef(const T& v) noexcept {
        return v / Lengthf(v);
    }

    template<VectorType T>
    inline double Dot(const T& lhs, const T& rhs) noexcept {
        double ret = 0.;
        for (int i = 0; i < T::size; i++) ret += lhs[i] * rhs[i];
        return ret;
    }

    template<VectorType T>
    inline double Length(const T& v) noexcept {
        return std::sqrt(Dot(v, v));
    }

    template<VectorType T>
    inline double SquaredLength(const T& v) noexcept {
        return Dot(v, v);
    }

    template<VectorType T>
    inline Vectord<T::size> Normalize(const T& v) noexcept {
        return v / Length(v);
    }

    template<NumericType T>
    inline Vector<T, 3> Cross(const Vector<T, 3>& lhs, const Vector<T, 3>& rhs) noexcept {
        return Vector<T, 3>(lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x);
    }

    template<VectorType T>
    inline T Reflect(const T& v, const T& n) noexcept {
        return v - 2. * n * Dot(n, v);
    }

}// namespace Moer

namespace Moer {
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<T, ROW, COL> operator+(const Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        Matrix<T, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs[i][j] + rhs[i][j];
        return ret;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<T, ROW, COL> operator-(const Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        Matrix<T, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs[i][j] - rhs[i][j];
        return ret;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<T, ROW, COL> operator/(const Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        Matrix<T, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs[i][j] / rhs[i][j];
        return ret;
    }

    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<T, ROW, COL> operator+(const Matrix<T, ROW, COL>& lhs, const U rhs) noexcept {
        Matrix<T, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs[i][j] + rhs;
        return ret;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<T, ROW, COL> operator-(const Matrix<T, ROW, COL>& lhs, const U rhs) noexcept {
        Matrix<T, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs[i][j] - rhs;
        return ret;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<T, ROW, COL> operator*(const Matrix<T, ROW, COL>& lhs, const U rhs) noexcept {
        Matrix<T, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs[i][j] * rhs;
        return ret;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<T, ROW, COL> operator/(const Matrix<T, ROW, COL>& lhs, const U rhs) noexcept {
        Matrix<T, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs[i][j] / rhs;
        return ret;
    }

    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<U, ROW, COL> operator+(const T lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        Matrix<U, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs + rhs[i][j];
        return ret;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<U, ROW, COL> operator-(const T lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        Matrix<U, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs - rhs[i][j];
        return ret;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<U, ROW, COL> operator*(const T lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        Matrix<U, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs * rhs[i][j];
        return ret;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline Matrix<U, ROW, COL> operator/(const T lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        Matrix<U, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs / rhs[i][j];
        return ret;
    }

    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline void operator+=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) lhs[i][j] += rhs[i][j];
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline void operator-=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) lhs[i][j] -= rhs[i][j];
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline void operator/=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) lhs[i][j] /= rhs[i][j];
    }

    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline void operator+=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept {
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) lhs[i][j] += rhs;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline void operator-=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept {
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) lhs[i][j] -= rhs;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline void operator*=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept {
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) lhs[i][j] *= rhs;
    }
    template<NumericType T, NumericType U, size_t ROW, size_t COL>
    inline void operator/=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept {
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) lhs[i][j] /= rhs;
    }

    template<NumericType T, NumericType U, size_t N, size_t M, size_t K>
    inline Matrix<T, N, K> operator*(const Matrix<T, N, M>& lhs, const Matrix<U, M, K>& rhs) noexcept {
        Matrix<T, N, K> ret;
        for (int i = 0; i < N; i++)
            for (int j = 0; j < K; j++) ret[i][j] = Dot(lhs[i], rhs.GetColumn(j));
        return ret;
    }

    template<NumericType T, size_t ROW, size_t COL>
    inline Vector<T, ROW> operator*(const Matrix<T, ROW, COL>& m, const Vector<T, COL>& v) noexcept {
        Vector<T, ROW> ret;
        for (int i = 0; i < ROW; i++) ret[i] = Dot(m[i], v);
        return ret;
    }
    template<NumericType T, size_t ROW, size_t COL>
    inline Vector<T, COL> operator*(const Vector<T, ROW>& v, const Matrix<T, ROW, COL>& m) noexcept {
        Vector<T, COL> ret;
        for (int i = 0; i < COL; i++)
            for (int j = 0; j < ROW; j++) ret[i] += m[j][i] * v[j];
        return ret;
    }

    template<NumericType T, size_t N, size_t M>
    inline Matrix<T, M, N> Transpose(const Matrix<T, N, M>& m) noexcept {
        Matrix<T, M, N> ret;
        for (int i = 0; i < M; i++)
            for (int j = 0; j < N; j++) ret[i][j] = m[j][i];
        return ret;
    }

    template<NumericType T, size_t ROW, size_t COL>
    inline Matrix<T, 2, 2> GetDiagonal2x2(const Matrix<T, ROW, COL>& m) noexcept {
        return Matrix<T, 2, 2>(m[0][0], m[0][1], m[1][0], m[1][1]);
    }

    template<NumericType T, size_t ROW, size_t COL>
    inline Matrix<T, 3, 3> GetDiagonal3x3(const Matrix<T, ROW, COL>& m) noexcept {
        static_assert((ROW >= 3 && COL >= 3));
        return Matrix<T, 3, 3>(m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
    }

    template<NumericType T>
    inline Matrix<T, 3, 3> FillDiagonal3x3(const Matrix<T, 2, 2>& m, T diagonal_element) noexcept {
        return Matrix<T, 3, 3>(m[0][0], m[0][1], 0, m[1][0], m[1][1], 0, 0, 0, diagonal_element);
    }
    template<NumericType T, size_t ROW, size_t COL>
    inline Matrix<T, 4, 4> FillDiagonal4x4(const Matrix<T, ROW, COL>& m, T diagonal_element) noexcept {
        static_assert((ROW <= 3 && COL <= 3));
        Matrix<T, 4, 4> ret;
        ret.r0 = Vector<T, 4>(m.r0);
        ret.r1 = Vector<T, 4>(m.r1);
        if constexpr (ROW == 3)
            ret.r2 = Vector<T, 4>(m.r2);
        else
            ret.r2 = Vector<T, 4>(0, 0, diagonal_element, 0);
        ret.r3 = Vector<T, 4>(0, 0, 0, diagonal_element);

        return ret;
    }
}// namespace Moer
