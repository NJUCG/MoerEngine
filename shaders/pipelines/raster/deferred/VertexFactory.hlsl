
#include "shared/utils/Packing.h"

namespace VertexFactory {

struct VsInput {

  // MARK: Attributes

  float3 position : POSITION;

  uint normal : NORMAL;

#ifdef HAS_TANGENT
  uint tangent : TANGENT;
#endif

#ifdef HAS_TEXCOORD0
  float2 texcoord0 : TEXCOORD0;
#endif

#ifdef HAS_TEXCOORD1
  float2 texcoord1 : TEXCOORD1;
#endif

  // MARK: Getter

  float3 GetPosition() { return position; }

  float3 GetNormal() { return Moer::Unpack_Normal(normal); }

  float3 GetTangent() {
#ifdef HAS_TANGENT
    return Moer::Unpack_Normal(tangent);
#else
    return float3(0, 0, 0);
#endif
  }

  float3 GetTangentWithTransformedNormal(float3 transfromed_normal) {
#ifdef HAS_TANGENT
    return Moer::Unpack_Normal(tangent);
#else
    float3 tangent = cross(transfromed_normal, float3(0, 0, 1));
    if (length(tangent) < 1e-2) {
      tangent = cross(transfromed_normal, float3(0, 1, 0));
    }
    return normalize(tangent);
#endif
  }

  float2 GetTexcoord0() {
#ifdef HAS_TEXCOORD0
    return texcoord0;
#else
    return float2(0, 0);
#endif
  }

  float2 GetTexcoord1() {
#ifdef HAS_TEXCOORD1
    return texcoord1;
#else
    return float2(0, 0);
#endif
  }
};

#if SHADOW_DEPTH_PASS

struct VsOutput {
  float4 position : SV_POSITION;
};

VsOutput GetConvertedAttributes(VsInput input, float3x4 model2world,
                                float4x4 world2clip, int instance_id) {
  float3x3 model = (float3x3)model2world;

  float3 world_position = mul(model2world, float4(input.GetPosition(), 1.0));
  float4 pos = mul(world2clip, float4(world_position, 1.0));

  VsOutput output;
  output.position = pos;

  return output;
}

#else

struct VsOutput {
  float4 position : SV_POSITION;
  float3 world_position : POSITION;
  float2 texcoord0 : TEXCOORD0;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  int instance_id : INSTANCEID;
};

VsOutput GetConvertedAttributes(VsInput input, float3x4 model2world,
                                float4x4 world2clip, int instance_id) {
  float3x3 model = (float3x3)model2world;

  float3 world_position = mul(model2world, float4(input.GetPosition(), 1.0));
  float4 pos = mul(world2clip, float4(world_position, 1.0));

  float3 normal = input.GetNormal();
  float3 tangent = input.GetTangentWithTransformedNormal(normal);

  // FIXME: use NormalMatrix to transform normal

  VsOutput output;
  output.position = pos;
  output.world_position = world_position;
  output.texcoord0 = input.GetTexcoord0();
  output.normal = normalize(mul(model, normal));
  output.tangent = normalize(mul(model, tangent));
  output.instance_id = instance_id;

#ifdef HAS_TEXCOORD1
  float2 x = input.GetTexcoord1(); // comsume texcoord1 to avoid warning
#endif

  return output;
}

#endif

} // namespace VertexFactory