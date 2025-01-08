#ifndef MOER_MATH_HLSL
#define MOER_MATH_HLSL
#include <MathLib/STL.hlsli>

#define PI 3.1415926535897932384626433832795

template <typename T> T square(T x) { return x * x; }
namespace Math {

#define _Pi(x) radians(180.0 * x)

float Pi(float x) { return _Pi(x); }

float2 Pi(float2 x) { return _Pi(x); }

float3 Pi(float3 x) { return _Pi(x); }

float4 Pi(float4 x) { return _Pi(x); }

#define _RadToDeg(x) (x * 180.0 / Pi(1.0))

float RadToDeg(float x) { return _RadToDeg(x); }

float2 RadToDeg(float2 x) { return _RadToDeg(x); }

float3 RadToDeg(float3 x) { return _RadToDeg(x); }

float4 RadToDeg(float4 x) { return _RadToDeg(x); }

#define _DegToRad(x) (x * Pi(1.0) / 180.0)

float DegToRad(float x) { return _DegToRad(x); }

float2 DegToRad(float2 x) { return _DegToRad(x); }

float3 DegToRad(float3 x) { return _DegToRad(x); }

float4 DegToRad(float4 x) { return _DegToRad(x); }

float square(float x) { return x * x; }

float2 square(float2 x) { return x * x; }

float3 square(float3 x) { return x * x; }

float4 square(float4 x) { return x * x; }

float3 slerp(float3 a, float3 b, float angle, float t) {
  t = saturate(t);
  float sin1 = sin(angle * t);
  float sin2 = sin(angle * (1.f - t));
  float ta = sin1 / (sin1 + sin2);
  return normalize(lerp(a, b, ta));
}

float copysign(float x, float y) {
  uint xi = asint(x);
  uint yi = asint(y);

  xi = (xi & 0x7fffffff) | (yi & 0x80000000);
  return asfloat(xi);
}

float snz(float x) { return (x >= 0.f) ? 1.f : -1.f; }

float2 snz(float2 x) { return float2(snz(x.x), snz(x.y)); }

float3 snz(float3 x) { return float3(snz(x.x), snz(x.y), snz(x.z)); }

float4 snz(float4 x) { return float4(snz(x.x), snz(x.y), snz(x.z), snz(x.w)); }
// flip xy alone oct diagnals
float2 OctWarp(float2 _v) {
  return (1.f - abs(_v.yx)) * (select(_v.xy > 0.f, 1.f, -1.f));
}

float2 NdirToOctSigned(float3 _v) {
  float2 _v2 = _v.xy * (1.f / (abs(_v.x) + abs(_v.y) + abs(_v.z)));
  return _v.z < 0.f ? OctWarp(_v2) : _v2;
}

float3 OctToNdirSigned(float2 _p) {
  float3 n = float3(_p.x, _p.y, 1.f - abs(_p.x) - abs(_p.y));
  float t = max(-n.z, 0.f);
  n.xy += select(n.xy >= 0.f, -t, t); // flip xy back alone corresponding
                                      // diagnal

  return normalize(n);
}

int NdirToOctUnorm32(float3 _v) {
  float2 p = NdirToOctSigned(_v) * 0.5f + 0.5f;
  return int(p.x * 65535.f) | (int(p.y * 65535.f) << 16);
}

float3 OctToNdirUnorm32(int _p) {
  float2 p = float2(_p & 0xffff, _p >> 16) * (1.f / 65535.f);
  return OctToNdirSigned(p * 2.f - 1.f);
}

/*https://graphics.pixar.com/library/OrthonormalB/paper.pdf*/
void BranchlessONB(in float3 n, out float3 b1, out float3 b2) {
  float sign = n.z >= 0.0f ? 1.0f : -1.0f;
  float a = -1.0f / (sign + n.z);
  float b = n.x * n.y * a;
  b1 = float3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
  b2 = float3(b, sign + n.y * n.y * a, -n.y);
}

float3 SphericalDirection(float3 x, float3 y, float3 z, float sintheta,
                          float costheta, float sinphi, float cosphi) {
  return x * sintheta * cosphi + y * sintheta * sinphi + z * costheta;
}

float2 SampleDisk(float2 rand) {
  float angle = 2 * PI * rand.x;
  return float2(cos(angle), sin(angle)) * sqrt(rand.y);
}

float3 SampleTriangle(float2 rand) {
  float u = sqrt(rand.x);
  return float3(1 - u, u * rand.y, u * (1 - rand.y));
}

float3 SampleHemisphere(float2 rand) {
  float z = rand.x;
  float r = sqrt(max(0.f, 1.f - z * z));
  float phi = 2 * PI * rand.y;
  return float3(r * cos(phi), r * sin(phi), z);
}

float3 SampleHemisphereCosine(float2 rand) {
  float2 disk = SampleDisk(rand);
  float z = sqrt(max(0.f, 1.f - rand.y));
  return float3(disk, z);
}

float3 SampleHemisphereCosineWithPdf(float2 rand, out float pdf) {
  float2 disk = SampleDisk(rand);
  float z = sqrt(max(0.f, 1.f - rand.y));
  pdf = z / PI;
  return float3(disk, z);
}

float3 SampleSphere(float2 _rnd, out float _pdf) {
  float z = 1.f - 2.f * _rnd.x;
  float r = sqrt(max(0.f, 1.f - z * z));
  float phi = 2.f * PI * _rnd.y;
  _pdf = 1.f / (4.f * PI);
  return float3(r * cos(phi), r * sin(phi), z);
}

float3 EquirectangularUVToDir(float2 _uv, out float _cos) {
  float theta = _uv.x * 2.f * PI;
  float phi = (_uv.y - 0.5f) * PI;

  _cos = cos(phi);
  return float3(_cos * sin(theta), sin(phi), cos(theta) * _cos);
}

float2 DirToEquirectangularUV(float3 _dir) {
  float phi = asin(_dir.y);
  float theta = 0.f;
  if (abs(_dir.y) < 1.f)
    theta = atan2(_dir.z, _dir.x);

  float2 uv;
  uv.x = theta / (2.f * PI);
  uv.y = phi / PI + 0.5f;
  return uv;
}

float3 Rand2ToBaryCentrics(float2 _uv) {
  float sqrt_x = sqrt(_uv.x);
  return float3(1.0 - sqrt_x, _uv.y * sqrt_x, (1.0 - _uv.y) * sqrt_x);
}

float3 HitUVToBarycentrics(float2 _uv) {
  return float3(1.0 - _uv.x - _uv.y, _uv.x, _uv.y);
}

float2 BaryCentricsToRand2(float3 _bary) {
  float sqrt_x = 1.f - _bary.x;
  return float2(sqrt_x * sqrt_x, _bary.y / sqrt_x);
}

// Random State

namespace Rng {

struct Tea {
  uint2 val;

  void Init(uint _linear_idx, uint _frame_idx, uint _spin_num = 16) {
    val.x = _linear_idx;
    val.y = _frame_idx;

    uint s = 0;
    [unroll] for (uint n = 0; n < _spin_num; n++) {
      s += 0x9E3779B9;
      val.x += ((val.y << 4) + 0xA341316C) ^ (val.y + s) ^
               ((val.y >> 5) + 0xC8013EA4);
      val.y += ((val.x << 4) + 0xAD90777D) ^ (val.x + s) ^
               ((val.x >> 5) + 0x7E95761E);
    }
  }

  void Init(uint2 _pos, uint _frame_idx) {
    Init(STL::Sequence::Zorder(_pos), _frame_idx);
  }

  uint GetUint() {
    val.x = STL::Rng::_Next(val.x);

    return val.x;
  }

  uint2 GetUint2() {
    val.x = STL::Rng::_Next(val.x);
    val.y = STL::Rng::_Next(val.y);

    return val;
  }

  uint4 GetUint4() { return float4(GetUint2(), GetUint2()); }

  float GetFloat() {
    uint x = GetUint();
    return _UintToFloat01(x);
  }

  float2 GetFloat2() {
    uint2 x = GetUint2();
    return _UintToFloat01(x);
  }

  float4 GetFloat4() {
    uint4 x = GetUint4();
    return _UintToFloat01(x);
  }
};

struct Hash {
  uint state;

  void Init(uint _linear_idx, uint _frame_idx) {
    state = STL::Sequence::HashCombine(
        STL::Sequence::Hash(_frame_idx + 0x035F9F29), _linear_idx);
  }

  void Initialize(uint2 _pos, uint _frame_idx) {
    Initialize(STL::Sequence::Zorder(_pos), _frame_idx);
  }

  uint GetUint() {
    state = STL::Rng::_Next(state);

    return state;
  }

  uint2 GetUint2() { return uint2(GetUint(), GetUint()); }

  uint4 GetUint4() { return float4(GetUint2(), GetUint2()); }

  float GetFloat() {
    uint x = GetUint();
    return _UintToFloat01(x);
  }

  float2 GetFloat2() {
    uint2 x = GetUint2();
    return _UintToFloat01(x);
  }

  float4 GetFloat4() {
    uint4 x = GetUint4();
    return _UintToFloat01(x);
  }
};
} // namespace Rng

} // namespace Math
#endif