#pragma once

#include <type_traits>
#include <cstdint>

namespace Moer {
    template<typename T>
    concept NumericType =
        requires(T param) {
            requires std::is_integral_v<T> || std::is_floating_point_v<T>;
            requires !std::is_same_v<bool, T>;
            requires std::is_arithmetic_v<decltype(param + 1)>;
            requires !std::is_pointer_v<T>;
        };

    template<NumericType T, size_t N>
        requires(N > 1 && N < 5)
    struct Vector {};

    template<NumericType T>
    struct Vector<T, 2> {
        union {
            struct {
                T x, y;
            };
            T e[2];
        };
        static constexpr size_t size = 2;

        Vector() noexcept : x(0), y(0) {}
        Vector(T x, T y) noexcept : x(x), y(y) {}
        Vector(const Vector<T, 2>& v) noexcept : x(v.x), y(v.y) {}

        explicit Vector(const Vector<T, 3>& v) noexcept : x(v.x), y(v.y) {}
        explicit Vector(const Vector<T, 4>& v) noexcept : x(v.x), y(v.y) {}
        explicit Vector(T xy) noexcept : x(xy), y(xy) {}

        template<NumericType U>
        explicit Vector(const Vector<U, 2>& v) noexcept : x(v.x), y(v.y) {}

        T  operator[](size_t i) const { return e[i]; }
        T& operator[](size_t i) { return e[i]; }
    };

    template<NumericType T>
    struct Vector<T, 3> {
        union {
            struct {
                T x, y, z;
            };
            T e[3];
        };
        static constexpr size_t size = 3;

        Vector() noexcept : x(0), y(0), z(0) {}
        Vector(T x, T y, T z) noexcept : x(x), y(y), z(z) {}
        Vector(const Vector<T, 3>& v) noexcept : x(v.x), y(v.y), z(v.z) {}

        explicit Vector(const Vector<T, 2>& v, T z = 0) noexcept : x(v.x), y(v.y), z(z) {}
        explicit Vector(const Vector<T, 4>& v) noexcept : x(v.x), y(v.y), z(v.z) {}
        explicit Vector(T xyz) noexcept : x(xyz), y(xyz), z(xyz) {}

        template<NumericType U>
        explicit Vector(const Vector<U, 3>& v) noexcept : x(v.x), y(v.y), z(v.z) {}

        T  operator[](size_t i) const { return e[i]; }
        T& operator[](size_t i) { return e[i]; }
    };

    template<NumericType T>
    struct Vector<T, 4> {
        union {
            struct {
                T x, y, z, w;
            };
            T e[4];
        };
        static constexpr size_t size = 4;

        Vector() noexcept : x(0), y(0), z(0), w(0) {}
        Vector(T x, T y, T z, T w) noexcept : x(x), y(y), z(z), w(w) {}
        Vector(const Vector<T, 4>& v) noexcept : x(v.x), y(v.y), z(v.z), w(v.w) {}

        explicit Vector(const Vector<T, 2>& v, T z = 0, T w = 0) noexcept : x(v.x), y(v.y), z(z), w(w) {}
        explicit Vector(const Vector<T, 3>& v, T w = 0) noexcept : x(v.x), y(v.y), z(v.z), w(w) {}
        explicit Vector(T xyzw) noexcept : x(xyzw), y(xyzw), z(xyzw), w(xyzw) {}

        template<NumericType U>
        explicit Vector(const Vector<U, 4>& v) noexcept : x(v.x), y(v.y), z(v.z), w(v.w) {}

        T  operator[](size_t i) const { return e[i]; }
        T& operator[](size_t i) { return e[i]; }
    };

    // clang-format off
    template<typename T> struct IsVectorType { static constexpr bool value = false; };
    template<NumericType T> struct IsVectorType<Vector<T, 2>> { static constexpr bool value = true; };
    template<NumericType T> struct IsVectorType<Vector<T, 3>> { static constexpr bool value = true; };
    template<NumericType T> struct IsVectorType<Vector<T, 4>> { static constexpr bool value = true; };

    template<typename T> concept VectorType = IsVectorType<T>::value;

    template<size_t N> using Vectori  = Vector<int, N>;
    template<size_t N> using Vectorf  = Vector<float, N>;
    template<size_t N> using Vectord  = Vector<double, N>;
    template<size_t N> using Vectorui = Vector<unsigned int, N>;

    using Vector2i  = Vector<int, 2>;
    using Vector3i  = Vector<int, 3>;
    using Vector4i  = Vector<int, 4>;
    using Vector2f  = Vector<float, 2>;
    using Vector3f  = Vector<float, 3>;
    using Vector4f  = Vector<float, 4>;
    using Vector2d  = Vector<double, 2>;
    using Vector3d  = Vector<double, 3>;
    using Vector4d  = Vector<double, 4>;
    using Vector2ui = Vector<unsigned int, 2>;
    using Vector3ui = Vector<unsigned int, 3>;
    using Vector4ui = Vector<unsigned int, 4>;

    // clang-format on
}// namespace Moer

namespace Moer {
    // angle of a radian system
    struct Angle {
        float radian;

        Angle(float radian = 0.f) noexcept : radian(radian) {}
        operator float() const noexcept { return radian; }

        void SetDegree(float degree) noexcept { this->radian = degree / 180.f * 3.14159265358979323846f; }
        void SetRadian(float radian) noexcept { this->radian = radian; }

        float GetDegree() const noexcept { return radian * 180.f / 3.14159265358979323846f; }
        float GetRadian() const noexcept { return radian; }

        static Angle MakeFromRadian(float radian) noexcept { return Angle(radian); }
        static Angle MakeFromDegree(float degree) noexcept { return Angle(DegreeToRadian(degree)); }
        static float DegreeToRadian(float degree) noexcept { return degree / 180.f * 3.14159265358979323846f; }
        static float RadianToDegree(float radian) noexcept { return radian * 180.f / 3.14159265358979323846f; }
    };
}// namespace Moer
