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
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline Matrix<T, ROW, COL> operator*(const Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
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
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator*=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator/=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept;

    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator+=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator-=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator*=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;
    template<NumericType T, NumericType U, size_t ROW, size_t COL> inline void operator/=(Matrix<T, ROW, COL>& lhs, const U rhs) noexcept;

    // column major matrix(n x m) * vector(m x 1) = vector(n x 1)
    template<NumericType T, size_t ROW, size_t COL> inline Vector<T, ROW> operator*(const Matrix<T, ROW, COL>& m, const Vector<T, COL>& v) noexcept;
    // row major vector(1 x n) * matrix(n x m) = vector(1 x m)
    template<NumericType T, size_t ROW, size_t COL> inline Vector<T, COL> operator*(const Vector<T, ROW>& v, const Matrix<T, ROW, COL>& m) noexcept;

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
    inline Matrix<T, ROW, COL> operator*(const Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        Matrix<T, ROW, COL> ret;
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) ret[i][j] = lhs[i][j] * rhs[i][j];
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
    inline void operator*=(Matrix<T, ROW, COL>& lhs, const Matrix<U, ROW, COL>& rhs) noexcept {
        for (int i = 0; i < ROW; i++)
            for (int j = 0; j < COL; j++) lhs[i][j] *= rhs[i][j];
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

}// namespace Moer
