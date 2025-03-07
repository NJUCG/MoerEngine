
namespace VertexFactory {

struct InVertexAttributes {
    
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

    uint GetNormal() { return normal; }

    float2 GetTangent() {
#ifdef HAS_TANGENT
        return tangent;
#else
        return float2(0, 0);
#endif
    }

    float2 GetTangentWithTransformedNormal(float3 transfromed_normal) {
#ifdef HAS_TANGENT
        return tangent;
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

struct OutVertexAttributes {
    float4 position : SV_POSITION;
    float3 world_positon : POSITION;
    float2 texcoord0 : TEXCOORD0;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    int instance_id : INSTANCEID;
};

OutVertexAttributes GetConvertedAttributes(InVertexAttributes input, float3x4 model2world, float4x4 world2clip, int instance_id) {
    float3x3 model = (float3x3)model2world;

    float3 world_position = mul(model2world, float4(input.GetPosition(), 1.0f)).xyz;

    float4 pos = mul(world2clip, float4(world_position, 1.0f));

    OutVertexAttributes output;
    output.position = pos;
    output.world_positon = world_position;
    output.texcoord0 = input.GetTexcoord0();
    output.normal = normalize(mul(model, Moer::Unpack_RGB8_SNORM(input.GetNormal())));
    output.tangent = normalize(mul(model, Moer::Unpack_RGB8_SNORM(input.GetTangentWithTransformedNormal(output.normal))));
    output.instance_id = instance_id;

    float2 x = input.GetTexcoord1(); // comsume texcoord1 to avoid warning

    return output;
}

} // namespace VertexFactory