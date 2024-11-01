#include "framework/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "framework/Common.hlsl"
#include "framework/Material.hlsl"
#include "framework/Lighting.hlsl"


struct LightingData {
    float4x4 inv_view_proj;
    uint     light_count;
    uint3    padding;
    float3   camera_position;
};

// [[vk::binding(0, 2)]] Texture2D scene_textures[25];
// [[vk::binding(0, 2)]] Texture2D scene_texture;

static const uint CUR_MATERIAL_TYPE = 0;

// [[vk::binding(0, 0)]] StructuredBuffer<MaterialData> material_data
//     : register(t0, space0);
// [[vk::binding(1, 0)]] StructuredBuffer<Light> light_data : register(t1, space0);
// [[vk::push_constant]] ConstantBuffer<LightingData> lighting_data : register(b0);

// [[vk::binding(0, 1)]] Texture2D<uint> mat_attach;
// [[vk::binding(1, 1)]] Texture2D<float4> normal_attach;
// [[vk::binding(2, 1)]] Texture2D<float2> gbuffer_uv;
// [[vk::binding(3, 1)]] Texture2D depth_attach;

// [[vk::binding(4, 1)]] SamplerState default_sampler;

struct MaterialData {
    float4 base_color_factor;
    float3 emissive_factor;
    float  metallic_factor;
    float  roughness_factor;
    float  ao;
    uint   albedo_map;
    int    normal_map;
    int    metallic_roughness_map;
    int    ao_map;
    int    emissive_map;
    int    padding;
};

struct PackedMaterialData{
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
};

[[vk::push_constant]] ConstantBuffer<Constant> param;

// [[vk::input_attachment_index(1), vk::binding(1)]] SubpassInput depthAttach;
// float3 worldPosFromDepth(float depth, float2 in_uv) {
//     float4 clip    = float4(in_uv.x * 2.f - 1.f, 1.f - in_uv.y * 2.f, depth, 1.0);
//     float4 world_w = mul(lighting_data.inv_view_proj, clip);
//     float3 pos     = world_w.xyz / world_w.w;
//     return pos;
// }
float4 main(float2 in_uv
            : TEXCOORD0, float4 position
            : SV_POSITION)
    : SV_TARGET {
    TextureHandle mat_attach = TextureHandle(param.vbuffer);

    uint         gbuffer_mat = TextureHandle(param.vbuffer).Sample2D<uint>(in_uv);
    uint         mat_type    = gbuffer_mat & 0x000000FF;
    float2       uv          = TextureHandle(param.gbuffer_uv).Sample2D<float2>(in_uv);
    uint         mat_id      = (gbuffer_mat & 0xFFFFFF00) >> 8;
    MaterialData mat         = UnpackMaterialData<MaterialData>(param.material_buffer, mat_id);
    float4       base_color;
    if (mat.albedo_map == -1) {
        base_color = mat.base_color_factor;
    } else {
        base_color = TextureHandle(mat.albedo_map).Sample2D<float4>(uv);
    }
    return float4(base_color.xyz, 1.0f);
}