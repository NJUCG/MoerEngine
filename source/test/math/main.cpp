#include <iostream>
#include <assert.h>
#include <random>
#include <iomanip>
#include "math/Math.h"

template<Moer::VectorType T>
std::ostream& operator<<(std::ostream& out, const T& v) {
    if constexpr (T::size == 2)
        out << "(" << v[0] << ", " << v[1] << ")";
    else if constexpr (T::size == 3)
        out << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
    else if constexpr (T::size == 4)
        out << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
    return out;
}

template<Moer::MatrixType T>
std::ostream& operator<<(std::ostream& out, const T& m) {
    for (int i = 0; i < T::row_size; i++) {
        for (int j = 0; j < T::col_size; j++) std::cout << m[i][j] << " ";
        std::cout << "\n";
    }
    return out;
}

float GetRandom() {
    static std::random_device                    rd;
    static std::mt19937                          mt(rd());
    static std::uniform_real_distribution<float> dist(-1, 1);
    // static std::normal_distribution<float> dist(0, 0.00001);

    return dist(mt);
}

int GetRandomInt() {
    return (int)(GetRandom() * 100000);
}

float GetRandomFloat() {
    return GetRandom() * 100000;
}

double GetRandomDouble() {
    return (double)GetRandom() * 100000;
}

// #define TestAssert(a, b) \
//     if (!(Moer::ApproxEqual(a, b, 0.00001f))) printf("failed: %s %s\n %.6f != %.6f\n", #a, #b, (float)(a), (float)(b))

#define TestAssert(a, b)                                                                                                                      \
    if (!(Moer::ApproxEqual(a, b, 0.0001f))) std::cout << std::setprecision(6) << typeid(T).name() << "\nfailed: " << #a << " " << #b << "\n" \
                                                       << ((a) - (b)) << "\n."
// #define TestAssert(a) (a)

template<typename T>
void TestMaxMin(T a, T b, T c, T d, T e, T f, T g, T h) {

    TestAssert(Moer::Max(a, b), std::max(a, b));
    TestAssert(Moer::Max(Moer::Vector<T, 2>{a, b}, Moer::Vector<T, 2>{c, d}), (Moer::Vector<T, 2>{std::max(a, c), std::max(b, d)}));
    TestAssert(Moer::Max(Moer::Vector<T, 3>{a, c, d}, Moer::Vector<T, 3>{e, f, g}), (Moer::Vector<T, 3>{std::max(a, e), std::max(c, f), std::max(d, g)}));
    TestAssert(Moer::Max(Moer::Vector<T, 4>{a, b, c, d}, Moer::Vector<T, 4>{e, f, g, h}), (Moer::Vector<T, 4>{std::max(a, e), std::max(b, f), std::max(c, g), std::max(d, h)}));

    TestAssert(Moer::Min(c, d), std::min(c, d));
    TestAssert(Moer::Min(Moer::Vector<T, 2>{a, b}, Moer::Vector<T, 2>{c, d}), (Moer::Vector<T, 2>{std::min(a, c), std::min(b, d)}));
    TestAssert(Moer::Min(Moer::Vector<T, 3>{a, c, d}, Moer::Vector<T, 3>{e, f, g}), (Moer::Vector<T, 3>{std::min(a, e), std::min(c, f), std::min(d, g)}));
    TestAssert(Moer::Min(Moer::Vector<T, 4>{a, b, c, d}, Moer::Vector<T, 4>{e, f, g, h}), (Moer::Vector<T, 4>{std::min(a, e), std::min(b, f), std::min(c, g), std::min(d, h)}));
}

template<typename T, typename U>
void TestOp(T a, T b, T c, T d, U e, U f, U g, U h) {
    Moer::Vector<T, 2> x2t{a, b};
    Moer::Vector<T, 3> x3t{a, b, c};
    Moer::Vector<T, 4> x4t{a, b, c, d};
    Moer::Vector<U, 2> x2u{e, f};
    Moer::Vector<U, 3> x3u{e, f, g};
    Moer::Vector<U, 4> x4u{e, f, g, h};

    TestAssert((x2t + x2u), (Moer::Vector<T, 2>(a + e, b + f)));
    TestAssert((x2t - x2u), (Moer::Vector<T, 2>(a - e, b - f)));
    TestAssert((x2t * x2u), (Moer::Vector<T, 2>(a * e, b * f)));
    TestAssert((x2t / x2u), (Moer::Vector<T, 2>(a / e, b / f)));
    TestAssert((x2u + x2t), (Moer::Vector<U, 2>(e + a, f + b)));
    TestAssert((x2u - x2t), (Moer::Vector<U, 2>(e - a, f - b)));
    TestAssert((x2u * x2t), (Moer::Vector<U, 2>(e * a, f * b)));
    TestAssert((x2u / x2t), (Moer::Vector<U, 2>(e / a, f / b)));
    TestAssert((x2t + g), (Moer::Vector<T, 2>(a + g, b + g)));
    TestAssert((x2t - g), (Moer::Vector<T, 2>(a - g, b - g)));
    TestAssert((x2t * g), (Moer::Vector<T, 2>(a * g, b * g)));
    TestAssert((x2t / g), (Moer::Vector<T, 2>(a / g, b / g)));
    TestAssert((d + x2u), (Moer::Vector<U, 2>(d + e, d + f)));
    TestAssert((d - x2u), (Moer::Vector<U, 2>(d - e, d - f)));
    TestAssert((d * x2u), (Moer::Vector<U, 2>(d * e, d * f)));
    TestAssert((d / x2u), (Moer::Vector<U, 2>(d / e, d / f)));

    TestAssert((x3t + x3u), (Moer::Vector<T, 3>(a + e, b + f, c + g)));
    TestAssert((x3t - x3u), (Moer::Vector<T, 3>(a - e, b - f, c - g)));
    TestAssert((x3t * x3u), (Moer::Vector<T, 3>(a * e, b * f, c * g)));
    TestAssert((x3t / x3u), (Moer::Vector<T, 3>(a / e, b / f, c / g)));
    TestAssert((x3u + x3t), (Moer::Vector<U, 3>(e + a, f + b, g + c)));
    TestAssert((x3u - x3t), (Moer::Vector<U, 3>(e - a, f - b, g - c)));
    TestAssert((x3u * x3t), (Moer::Vector<U, 3>(e * a, f * b, g * c)));
    TestAssert((x3u / x3t), (Moer::Vector<U, 3>(e / a, f / b, g / c)));
    TestAssert((x3t + g), (Moer::Vector<T, 3>(a + g, b + g, c + g)));
    TestAssert((x3t - g), (Moer::Vector<T, 3>(a - g, b - g, c - g)));
    TestAssert((x3t * g), (Moer::Vector<T, 3>(a * g, b * g, c * g)));
    TestAssert((x3t / g), (Moer::Vector<T, 3>(a / g, b / g, c / g)));
    TestAssert((d + x3u), (Moer::Vector<U, 3>(d + e, d + f, d + g)));
    TestAssert((d - x3u), (Moer::Vector<U, 3>(d - e, d - f, d - g)));
    TestAssert((d * x3u), (Moer::Vector<U, 3>(d * e, d * f, d * g)));
    TestAssert((d / x3u), (Moer::Vector<U, 3>(d / e, d / f, d / g)));

    TestAssert((x4t + x4u), (Moer::Vector<T, 4>(a + e, b + f, c + g, d + h)));
    TestAssert((x4t - x4u), (Moer::Vector<T, 4>(a - e, b - f, c - g, d - h)));
    TestAssert((x4t * x4u), (Moer::Vector<T, 4>(a * e, b * f, c * g, d * h)));
    TestAssert((x4t / x4u), (Moer::Vector<T, 4>(a / e, b / f, c / g, d / h)));
    TestAssert((x4u + x4t), (Moer::Vector<U, 4>(e + a, f + b, g + c, h + d)));
    TestAssert((x4u - x4t), (Moer::Vector<U, 4>(e - a, f - b, g - c, h - d)));
    TestAssert((x4u * x4t), (Moer::Vector<U, 4>(e * a, f * b, g * c, h * d)));
    TestAssert((x4u / x4t), (Moer::Vector<U, 4>(e / a, f / b, g / c, h / d)));
    TestAssert((x4t + g), (Moer::Vector<T, 4>(a + g, b + g, c + g, d + g)));
    TestAssert((x4t - g), (Moer::Vector<T, 4>(a - g, b - g, c - g, d - g)));
    TestAssert((x4t * g), (Moer::Vector<T, 4>(a * g, b * g, c * g, d * g)));
    TestAssert((x4t / g), (Moer::Vector<T, 4>(a / g, b / g, c / g, d / g)));
    TestAssert((d + x4u), (Moer::Vector<U, 4>(d + e, d + f, d + g, d + h)));
    TestAssert((d - x4u), (Moer::Vector<U, 4>(d - e, d - f, d - g, d - h)));
    TestAssert((d * x4u), (Moer::Vector<U, 4>(d * e, d * f, d * g, d * h)));
    TestAssert((d / x4u), (Moer::Vector<U, 4>(d / e, d / f, d / g, d / h)));

    // clang-format off
    x2t += x2u; TestAssert((x2t), (Moer::Vector<T, 2>(a + e, b + f)));
    x2t -= x2u; TestAssert((x2t), (Moer::Vector<T, 2>(a + e - e, b + f - f)));
    x2t *= x2u; TestAssert((x2t), (Moer::Vector<T, 2>(a * e, b * f)));
    x2t /= x2u; TestAssert((x2t), (Moer::Vector<T, 2>(a * e / e, b * f / f)));
    x2t += h; TestAssert((x2t), (Moer::Vector<T, 2>(a + h, b + h)));
    x2t -= h; TestAssert((x2t), (Moer::Vector<T, 2>(a, b)));
    x2t *= h; TestAssert((x2t), (Moer::Vector<T, 2>(a * h, b * h)));
    x2t /= h; TestAssert((x2t), (Moer::Vector<T, 2>(a * h / h, b * h / h)));

    x3t += x3u; TestAssert((x3t), (Moer::Vector<T, 3>(a + e, b + f, c + g)));
    x3t -= x3u; TestAssert((x3t), (Moer::Vector<T, 3>(a, b, c)));
    x3t *= x3u; TestAssert((x3t), (Moer::Vector<T, 3>(a * e, b * f, c * g)));
    x3t /= x3u; TestAssert((x3t), (Moer::Vector<T, 3>(a * e / e, b * f / f, c * g / g)));
    x3t += h; TestAssert((x3t), (Moer::Vector<T, 3>(a + h, b + h, c + h)));
    x3t -= h; TestAssert((x3t), (Moer::Vector<T, 3>(a, b, c)));
    x3t *= h; TestAssert((x3t), (Moer::Vector<T, 3>(a * h, b * h, c * h)));
    x3t /= h; TestAssert((x3t), (Moer::Vector<T, 3>(a * h / h, b * h / h, c * h / h)));

    x4t += x4u; TestAssert((x4t), (Moer::Vector<T, 4>(a + e, b + f, c + g, d + h)));
    x4t -= x4u; TestAssert((x4t), (Moer::Vector<T, 4>(a, b, c, d)));
    x4t *= x4u; TestAssert((x4t), (Moer::Vector<T, 4>(a * e, b * f, c * g, d * h)));
    x4t /= x4u; TestAssert((x4t), (Moer::Vector<T, 4>(a * e / e, b * f / f, c * g / g, d * h / h)));
    x4t += h; TestAssert((x4t), (Moer::Vector<T, 4>(a + h, b + h, c + h, d + h)));
    x4t -= h; TestAssert((x4t), (Moer::Vector<T, 4>(a, b, c, d)));
    x4t *= h; TestAssert((x4t), (Moer::Vector<T, 4>(a * h, b * h, c * h, d * h)));
    x4t /= h; TestAssert((x4t), (Moer::Vector<T, 4>(a * h / h, b * h / h, c * h / h, d * h / h)));
    // clang-format on
}

template<typename T>
void TestClamp(T a, T b, T c, T d) {
    Moer::Vector<T, 2> x2t{a, b};
    Moer::Vector<T, 3> x3t{a, b, c};
    Moer::Vector<T, 4> x4t{a, b, c, d};

    TestAssert(Moer::Clamp(a, a - 1, a + 1), a);
    TestAssert(Moer::Clamp(a, a + 1, a + 2), a + 1);
    TestAssert(Moer::Clamp(a, a - 2, a - 1), a - 1);

    int dl[] = {-1, 1, -2};
    int dr[] = {1, 2, -1};
    int dm[] = {0, 1, -1};
    for (int i = 0; i < 3; i++) {
        TestAssert(Moer::Clamp(a, a + dl[i], a + dr[i]), a + dm[i]);
    }

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            TestAssert(Moer::Clamp(x2t, x2t + Moer::Vector<T, 2>(dl[i], dl[j]), x2t + Moer::Vector<T, 2U>(dr[i], dr[j])), (x2t + Moer::Vector<T, 2U>(dm[i], dm[j])));
        }
    }

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++) {
                TestAssert(Moer::Clamp(x3t, x3t + Moer::Vector<T, 3>(dl[i], dl[j], dl[k]), x3t + Moer::Vector<T, 3>(dr[i], dr[j], dr[k])), (x3t + Moer::Vector<T, 3>(dm[i], dm[j], dm[k])));
            }
        }
    }

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++) {
                for (int o = 0; o < 3; o++) {
                    TestAssert(
                        Moer::Clamp(
                            x4t, x4t + Moer::Vector<T, 4>(dl[i], dl[j], dl[k], dl[o]), x4t + Moer::Vector<T, 4>(dr[i], dr[j], dr[k], dr[o])),
                        (x4t + Moer::Vector<T, 4>(dm[i], dm[j], dm[k], dm[o])));
                }
            }
        }
    }
}

#include <DirectXMath.h>

int main() {

    Moer::Vector4f vec_4f(2.f, 1.f, 3.f, 4.f);

    Moer::Vector2f vec_2f(vec_4f);

    std::cout << vec_2f << "\n"
              << vec_4f << "\n";

    for (int i = 0; i < 1; i++) {
        TestMaxMin(GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt());
        TestMaxMin(GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat());
        TestMaxMin(GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble());

        // TestOp(GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt());
        // TestOp(GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat());
        // TestOp(GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt());
        // TestOp(GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble());
        TestOp(GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble());
        TestOp(GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat());

        TestClamp(GetRandomInt(), GetRandomInt(), GetRandomInt(), GetRandomInt());
        TestClamp(GetRandomFloat(), GetRandomFloat(), GetRandomFloat(), GetRandomFloat());
        TestClamp(GetRandomDouble(), GetRandomDouble(), GetRandomDouble(), GetRandomDouble());
    }

    auto x = Moer::Matrix2x2f(Moer::Vector2f(1.f), Moer::Vector2f(2.f));
    auto y = Moer::Matrix2x2f(Moer::Vector2f(2.f), Moer::Vector2f(2.f));

    x += y;
    std::cout << x << "\n";
    x = x + x;
    std::cout << x << "\n";
    x = x - y;
    std::cout << x << "\n";
    x /= y;
    std::cout << x << "\n";

    auto v = Moer::Vector2f(2.5f, 1.2f);

    std::cout << (x * v) << "\n";
    std::cout << (v * x) << "\n";

    float me[16];
    for (int i = 1; i < 17; i++) me[i - 1] = GetRandomFloat();
    me[15] = 1.f;
    me[14] = 0.f;
    me[13] = 0.f;
    me[12] = 0.f;
    DirectX::XMFLOAT4X4 dx_m(me);

    Moer::Matrix4x4f moer_m(me);
    std::cout << moer_m;

    auto                dx_xm_m     = DirectX::XMLoadFloat4x4(&dx_m);
    auto                inv_dx_xm_m = DirectX::XMMatrixInverse(nullptr, dx_xm_m);
    DirectX::XMFLOAT4X4 inv_dx_m;
    DirectX::XMStoreFloat4x4(&inv_dx_m, inv_dx_xm_m);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) std::cout << inv_dx_m.m[i][j] << " ";
        std::cout << std::endl;
    }

    auto inv_moer_m = Moer::Inverse(moer_m);
    std::cout << inv_moer_m;

    return 0;
}