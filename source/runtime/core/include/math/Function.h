#pragma once

#include "Base.h"
#include "Matrix.h"

#include <cmath>

// vector functions
namespace Moer {
    // clang-format off
    template<NumericType T> inline T Max(T lhs, T rhs) noexcept;
    template<NumericType T> inline T Min(T lhs, T rhs) noexcept;
    template<VectorType  T> inline T Max(const T& lhs, const T& rhs) noexcept;
    template<VectorType  T> inline T Min(const T& lhs, const T& rhs) noexcept;

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
    template<VectorType T> inline Vectorf<T::size> Normalizef(const T& v) noexcept;

    template<VectorType T> inline double Dot(const T& lhs, const T& rhs) noexcept;
    template<VectorType T> inline double Length(const T& v) noexcept;
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
    template<NumericType T> inline Matrix<T, 2, 2> Inverse(const Matrix<T, 2, 2>& m) noexcept;
    template<NumericType T> inline Matrix<T, 3, 3> Inverse(const Matrix<T, 3, 3>& m) noexcept;
    template<NumericType T> inline Matrix<T, 4, 4> Inverse(const Matrix<T, 4, 4>& m) noexcept;

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
    inline Vectord<T::size> Normalize(const T& v) noexcept {
        return v / Length(v);
    }

    template<NumericType T>
    inline Vector<T, 3> Cross(const Vector<T, 3>& lhs, const Vector<T, 3>& rhs) noexcept {
        return Vector<T, 3>{lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x};
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

    template<NumericType T>
    inline Matrix<T, 2, 2> Inverse(const Matrix<T, 2, 2>& m) noexcept {
        Matrix<T, 2, 2> ret;

        T inv_det = (T)1 / (m[0][0] * m[1][1] - m[0][1] * m[1][0]);
        ret[0][0] = m[1][1] * inv_det;
        ret[0][1] = -m[0][1] * inv_det;
        ret[1][0] = -m[1][0] * inv_det;
        ret[1][1] = m[0][0] * inv_det;
    }
    template<NumericType T>
    inline Matrix<T, 3, 3> Inverse(const Matrix<T, 3, 3>& m) noexcept {
        Matrix<T, 3, 3> ret;

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
    template<NumericType T>
    inline Matrix<T, 4, 4> Inverse(const Matrix<T, 4, 4>& m) noexcept {
        Matrix<T, 4, 4> ret;

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

}// namespace Moer
