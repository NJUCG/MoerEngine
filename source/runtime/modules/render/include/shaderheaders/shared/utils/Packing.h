#ifndef MOER_SHARED_PACKING_H
#define MOER_SHARED_PACKING_H

#ifdef __cplusplus
#include "misc/Traits.h"
#include <algorithm>
#define CONST       constexpr
#define GLOBAL_FUNC static
#else
#define CONST const
#define GLOBAL_FUNC
#endif

namespace Moer {

#ifdef __cplusplus
#include "math/Math.h"
    template<typename T>
    static T clamp(const T& v, const T& lo, const T& hi) {
        if constexpr (std::is_floating_point_v<T>) {
            return std::clamp(v, lo, hi);
        } else {
            return Clamp(v, T(lo), T(hi));
        }
    }

    template<typename T>
    static T saturate(const T& v) {
        return clamp(v, T(0), T(1));
    }

    template<typename T, typename U>
    static T pow(const T& v, U e) {
        if constexpr (std::is_floating_point_v<T>) {
            return std::pow(v, T(e));
        } else {
            return Pow(v, T(e));
        }
    }

    static uint f32tof16(float _f) {
        uint32_t u        = *(uint32_t*)&_f;
        uint32_t sign     = (u & 0x80000000) >> 16;
        uint32_t exponent = (u & 0x7F800000) >> 13;
        uint32_t mantissa = (u & 0x007FFFFF);
        if (exponent == 0) {
            return sign;
        }
        if (exponent == 0xFF) {
            return sign | 0x7C00 | (mantissa >> 13);
        }
        return sign | ((exponent - 112) << 10) | (mantissa >> 13);
    }

    static float f16tof32(uint _h) {
        uint32_t sign     = (_h & 0x8000) << 16;
        uint32_t exponent = ((_h & 0x7C00) << 13) + 0x38000000;
        uint32_t mantissa = (_h & 0x03FF) << 13;
        uint32_t u        = sign | exponent | mantissa;
        return *(float*)&u;
    }

    static float2 f16tof32(uint2 _h) {
        return float2(f16tof32(_h.x), f16tof32(_h.y));
    }
    namespace Math {
        float2 OctWarp(float2 _v) {
            return (1.f - Abs(float2(_v.y, _v.x))) * Select(_v - float2(0.f), float2(1.f), float2(-1.f));
        }

        float2 NdirToOctSigned(float3 _v) {
            float2 _v2 = _v.xy * (1.f / (abs(_v.x) + abs(_v.y) + abs(_v.z)));
            return _v.z < 0.f ? OctWarp(_v2) : _v2;
        }

        float3 OctToNdirSigned(float2 _p) {
            float3 n = float3(_p.x, _p.y, 1.f - abs(_p.x) - abs(_p.y));
            float  t = Max(-n.z, 0.f);
            n.xy += Select(n.xy, float2(t), float2(-t));// flip xy back alone corresponding
                                                        // diagnal

            return Normalizef(n);
        }

        int NdirToOctUnorm32(float3 _v) {
            float2 p = NdirToOctSigned(_v) * 0.5f + 0.5f;
            return int(p.x * 65535.f) | (int(p.y * 65535.f) << 16);
        }

        float3 OctToNdirUnorm32(uint _p) {
            float2 p = float2(float(_p & 0xffff), float(_p >> 16)) * (1.f / 65535.f);
            return OctToNdirSigned(p * 2.f - 1.f);
        }

        float3 OctToNdir(float2 _p) {
            return OctToNdirSigned(_p * 2.f - 1.f);
        }

        float2 NdirToOct(float3 _v) {
            return NdirToOctSigned(_v) * 0.5f + 0.5f;
        }
    }// namespace Math

#else

#endif

// Pack [0.0, 1.0] float to a uint of a given bit depth
#define PACK_UFLOAT_TEMPLATE(size)                                    \
    GLOBAL_FUNC uint Pack_R##size##_UFLOAT(float r, float d = 0.5f) { \
        const uint mask = (1U << size) - 1U;                          \
                                                                      \
        return (uint)floor(r * mask + d) & mask;                      \
    }                                                                 \
                                                                      \
    GLOBAL_FUNC float Unpack_R##size##_UFLOAT(uint r) {               \
        const uint mask = (1U << size) - 1U;                          \
                                                                      \
        return (float)(r & mask) / (float)mask;                       \
    }

    PACK_UFLOAT_TEMPLATE(8)
    PACK_UFLOAT_TEMPLATE(10)
    PACK_UFLOAT_TEMPLATE(11)
    PACK_UFLOAT_TEMPLATE(16)

    GLOBAL_FUNC uint Pack_R8G8B8_UFLOAT(float3 rgb, float3 d = float3(0.5f, 0.5f, 0.5f)) {
        uint r = Pack_R8_UFLOAT(rgb.r, d.r);
        uint g = Pack_R8_UFLOAT(rgb.g, d.g) << 8;
        uint b = Pack_R8_UFLOAT(rgb.b, d.b) << 16;
        return r | g | b;
    }

    GLOBAL_FUNC float3 Unpack_R8G8B8_UFLOAT(uint rgb) {
        float r = Unpack_R8_UFLOAT(rgb);
        float g = Unpack_R8_UFLOAT(rgb >> 8);
        float b = Unpack_R8_UFLOAT(rgb >> 16);
        return float3(r, g, b);
    }

    GLOBAL_FUNC uint Pack_R8G8B8A8_Gamma_UFLOAT(float4 rgba, float gamma = 2.2, float4 d = float4(0.5f, 0.5f, 0.5f, 0.5f)) {
        rgba   = pow(saturate(rgba), 1.f / gamma);
        uint r = Pack_R8_UFLOAT(rgba.r, d.r);
        uint g = Pack_R8_UFLOAT(rgba.g, d.g) << 8;
        uint b = Pack_R8_UFLOAT(rgba.b, d.b) << 16;
        uint a = Pack_R8_UFLOAT(rgba.a, d.a) << 24;
        return r | g | b | a;
    }

    GLOBAL_FUNC float4 Unpack_R8G8B8A8_Gamma_UFLOAT(uint rgba, float gamma = 2.2) {
        float  r = Unpack_R8_UFLOAT(rgba);
        float  g = Unpack_R8_UFLOAT(rgba >> 8);
        float  b = Unpack_R8_UFLOAT(rgba >> 16);
        float  a = Unpack_R8_UFLOAT(rgba >> 24);
        float4 v = float4(r, g, b, a);
        v        = pow(saturate(v), gamma);
        return v;
    }

    GLOBAL_FUNC uint Pack_R11G11B10_UFLOAT(float3 rgb, float3 d = float3(0.5f, 0.5f, 0.5f)) {
        uint r = Pack_R11_UFLOAT(rgb.r, d.r);
        uint g = Pack_R11_UFLOAT(rgb.g, d.g) << 11;
        uint b = Pack_R10_UFLOAT(rgb.b, d.b) << 22;
        return r | g | b;
    }

    GLOBAL_FUNC float3 Unpack_R11G11B10_UFLOAT(uint rgb) {
        float r = Unpack_R11_UFLOAT(rgb);
        float g = Unpack_R11_UFLOAT(rgb >> 11);
        float b = Unpack_R10_UFLOAT(rgb >> 22);
        return float3(r, g, b);
    }

    GLOBAL_FUNC uint Pack_R8G8B8A8_UFLOAT(float4 rgba, float4 d = float4(0.5f, 0.5f, 0.5f, 0.5f)) {
        uint r = Pack_R8_UFLOAT(rgba.r, d.r);
        uint g = Pack_R8_UFLOAT(rgba.g, d.g) << 8;
        uint b = Pack_R8_UFLOAT(rgba.b, d.b) << 16;
        uint a = Pack_R8_UFLOAT(rgba.a, d.a) << 24;
        return r | g | b | a;
    }

    GLOBAL_FUNC float4 Unpack_R8G8B8A8_UFLOAT(uint rgba) {
        float r = Unpack_R8_UFLOAT(rgba);
        float g = Unpack_R8_UFLOAT(rgba >> 8);
        float b = Unpack_R8_UFLOAT(rgba >> 16);
        float a = Unpack_R8_UFLOAT(rgba >> 24);
        return float4(r, g, b, a);
    }

    GLOBAL_FUNC uint Pack_R16G16_UFLOAT(float2 rg, float2 d = float2(0.5f, 0.5f)) {
        uint r = Pack_R16_UFLOAT(rg.r, d.r);
        uint g = Pack_R16_UFLOAT(rg.g, d.g) << 16;
        return r | g;
    }

    GLOBAL_FUNC float2 Unpack_R16G16_UFLOAT(uint rg) {
        float r = Unpack_R16_UFLOAT(rg);
        float g = Unpack_R16_UFLOAT(rg >> 16);
        return float2(r, g);
    }

    // Todo: FLOAT is not consistent with the rest of the naming here, they should be changed
    // to UNORM as they do not actually decode into full floats but are rather normalized unsigned
    // floats, whereas this should be a SFLOAT.
    GLOBAL_FUNC uint Pack_R16G16_FLOAT(float2 rg) {
        uint r = f32tof16(rg.r);
        uint g = f32tof16(rg.g) << 16;
        return r | g;
    }

    GLOBAL_FUNC uint2 Pack_R16G16B16A16_FLOAT(float4 rgba) {
        return uint2(Pack_R16G16_FLOAT(rgba.rg), Pack_R16G16_FLOAT(rgba.ba));
    }

    GLOBAL_FUNC float2 Unpack_R16G16_FLOAT(uint rg) {
        uint2 d = uint2(rg & 0xffff, rg >> 16);
        return f16tof32(d);
    }

    GLOBAL_FUNC float4 Unpack_R16G16B16A16_FLOAT(uint2 rgba) {
        return float4(Unpack_R16G16_FLOAT(rgba.x), Unpack_R16G16_FLOAT(rgba.y));
    }

    GLOBAL_FUNC uint Pack_R8_SNORM(float _value) {
        return int(clamp(_value, -1.f, 1.f) * 127.0) & 0xFF;
    }

    GLOBAL_FUNC float Unpack_R8_SNORM(uint _value) {
        int signed_value = int(_value << 24) >> 24;
        return clamp(float(signed_value) / 127.f, -1.f, 1.f);
    }

    // This function is deprecated. Use `Pack_Normal` instead.
    GLOBAL_FUNC uint Pack_RGB8_SNORM(float3 _value) {
        return Pack_R8_SNORM(_value.x) | (Pack_R8_SNORM(_value.y) << 8) | (Pack_R8_SNORM(_value.z) << 16);
    }

    // This function is deprecated. Use `Unpack_Normal` instead.
    GLOBAL_FUNC float3 Unpack_RGB8_SNORM(uint _value) {
        return float3(
            Unpack_R8_SNORM(_value & 0xFF),
            Unpack_R8_SNORM((_value >> 8) & 0xFF),
            Unpack_R8_SNORM((_value >> 16) & 0xFF));
    }

    GLOBAL_FUNC float3 Unpack_Normal(uint _val) {
        return Unpack_R11G11B10_UFLOAT(_val) * 2.f - 1.f;
    }

    GLOBAL_FUNC uint Pack_Normal(float3 _val) {
        return Pack_R11G11B10_UFLOAT(_val * .5f + .5f);
    }

    GLOBAL_FUNC float3 Unpack_NormalOct(float2 _val) {
        return Math::OctToNdir(_val);
    }

    GLOBAL_FUNC float2 Pack_NormalOct(float3 _normal) {
        return Math::NdirToOct(_normal);
    }

    GLOBAL_FUNC float3 Interpolate(float3 _val[3], float3 _bary) {
        return _val[0] * _bary.x + _val[1] * _bary.y + _val[2] * _bary.z;
    }

    GLOBAL_FUNC float2 Interpolate(float2 _val[3], float3 _bary) {
        return _val[0] * _bary.x + _val[1] * _bary.y + _val[2] * _bary.z;
    }

    GLOBAL_FUNC float4 Interpolate(float4 _val[3], float3 _bary) {
        return _val[0] * _bary.x + _val[1] * _bary.y + _val[2] * _bary.z;
    }
};// namespace Moer

#undef GLOBAL_FUNC
#undef CONST

#endif