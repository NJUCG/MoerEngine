#pragma once

#include "Base.h"

namespace Moer {
    template<NumericType T, size_t ROW, size_t COL>
        requires((ROW > 1 && ROW < 5) && (COL > 1 && COL < 5))
    struct Matrix {};

    template<NumericType T>
    struct Matrix<T, 2, 2> {
        union {
            struct {
                Vector<T, 2> r0;
                Vector<T, 2> r1;
            };
            T            e[4];
            Vector<T, 2> re[2];
        };

        static constexpr size_t row_size = 2;
        static constexpr size_t col_size = 2;

        Matrix() noexcept : r0(0.f), r1(0.f) {}

        Matrix(const Matrix<T, 2, 2>& m) noexcept : r0(m.r0), r1(m.r1) {}
        explicit Matrix(const Vector<T, 2>& r0, const Vector<T, 2>& r1) noexcept
            : r0(r0), r1(r1) {}
        explicit Matrix(T m00, T m01, T m10, T m11) noexcept
            : r0(m00, m01), r1(m10, m11) {}
        explicit Matrix(T (&e)[4]) noexcept { std::copy(e, e + 4, this->e); }
        explicit Matrix(const T* e) noexcept { std::copy(e, e + 4, this->e); }

        template<NumericType U>
        explicit Matrix(const Matrix<U, 2, 2>& m) noexcept : r0(m.r0), r1(m.r1) {}

        Vector<T, 2>  operator[](size_t i) const { return re[i]; }
        Vector<T, 2>& operator[](size_t i) { return re[i]; }

        Vector<T, 2> GetColumn(size_t i) const noexcept { return Vector<T, 2>(re[0][i], re[1][i]); }

        static Matrix<T, 2, 2> Identity() noexcept {
            Matrix<T, 2, 2> identity;
            identity[0][0] = (T)1;
            identity[1][1] = (T)1;
            return identity;
        }
    };

    template<NumericType T>
    struct Matrix<T, 3, 3> {
        union {
            struct {
                Vector<T, 3> r0;
                Vector<T, 3> r1;
                Vector<T, 3> r2;
            };
            T            e[9];
            Vector<T, 3> re[3];
        };

        static constexpr size_t row_size = 3;
        static constexpr size_t col_size = 3;

        Matrix() noexcept : r0(0.f), r1(0.f), r2(0.f) {}

        Matrix(const Matrix<T, 3, 3>& m) noexcept : r0(m.r0), r1(m.r1), r2(m.r2) {}
        explicit Matrix(const Vector<T, 3>& r0, const Vector<T, 3>& r1, const Vector<T, 3>& r2) noexcept
            : r0(r0), r1(r1), r2(r2) {}
        explicit Matrix(T m00, T m01, T m02, T m10, T m11, T m12, T m20, T m21, T m22) noexcept
            : r0(m00, m01, m02), r1(m10, m11, m12), r2(m20, m21, m22) {}
        explicit Matrix(T (&e)[9]) noexcept { std::copy(e, e + 9, this->e); }
        explicit Matrix(const T* e) noexcept { std::copy(e, e + 9, this->e); }

        template<NumericType U>
        explicit Matrix(const Matrix<U, 3, 3>& m) noexcept : r0(m.r0), r1(m.r1), r2(m.r2) {}

        Vector<T, 3>  operator[](size_t i) const { return re[i]; }
        Vector<T, 3>& operator[](size_t i) { return re[i]; }

        Vector<T, 3> GetColumn(size_t i) const noexcept { return Vector<T, 3>(re[0][i], re[1][i], re[2][i]); }

        static Matrix<T, 3, 3> Identity() noexcept {
            Matrix<T, 3, 3> identity;
            identity[0][0] = (T)1;
            identity[1][1] = (T)1;
            identity[2][2] = (T)1;
            return identity;
        }
    };

    template<NumericType T>
    struct Matrix<T, 3, 4> {
        union {
            struct {
                Vector<T, 4> r0;
                Vector<T, 4> r1;
                Vector<T, 4> r2;
            };
            T            e[12];
            Vector<T, 4> re[3];
        };

        static constexpr size_t row_size = 3;
        static constexpr size_t col_size = 4;

        Matrix() noexcept : r0(0.f), r1(0.f), r2(0.f) {}

        Matrix(const Matrix<T, 3, 4>& m) noexcept : r0(m.r0), r1(m.r1), r2(m.r2) {}
        explicit Matrix(const Vector<T, 4>& r0, const Vector<T, 4>& r1, const Vector<T, 4>& r2) noexcept
            : r0(r0), r1(r1), r2(r2) {}
        explicit Matrix(T m00, T m01, T m02, T m03, T m10, T m11, T m12, T m13, T m20, T m21, T m22, T m23) noexcept
            : r0(m00, m01, m02, m03), r1(m10, m11, m12, m13), r2(m20, m21, m22, m23) {}
        explicit Matrix(T (&e)[12]) noexcept { std::copy(e, e + 12, this->e); }
        explicit Matrix(const T* e) noexcept { std::copy(e, e + 12, this->e); }

        template<NumericType U>
        explicit Matrix(const Matrix<U, 3, 4>& m) noexcept : r0(m.r0), r1(m.r1), r2(m.r2) {}

        Vector<T, 4>  operator[](size_t i) const { return re[i]; }
        Vector<T, 4>& operator[](size_t i) { return re[i]; }

        Vector<T, 3> GetColumn(size_t i) const noexcept { return Vector<T, 3>(re[0][i], re[1][i], re[2][i]); }
    };

    template<NumericType T>
    struct Matrix<T, 4, 4> {
        union {
            struct {
                Vector<T, 4> r0;
                Vector<T, 4> r1;
                Vector<T, 4> r2;
                Vector<T, 4> r3;
            };
            T            e[16];
            Vector<T, 4> re[4];
        };

        static constexpr size_t row_size = 4;
        static constexpr size_t col_size = 4;

        Matrix() noexcept : r0(0.f), r1(0.f), r2(0.f), r3(0.f) {}

        Matrix(const Matrix<T, 4, 4>& m) noexcept : r0(m.r0), r1(m.r1), r2(m.r2), r3(m.r3) {}
        explicit Matrix(const Vector<T, 4>& r0, const Vector<T, 4>& r1, const Vector<T, 4>& r2, const Vector<T, 4>& r3) noexcept
            : r0(r0), r1(r1), r2(r2), r3(r3) {}
        explicit Matrix(T m00, T m01, T m02, T m03, T m10, T m11, T m12, T m13, T m20, T m21, T m22, T m23, T m30, T m31, T m32, T m33) noexcept
            : r0(m00, m01, m02, m03), r1(m10, m11, m12, m13), r2(m20, m21, m22, m23), r3(m30, m31, m32, m33) {}
        explicit Matrix(T (&e)[16]) noexcept { std::copy(e, e + 16, this->e); }
        explicit Matrix(const T* e) noexcept { std::copy(e, e + 16, this->e); }

        template<NumericType U>
        explicit Matrix(const Matrix<U, 4, 4>& m) noexcept : r0(m.r0), r1(m.r1), r2(m.r2), r3(m.r3) {}

        Vector<T, 4>  operator[](size_t i) const { return re[i]; }
        Vector<T, 4>& operator[](size_t i) { return re[i]; }

        Vector<T, 4> GetColumn(size_t i) const noexcept { return Vector<T, 4>(re[0][i], re[1][i], re[2][i], re[3][i]); }

        static Matrix<T, 4, 4> Identity() noexcept {
            Matrix<T, 4, 4> identity;
            identity[0][0] = (T)1;
            identity[1][1] = (T)1;
            identity[2][2] = (T)1;
            identity[3][3] = (T)1;
            return identity;
        }
    };

    // clang-format off
    template<typename T> struct IsMatrix { static constexpr bool value = false; };
    template<NumericType T> struct IsMatrix<Matrix<T, 2, 2>> { static constexpr bool value = true; };
    template<NumericType T> struct IsMatrix<Matrix<T, 3, 3>> { static constexpr bool value = true; };
    template<NumericType T> struct IsMatrix<Matrix<T, 3, 4>> { static constexpr bool value = true; };
    template<NumericType T> struct IsMatrix<Matrix<T, 4, 4>> { static constexpr bool value = true; };

    template<typename T> concept MatrixType = IsMatrix<T>::value;

    template<size_t ROW, size_t COL> using Matrixi  = Matrix<int, ROW, COL>;
    template<size_t ROW, size_t COL> using Matrixf  = Matrix<float, ROW, COL>;
    template<size_t ROW, size_t COL> using Matrixd  = Matrix<double, ROW, COL>;
    template<size_t ROW, size_t COL> using Matrixui = Matrix<unsigned int, ROW, COL>;

    using Matrix2x2i  = Matrix<int, 2, 2>;
    using Matrix3x3i  = Matrix<int, 3, 3>;
    using Matrix3x4i  = Matrix<int, 3, 4>;
    using Matrix4x4i  = Matrix<int, 4, 4>;
    using Matrix2x2f  = Matrix<float, 2, 2>;
    using Matrix3x3f  = Matrix<float, 3, 3>;
    using Matrix3x4f  = Matrix<float, 3, 4>;
    using Matrix4x4f  = Matrix<float, 4, 4>;
    using Matrix2x2d  = Matrix<double, 2, 2>;
    using Matrix3x3d  = Matrix<double, 3, 3>;
    using Matrix3x4d  = Matrix<double, 3, 4>;
    using Matrix4x4d  = Matrix<double, 4, 4>;
    using Matrix2x2ui = Matrix<unsigned int, 2, 2>;
    using Matrix3x3ui = Matrix<unsigned int, 3, 3>;
    using Matrix3x4ui = Matrix<unsigned int, 3, 4>;
    using Matrix4x4ui = Matrix<unsigned int, 4, 4>;

    // clang-format on
}// namespace Moer