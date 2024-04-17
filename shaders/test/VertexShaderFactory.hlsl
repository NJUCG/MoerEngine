#include "framework/Common.hlsl"

struct InVertexAttributes {
  float3 position : POSITION;
  float3 normal : NORMAL;
#ifdef TAGENT_ATTRIBUTE
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
#endif
  float2 uv : TEXCOORD0;
#ifdef INSTANCE_ATTRIBUTE_ID
  uint instance_id : INSTANCE_ID;
#endif

  float3 GetPosition(in InVertexAttributes attribs) { return attribs.position; }

  float3 GetNormal(in InVertexAttributes attribs) { return attribs.normal; }

  float2 GetUV(in InVertexAttributes attribs) { return attribs.uv; }

  float3 GetTangent(in InVertexAttributes attribs) {
#ifdef TAGENT_ATTRIBUTE

    return attribs.tangent;
#else
    return float3(0, 0, 0);
#endif
  }

  float3 GetBinormal(in InVertexAttributes attribs) {
#ifdef TAGENT_ATTRIBUTE

    return attribs.binormal;
#else
    return float3(0, 0, 0);
#endif
  }

  uint GetInstanceID(in InVertexAttributes attribs) {
#ifdef INSTANCE_ATTRIBUTE_ID
    return attribs.instance_id;
#else
    return 0;
#endif
  }
};

struct OutVertexAttributes {
  float4 position : SV_POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
  float2 uv : TEXCOORD0;
  uint instance_id : INSTANCE_ID;
  uint iid : SV_INSTANCEID;
};