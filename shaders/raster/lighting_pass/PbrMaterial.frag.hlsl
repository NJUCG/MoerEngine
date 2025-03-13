#include "framework/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "framework/Common.hlsl"
#include "framework/Lighting.hlsl"
#include "framework/Material.hlsl"

struct LightingData {
    column_major float4x4 inv_view_proj;
    uint light_count;
    uint3 padding;
    float3 camera_position;
};

static const uint CUR_MATERIAL_TYPE = 0;

struct PackedMaterialData {
    float4 packed_0;
    float4 packed_1;
    float4 packed_2;
    float4 packed_3;
    float4 packed_4;
    float4 packed_5;
    float4 packed_6;
    float4 packed_7;
};

struct Constant {
    uint material_type;
    uint light_buffer;
    uint material_buffer;
    uint vbuffer;
    uint gbuffer_normal;
    uint gbuffer_uv;
    uint gbuffer_depth;
    uint gbuffer_position;
    uint global_param_handle;
};

[[vk::push_constant]] ConstantBuffer<Constant> param;

float ndfGGX(float cosLh, float roughness) {
    float alpha = roughness * roughness;
    float alphaSq = alpha * alpha;
    float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * denom * denom);
}

float gaSchlickG1(float cosTheta, float k) {
    return cosTheta / (cosTheta * (1.0 - k) + k);
}

float gaSchlickGGX(float cosLi, float cosLo, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return gaSchlickG1(cosLi, k) * gaSchlickG1(cosLo, k);
}

float3 fresnelSchlick(float3 F0, float cosTheta) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

static const float3 Fdielectric = 0.04;
static const float Epsilon = 0.0001;
struct PBRInfo {
    float roughness;
    float3 albedo;
    float metalness;
    float3 normal;
    float3 viewDir;

    float3 Evaluate(float3 lightDir) {
        float3 F0 = lerp(Fdielectric, albedo, metalness);
        float3 halfDir = normalize(lightDir + viewDir);
        float cosLi = saturate(dot(normal, lightDir));
        float cosLh = saturate(dot(normal, halfDir));
        float cosLo = saturate(dot(normal, viewDir));
        float3 F = fresnelSchlick(F0, cosLo);
        float D = ndfGGX(cosLh, roughness);
        float G = gaSchlickGGX(cosLi, cosLo, roughness);
        float3 kd = lerp(float3(1, 1, 1) - F, float3(0, 0, 0), metalness);
        float3 diffuseBRDF = kd * albedo;
        float3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);
        return (diffuseBRDF + specularBRDF) * cosLi;
    }
};

 float3 WorldPosFromDepth(float depth, float2 screen_uv,float4x4 inv_view_proj) {
     float4 clip    = float4(screen_uv.x * 2.f - 1.f, 1.f - screen_uv.y * 2.f, depth, 1.0);
     float4 world_w = mul(inv_view_proj, clip);
     float3 pos     = world_w.xyz / world_w.w;
     return pos;
 }
float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    TextureHandle mat_attach = TextureHandle(param.vbuffer);
    uint gbuffer_mat = TextureHandle(param.vbuffer).Sample2D<uint>(in_uv);
    uint mat_type = gbuffer_mat & 0x000000FF;
    if (mat_type != param.material_type) {
        // printf("mat_type:%d, param.material_type:%d\n", mat_type, param.material_type);
        discard;
    }
    float2 uv = TextureHandle(param.gbuffer_uv).Sample2D<float2>(in_uv);
    float depth = TextureHandle(param.gbuffer_depth).Sample2D<float>(in_uv);

    // empty
    if (depth == 0.0) {
        // TODO: Image Based Lighting or Skybox
        discard;
    }

    uint mat_id = (gbuffer_mat & 0xFFFFFF00) >> 8;
    MaterialData mat = UnpackMaterialData<MaterialData>(param.material_buffer, mat_id);

    PBRInfo pbrInfo;
    float2 metallic_roughness = GetTextureData<float2>(mat.metallic_roughness_map, uv, float2(mat.metallic_factor, mat.roughness_factor));
    pbrInfo.roughness = metallic_roughness.y;
    pbrInfo.metalness = metallic_roughness.x;
    float3 packed_normal = TextureHandle(param.gbuffer_normal).Sample2D<float3>(in_uv);
    float3 normal = DeferedRendering::UnpackNormal(packed_normal);
    pbrInfo.normal = normal;

    if (mat.albedo_map == -1) {
        pbrInfo.albedo = mat.base_color_factor.xyz;
    } else if (mat.albedo_map == 0) { // use 0.0 presents missing texture
        // FIXME: use a better way to present missing texture
        // FIXME: use a MACRO to define missing texture color
        pbrInfo.albedo = float3(1.0, 0.0, 1.0);
    } else {
        pbrInfo.albedo = TextureHandle(mat.albedo_map).Sample2D<float3>(uv);
    }

    ArrayBuffer global_params = ArrayBuffer(param.global_param_handle);
    LightingData lighting_data = global_params.Load<LightingData>(0);

    //Shoude be reconstructed from depth
    float3 position = WorldPosFromDepth(depth, in_uv, lighting_data.inv_view_proj);
    // float3 position = TextureHandle(param.gbuffer_position).Sample2D<float3>(in_uv);

    pbrInfo.viewDir = normalize(lighting_data.camera_position - position.xyz);
    float3 color = float3(0, 0, 0);

    ArrayBuffer light_buffer = ArrayBuffer(param.light_buffer);

    for (uint i = 0; i < lighting_data.light_count; i++) {
        LightData light = light_buffer.Load<LightData>(i);
       // printf("light_type:%d light_color:%f %f %f position:%f %f %f\n", light.type, light.color.x, light.color.y, light.color.z, light.position.x, light.position.y, light.position.z);
        float3 lightDir = calculate_light_dir(light, position);
        color += pbrInfo.Evaluate(lightDir) * apply_light(light, position, normal);
    }
    //color = position;
    //color = pbrInfo.albedo;
    //printf("light_count:%d camera_position:%f %f %f\n", lighting_data.light_count, lighting_data.camera_position.x, lighting_data.camera_position.y, lighting_data.camera_position.z);
    return float4(color, 1.0);
}