#ifndef MOER_FRAMEWORK_POLYMORPHIC_LIGHT_HLSLI
#define MOER_FRAMEWORK_POLYMORPHIC_LIGHT_HLSLI

#include "MathLib/STL.hlsli"
#include "framework/Math.hlsli"
#include "shared/lighting/ShaderParameters.h"
#include "shared/utils/Packing.h"

namespace Moer {

static const float s_light_epsilon = 1e-10f;
static const float s_light_max_distance = 50000.f;

#pragma region[ light shaping ]

// Computes the conservative cone of influence for a spherical light source that
// has a shaping angle. The cone angle and axis are the same as the shaping
// angle and axis, and the cone vertex is the light center offset against the
// axis by the distance that is necessary to inscribe the sphere into the cone.
// Assumes nonzero cone angle.
float3 GetConservativeConeVertexForSphericalSource(float3 _sphere_center,
                                                   float _sphere_radius,
                                                   float3 _cone_axis,
                                                   float _cone_half_angle) {
  // Compute the sine of the clamped half angle. When the angle is more than 90
  // degrees (half a pi), the offset should be exactly one sphere radius.
  float sin_half_angle = sin(min(_cone_half_angle, PI * 0.5f));

  // Offset is the hypotenuse of a right triangle whose vertices are: the light
  // center; the cone vertex; and any point on the circle where the cone touches
  // the sphere.
  float offset = _sphere_radius / sin_half_angle;

  // Compute the cone vertex assuming that the aforementioned hypotenuse is
  // collinear with the cone axis.
  float3 vtx = _sphere_center - _cone_axis * offset;

  return vtx;
}

bool TestSphereConeIntersection(float3 _cone_vtx, float3 _axis,
                                float _cone_half_angle, float3 _center,
                                float _radius) {
  float3 l = _center - _cone_vtx;
  float dist = length(l);

  if (dist <= _radius) {
    return true;
  }

  float inv_dist = rcp(dist);
  float angle = acos(dot(l, _axis) * inv_dist);
  float half_angle = asin(_radius * inv_dist);
  return angle <= _cone_half_angle + half_angle;
}

struct LightShaping {
  float cos_cone_angle;
  float3 axis;
  float cos_cone_softness;
  uint is_spot;
  int ies_profile_idx;

  float GetFluxFactor() {

    if (is_spot) {
      float solid_angle_over_2pi = 1.0f - cos_cone_angle;
      return solid_angle_over_2pi * lerp(1.f, 0.5f, cos_cone_softness) * 0.5f;
    }
    return 1.0f;
  }

  bool TestSphereIntersection(float3 _center, float _radius,
                              float3 _sphere_center, float _sphere_radius) {
    if (!is_spot) {
      return true;
    }

    float cone_half_angle = acos(cos_cone_angle);
    float3 cone_vertex = GetConservativeConeVertexForSphericalSource(
        _center, _radius, axis, cone_half_angle);
    return TestSphereConeIntersection(cone_vertex, axis, cone_half_angle,
                                      _sphere_center, _sphere_radius);
  }
};

LightShaping UnpackLightShaping(PolymorphicLightInfo _info) {

  LightShaping shaping;
  shaping.is_spot =
      (_info.color_type_flags & g_poly_morphic_light_shaping_bit) != 0;
  shaping.axis = Math::OctToNdirUnorm32(_info.primary_axis);
  shaping.cos_cone_angle = f16tof32(_info.cos_cone_angle_softness & 0xffff);
  shaping.cos_cone_softness = f16tof32(_info.cos_cone_angle_softness >> 16);
  shaping.ies_profile_idx = -1;
  return shaping;
}

PolymorphicLightInfo EmptyLightInfo() {
  PolymorphicLightInfo info = (PolymorphicLightInfo)0;
  return info;
}

#pragma endregion

#pragma region[ PolymorphicLightInfo ]

struct PolymorphicLightSample {
  float3 pos;
  float3 normal;
  float3 radiance;
  float solid_angle_pdf;
};

EPolyLightType GetLightType(PolymorphicLightInfo _info) {
  uint type = (_info.color_type_flags >> g_poly_morphic_light_type_shift) &
              g_poly_morphic_light_type_mask;

  return (EPolyLightType)type;
}

float UnpackLightRadiance(uint _log_radiance) {
  return (_log_radiance == 0)
             ? 0.0f
             : exp2(float(_log_radiance - 1) / 65534.f *
                        (g_poly_morphic_light_max_log2_radiance -
                         g_poly_morphic_light_min_log2_radiance) +
                    g_poly_morphic_light_min_log2_radiance);
}

float3 UnpackLightColor(PolymorphicLightInfo _info) {
  float3 color = Moer::Unpack_R8G8B8_UFLOAT(_info.color_type_flags);
  float radiance = UnpackLightRadiance(_info.log_radiance &
                                       g_poly_morphic_light_log_radiance_mask);
  return color * radiance;
}

void PackLightColor(inout PolymorphicLightInfo _info, float3 _radiance) {
  float intensity = max(max(_radiance.x, _radiance.y), _radiance.z);
  if (intensity > 0.0f) {
    float log_radiance =
        saturate((log2(intensity) - g_poly_morphic_light_min_log2_radiance) /
                 (g_poly_morphic_light_max_log2_radiance -
                  g_poly_morphic_light_min_log2_radiance));
    uint packed_radiance = min(uint(ceil(log_radiance * 65534.f)) + 1, 0xffff);

    float unpacked_radiance = UnpackLightRadiance(packed_radiance);
    float3 normalized_radiance = saturate(_radiance / unpacked_radiance);

    _info.log_radiance |= packed_radiance;
    // printf("normalized_radiance %f %f %f \n", normalized_radiance.x,
    // normalized_radiance.y, normalized_radiance.z);
    _info.color_type_flags |= Moer::Pack_R8G8B8_UFLOAT(normalized_radiance);
  }
}

bool PackCompactLightInfo(PolymorphicLightInfo _info, out uint4 _res1,
                          out uint4 _res2) {
  if (UnpackLightShaping(_info).is_spot) {
    _res1 = 0;
    _res2 = 0;
    return false;
  }
  _res1.xyz = asuint(_info.center.xyz);
  _res1.w = _info.color_type_flags;

  _res2.x = _info.direction1;
  _res2.y = _info.direction2;
  _res2.z = _info.scalars;
  _res2.w = _info.log_radiance;
  return true;
}

PolymorphicLightInfo UnpackCompactLightInfo(uint4 _res1, uint4 _res2) {
  PolymorphicLightInfo info;
  info.center.xyz = asfloat(_res1.xyz);
  info.color_type_flags = _res1.w;
  info.direction1 = _res2.x;
  info.direction2 = _res2.y;
  info.scalars = _res2.z;
  info.log_radiance = _res2.w;
  return info;
}

float AvgDistanceToVolume(float _center_dist, float _radius) {
  const float factor = 1.1547;
  return _center_dist +
         _radius * square(factor) / square(_center_dist + factor * _radius);
}

struct SphereLight {
  float3 pos;
  float radius;
  float3 radiance;
  LightShaping shaping;

  PolymorphicLightSample Sample(in const float2 _rnd, in const float3 _vp) {
    PolymorphicLightSample ls;

    const float3 l2p = pos - _vp;
    const float dist2 = dot(l2p, l2p);
    const float dist = sqrt(dist2);
    const float radius2 = square(radius);

    // PBRT's solid angle sphere sampling
    const float2 u = _rnd;
    const float sin_theta_max2 = radius2 / dist2;
    const float cos_theta_max = sqrt(max(0.0f, 1.0f - sin_theta_max2));
    const float cos_theta = lerp(1.0f, cos_theta_max, u.x);
    const float sin_theta = sqrt(max(0.0f, 1.0f - cos_theta * cos_theta));
    const float sin_theta2 = square(sin_theta);
    const float phi = 2.0f * PI * u.y;

    const float ds = dist * cos_theta -
                     sqrt(max(s_light_epsilon, radius2 - dist2 * sin_theta2));
    const float cos_alpha =
        (dist2 + radius2 - square(ds)) / (2.0f * dist * radius);
    const float sin_alpha = sqrt(max(0.0f, 1.0f - cos_alpha * cos_alpha));

    const float3 n = normalize(l2p);
    float3 t;
    float3 b;
    Math::BranchlessONB(n, t, b);

    float cos_phi, sin_phi;
    sincos(phi, sin_phi, cos_phi);

    const float3 radius_vec = Math::SphericalDirection(
        -t, -b, -n, sin_alpha, cos_alpha, sin_phi, cos_phi);

    const float3 sphere_pos = pos + radius_vec * radius;
    const float3 sphere_normal = normalize(radius_vec);

    const float solid_angle_pdf = 1.0f / (2.0f * PI * (1.0f - cos_theta_max));

    ls.pos = sphere_pos;
    ls.normal = sphere_normal;
    ls.radiance = radiance;
    ls.solid_angle_pdf = solid_angle_pdf;

    return ls;
  }

  float GetSurfaceArea() { return 4.0f * PI * square(radius); }

  float GetPower() {
    return GetSurfaceArea() * PI * STL::Color::Luminance(radiance) *
           shaping.GetFluxFactor();
  }

  float GetVolumeWeight(in const float3 _center, in const float _radius) {
    if (!shaping.TestSphereIntersection(pos, radius, _center, _radius)) {
      return 0.0f;
    }
    float dist = length(pos - _center);
    dist = AvgDistanceToVolume(dist, _radius);

    float sin_half_angle = radius / dist;
    float solid_angle =
        2.0f * PI * (1.0f - sqrt(1.0f - sin_half_angle * sin_half_angle));
    return solid_angle * STL::Color::Luminance(radiance);
  }

  static SphereLight Create(in const PolymorphicLightInfo _info) {
    SphereLight sl;
    sl.pos = _info.center.xyz;
    sl.radius = f16tof32(_info.scalars & 0xffff);
    sl.radiance = UnpackLightColor(_info);
    sl.shaping = UnpackLightShaping(_info);
    return sl;
  }
};

struct PointLight {
  float3 pos;
  float3 flux;
  LightShaping shaping;

  PolymorphicLightSample Sample(in const float2 _rnd, in const float3 _vp) {
    PolymorphicLightSample ls;
    const float3 l2p = pos - _vp;

    ls.pos = pos;
    ls.normal = normalize(-l2p);
    ls.radiance = flux / dot(l2p, l2p);
    ls.solid_angle_pdf = 1.0f;

    return ls;
  }

  float GetPower() {
    return 4.f * PI * STL::Color::Luminance(flux) * shaping.GetFluxFactor();
  }

  float GetVolumeWeight(in const float3 _center, in const float _radius) {
    if (!shaping.TestSphereIntersection(pos, 0, _center, _radius)) {
      return 0.0f;
    }
    float dist = length(pos - _center);
    dist = AvgDistanceToVolume(dist, _radius);
    return STL::Color::Luminance(flux) / square(dist);
  }

  static PointLight Create(in const PolymorphicLightInfo _info) {
    PointLight pl;
    pl.pos = _info.center.xyz;
    pl.flux = UnpackLightColor(_info);
    pl.shaping = UnpackLightShaping(_info);
    return pl;
  }
};

struct CylinderLight {
  float3 position;
  float radius;
  float3 radiance;
  float self_length;
  float3 tangent;

  float GetSurfaceArea() { return 2.0f * PI * radius * self_length; }

  float GetPower() {
    return GetSurfaceArea() * STL::Color::Luminance(radiance) * PI;
  }
  PolymorphicLightSample Sample(in const float2 _rnd, in const float3 _vp) {
    PolymorphicLightSample ls;

    float3 n;
    float3 b;
    Math::BranchlessONB(tangent, n, b);

    const float phi = 2.0f * PI * _rnd.x;

    float sin_phi, cos_phi;
    sincos(phi, sin_phi, cos_phi);

    const float z = (_rnd.y - 0.5f) * self_length; // random along the length

    const float3 radius_vec = cos_phi * n + sin_phi * b;
    const float3 pos = position + z * tangent + radius_vec * radius;
    const float3 normal = normalize(radius_vec);
    // reproject for position to minimize error

    const float area_pdf = 1.0f / GetSurfaceArea();
    const float3 sample_vec = pos - _vp;
    const float sample_dist = length(sample_vec);
    const float sample_cos_theta = dot(normalize(sample_vec), -normal);
    const float solid_angle_pdf =
        area_pdf * square(sample_dist) / sample_cos_theta;

    ls.pos = pos;
    ls.normal = normal;
    if (sample_cos_theta > 0.0f) {
      ls.radiance = radiance;
      ls.solid_angle_pdf = solid_angle_pdf;
    } else {
      ls.radiance = 0.f;
      ls.solid_angle_pdf = 0.0f;
    }
    return ls;
  }

  float GetVolumeWeight(in const float3 _center, in const float _radius) {
    float dist = length(position - _center);
    dist = AvgDistanceToVolume(dist, _radius);

    float quad_area = 2.f * radius * self_length;
    float approx_solid_angle = quad_area / square(dist);
    approx_solid_angle = min(approx_solid_angle, 2 * PI);

    return approx_solid_angle * STL::Color::Luminance(radiance);
  }

  static CylinderLight Create(in const PolymorphicLightInfo _info) {
    CylinderLight cl;
    cl.position = _info.center.xyz;
    cl.radius = f16tof32(_info.scalars & 0xffff);
    cl.radiance = UnpackLightColor(_info);
    cl.self_length = f16tof32(_info.scalars >> 16);
    cl.tangent = Math::OctToNdirUnorm32(_info.direction1);

    return cl;
  }
};

struct DiskLight {
  float3 position;
  float radius;
  float3 radiance;
  float3 normal;

  float GetSurfaceArea() { return PI * square(radius); }

  float GetPower() {
    return GetSurfaceArea() * STL::Color::Luminance(radiance) * PI;
  }

  PolymorphicLightSample Sample(in const float2 _rnd, in const float3 _vp) {
    PolymorphicLightSample ls;

    float3 t, b;
    Math::BranchlessONB(normal, t, b);

    const float2 raw_disk_sample = Math::SampleDisk(_rnd) * radius;
    const float3 sample_pos =
        position + t * raw_disk_sample.x + b * raw_disk_sample.y;
    const float3 sample_normal = normal;

    const float area_pdf = 1.0f / GetSurfaceArea();
    const float3 sample_vec = sample_pos - _vp;
    const float sample_dist = length(sample_vec);
    const float sample_cos_theta = dot(normalize(sample_vec), -sample_normal);

    const float solid_angle_pdf =
        area_pdf * square(sample_dist) / sample_cos_theta;

    ls.pos = sample_pos;
    ls.normal = sample_normal;
    if (sample_cos_theta > 0.0f) {
      ls.radiance = radiance;
      ls.solid_angle_pdf = solid_angle_pdf;
    } else {
      ls.radiance = 0.0f;
      ls.solid_angle_pdf = 0.0f;
    }
    return ls;
  }

  float GetVolumeWeight(in const float3 _center, in const float _radius) {
    float dist = dot(_center - position, normal);

    if (dist < -_radius) {
      return 0.0f;
    }

    dist = length(_center - position);
    dist = AvgDistanceToVolume(dist, _radius);

    float approx_solid_angle = GetSurfaceArea() / square(dist);
    approx_solid_angle = min(approx_solid_angle, 2 * PI);

    return approx_solid_angle * STL::Color::Luminance(radiance);
  }

  static DiskLight Create(in const PolymorphicLightInfo _info) {
    DiskLight dl;
    dl.position = _info.center.xyz;
    dl.radius = f16tof32(_info.scalars & 0xffff);
    dl.radiance = UnpackLightColor(_info);
    dl.normal = Math::OctToNdirUnorm32(_info.direction1);

    return dl;
  }
};

struct RectLight {
  float3 position;
  float2 dimensions;
  float3 dir_x;
  float3 dir_y;
  float3 radiance;
  float3 normal;

  float GetSurfaceArea() { return dimensions.x * dimensions.y; }

  float GetPower() {
    return GetSurfaceArea() * STL::Color::Luminance(radiance) * PI;
  }

  PolymorphicLightSample Sample(in const float2 _rnd, in const float3 _vp) {
    PolymorphicLightSample ls;

    const float2 raw_rect_sample = (_rnd - 0.5f) * dimensions;
    const float3 sample_pos =
        position + dir_x * raw_rect_sample.x + dir_y * raw_rect_sample.y;
    const float3 sample_normal = normal;

    const float area_pdf = 1.0f / GetSurfaceArea();
    const float3 sample_vec = sample_pos - _vp;
    const float sample_dist = length(sample_vec);
    const float sample_cos_theta = dot(normalize(sample_vec), -sample_normal);

    const float solid_angle_pdf =
        area_pdf * square(sample_dist) / sample_cos_theta;

    ls.pos = sample_pos;
    ls.normal = sample_normal;
    if (sample_cos_theta > 0.0f) {
      ls.radiance = radiance;
      ls.solid_angle_pdf = solid_angle_pdf;
    } else {
      ls.radiance = 0.0f;
      ls.solid_angle_pdf = 0.0f;
    }
    return ls;
  }

  float GetVolumeWeight(in const float3 _center, in const float _radius) {
    float dist = dot(_center - position, normal);

    if (dist < -_radius) {
      return 0.0f;
    }

    dist = length(_center - position);
    dist = AvgDistanceToVolume(dist, _radius);

    float approx_solid_angle = GetSurfaceArea() / square(dist);
    approx_solid_angle = min(approx_solid_angle, 2 * PI);
    return approx_solid_angle * STL::Color::Luminance(radiance);
  }

  static RectLight Create(in const PolymorphicLightInfo _info) {
    RectLight rl;
    rl.position = _info.center.xyz;
    rl.dimensions =
        float2(f16tof32(_info.scalars & 0xffff), f16tof32(_info.scalars >> 16));
    rl.radiance = UnpackLightColor(_info);
    rl.dir_x = Math::OctToNdirUnorm32(_info.direction1);
    rl.dir_y = Math::OctToNdirUnorm32(_info.direction2);
    rl.normal = cross(rl.dir_x, rl.dir_y);

    return rl;
  }
};

struct DirectionalLight {
  float3 direction;
  float cos_half_angle; // cosine of half angle, computed when created
  float sin_half_angle; // sine of half angle, computed when created
  float solid_angle;
  float3 radiance;

  PolymorphicLightSample Sample(in const float2 _rnd, in const float3 _vp) {
    PolymorphicLightSample ls;

    // sample as from a far disk
    const float2 disk_sample = Math::SampleDisk(_rnd);
    float3 t, b;
    Math::BranchlessONB(direction, t, b);
    const float3 disk_dir = direction + t * disk_sample.x * sin_half_angle +
                            b * disk_sample.y * sin_half_angle;

    const float3 sample_pos = _vp - disk_dir * s_light_max_distance * 0.6f;
    const float3 sample_normal = disk_dir;

    ls.pos = sample_pos;
    ls.normal = sample_normal;
    ls.radiance = radiance;
    ls.solid_angle_pdf = 1.f / solid_angle;

    return ls;
  }

  static DirectionalLight Create(in const PolymorphicLightInfo _info) {
    DirectionalLight dl;
    dl.direction = Math::OctToNdirUnorm32(_info.direction1);
    float half_angle = f16tof32(_info.scalars & 0xffff);
    sincos(half_angle, dl.sin_half_angle, dl.cos_half_angle);
    dl.solid_angle = f16tof32(_info.scalars >> 16);
    dl.radiance = UnpackLightColor(_info);

    return dl;
  }
};

struct TriangleLight {
  float3 v0;
  float3 edge1;
  float3 edge2;
  float3 radiance;
  float3 normal;
  float area; // precomputed area

  PolymorphicLightSample Sample(in const float2 _rnd, in const float3 _vp) {
    PolymorphicLightSample ls;

    const float3 bary = Math::SampleTriangle(_rnd);
    const float3 sample_pos = v0 + edge1 * bary.y + edge2 * bary.z;
    const float3 sample_normal = normal;

    ls.solid_angle_pdf = SolidAnglePdf(_vp, sample_pos, sample_normal);
    ls.pos = sample_pos;
    ls.normal = sample_normal;
    ls.radiance = radiance;

    return ls;
  }

  float SolidAnglePdf(in const float3 _vp, in const float3 _sample_pos,
                      in const float3 _sample_normal) {
    float3 l2p = _sample_pos - _vp;
    const float dist = length(l2p);
    l2p /= dist;

    const float area_pdf = 1.0f / area;
    const float cos_theta = saturate(dot(l2p, -_sample_normal));

    return area_pdf * square(dist) / cos_theta;
  }

  float GetPower() { return area * STL::Color::Luminance(radiance) * PI; }

  float GetVolumeWeight(in const float3 _center, in const float _radius) {
    float dist = dot(_center - v0, normal);
    if (dist < -_radius) {
      return 0.0f;
    }
    float3 bary_center = v0 + (edge1 + edge2) / 3.0f;

    dist = length(bary_center - _center);
    dist = AvgDistanceToVolume(dist, _radius);

    float approx_solid_angle = area / square(dist);
    approx_solid_angle = min(approx_solid_angle, 2 * PI);
    return approx_solid_angle * STL::Color::Luminance(radiance);
  }

  static TriangleLight Create(in const PolymorphicLightInfo _info) {
    TriangleLight tl;
    tl.v0 = _info.center.xyz;
    tl.edge1 = Math::OctToNdirUnorm32(_info.direction1) *
               f16tof32(_info.scalars & 0xffff);
    tl.edge2 = Math::OctToNdirUnorm32(_info.direction2) *
               f16tof32(_info.scalars >> 16);
    tl.radiance = UnpackLightColor(_info);
    tl.normal = cross(tl.edge1, tl.edge2);

    float normal_length = length(tl.normal);
    if (normal_length > 0.0f) {
      tl.area = 0.5f * normal_length;
      tl.normal /= normal_length;
    } else {
      tl.area = 0.0f;
      tl.normal = 0.0f;
    }
    return tl;
  }

  PolymorphicLightInfo ToLightInfo() {
    PolymorphicLightInfo info = (PolymorphicLightInfo)0;
    PackLightColor(info, radiance);
    info.center.xyz = v0 + (edge1 + edge2) / 3.0f;
    info.direction1 = Math::NdirToOctUnorm32(normalize(edge1));
    info.direction2 = Math::NdirToOctUnorm32(normalize(edge2));
    info.color_type_flags |= uint(EPolyLightType::ELTriangle)
                             << g_poly_morphic_light_type_shift;
    info.scalars = f32tof16(length(edge1)) | (f32tof16(length(edge2)) << 16);
    return info;
  }
};

struct EnvironmentLight {
  int tex_handle;
  bool b_importance_sampled;
  float3 radiance_scale;
  float rotation;
  uint2 texture_size;

  PolymorphicLightSample Sample(in const float2 _rnd, in const float3 _vp) {
    PolymorphicLightSample ls;

    float2 uv;
    float3 dir;

    if (b_importance_sampled) {
      float2 dir_uv = _rnd;
      dir_uv.x += rotation;

      float cos_phi;
      dir = Math::EquirectangularUVToDir(dir_uv, cos_phi);
      ls.solid_angle_pdf =
          (texture_size.x * texture_size.y) / (2.0f * PI * PI * cos_phi);
      uv = _rnd;
    } else {
      dir = Math::SampleSphere(_rnd, ls.solid_angle_pdf);
      uv = Math::DirToEquirectangularUV(dir);
      uv.x -= rotation;
    }

    float3 radiance = radiance_scale;
    if (tex_handle >= 0) {
      TextureHandle tex = TextureHandle(tex_handle);
      radiance *= tex.SampleLevel<float4>(uv, 0).rgb;
    }

    float radiance_sum = radiance.r + radiance.g + radiance.b;
    if (isinf(radiance_sum) || isnan(radiance_sum)) {
      radiance = 0.0f;
    }

    ls.pos = _vp + dir * s_light_max_distance;
    ls.normal = -dir;
    ls.radiance = radiance;

    return ls;
  }

  static EnvironmentLight Create(in const PolymorphicLightInfo _info) {
    EnvironmentLight el;
    el.tex_handle = int(_info.direction1);
    el.b_importance_sampled = ((_info.scalars >> 16) != 0);
    el.radiance_scale = UnpackLightColor(_info);
    el.rotation = f16tof32(_info.scalars & 0xffff);
    el.texture_size = uint2(_info.direction2 & 0xffff, _info.direction2 >> 16);

    return el;
  }
};

struct PolymorphicLight {
  static PolymorphicLightSample Sample(in const PolymorphicLightInfo _info,
                                       in const float2 _rnd,
                                       in const float3 _vp) {
    EPolyLightType type = GetLightType(_info);
    switch (type) {
    case EPolyLightType::ELSphere:
      return SphereLight::Create(_info).Sample(_rnd, _vp);
    case EPolyLightType::ELPoint:
      return PointLight::Create(_info).Sample(_rnd, _vp);
    case EPolyLightType::ELCylinder:
      return CylinderLight::Create(_info).Sample(_rnd, _vp);
    case EPolyLightType::ELDisk:
      return DiskLight::Create(_info).Sample(_rnd, _vp);
    case EPolyLightType::ELRect:
      return RectLight::Create(_info).Sample(_rnd, _vp);
    case EPolyLightType::ELDirectional:
      return DirectionalLight::Create(_info).Sample(_rnd, _vp);
    case EPolyLightType::ELTriangle:
      return TriangleLight::Create(_info).Sample(_rnd, _vp);
    case EPolyLightType::ELEnv:
      return EnvironmentLight::Create(_info).Sample(_rnd, _vp);
    default:
      PolymorphicLightSample ls;
      ls.radiance = 0.0f;
      return ls;
    }
  }

  static float GetPower(in const PolymorphicLightInfo _info) {
    EPolyLightType type = GetLightType(_info);
    switch (type) {
    case EPolyLightType::ELSphere:
      return SphereLight::Create(_info).GetPower();
    case EPolyLightType::ELPoint:
      return PointLight::Create(_info).GetPower();
    case EPolyLightType::ELCylinder:
      return CylinderLight::Create(_info).GetPower();
    case EPolyLightType::ELDisk:
      return DiskLight::Create(_info).GetPower();
    case EPolyLightType::ELRect:
      return RectLight::Create(_info).GetPower();
    case EPolyLightType::ELDirectional:
      return 0.f;
    case EPolyLightType::ELTriangle:
      return TriangleLight::Create(_info).GetPower();
    case EPolyLightType::ELEnv:
      return 0.f;
    default:
      return 0.0f;
    }
  }

  static float GetVolumeWeight(in const PolymorphicLightInfo _info,
                               in const float3 _center,
                               in const float _radius) {
    EPolyLightType type = GetLightType(_info);
    switch (type) {
    case EPolyLightType::ELSphere:
      return SphereLight::Create(_info).GetVolumeWeight(_center, _radius);
    case EPolyLightType::ELPoint:
      return PointLight::Create(_info).GetVolumeWeight(_center, _radius);
    case EPolyLightType::ELCylinder:
      return CylinderLight::Create(_info).GetVolumeWeight(_center, _radius);
    case EPolyLightType::ELDisk:
      return DiskLight::Create(_info).GetVolumeWeight(_center, _radius);
    case EPolyLightType::ELRect:
      return RectLight::Create(_info).GetVolumeWeight(_center, _radius);
    case EPolyLightType::ELDirectional:
      return 0.f;
    case EPolyLightType::ELTriangle:
      return TriangleLight::Create(_info).GetVolumeWeight(_center, _radius);
    case EPolyLightType::ELEnv:
      return 0.f;
    default:
      return 0.0f;
    }
  }
};

#pragma endregion
} // namespace Moer
#endif // MOER_FRAMEWORK_POLYMORPHIC_LIGHT_HLSLI